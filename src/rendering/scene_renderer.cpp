#include "rendering/scene_renderer.h"
#include "rendering/traffic_lights_draw.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <cmath>

#include "diagnostics/log.h"

namespace mc {

namespace {
// The backdrop is a dimmed (optionally blurred) wallpaper: half resolution is enough and saves 75 %
// of its GPU memory. Labels, strip and focus chrome are full resolution.
constexpr float kBackdropScale = 0.5f;

D2D1_COLOR_F white(float a) { return D2D1::ColorF(1.f, 1.f, 1.f, a); }
D2D1_RECT_F toD2D(const Rect& r) { return D2D1::RectF(float(r.x), float(r.y), float(r.right()), float(r.bottom())); }
void setOpacity(IDCompositionVisual3* v, double a) {
    if (v) v->SetOpacity(float(std::clamp(a, 0.0, 1.0)));
}
}  // namespace

bool SceneRenderer::init(HWND overlay, int width, int height) {
    reset();
    overlay_ = overlay;
    (void)width;
    (void)height;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    ComPtr<IDXGIDevice> dxgi;
    if (!log::check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                      D3D11_SDK_VERSION, &d3d_, nullptr, nullptr), "D3D11CreateDevice") ||
        !log::check(d3d_.As(&dxgi), "QueryInterface(IDXGIDevice)") ||
        !log::check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                      reinterpret_cast<void**>(d2dFactory_.GetAddressOf())), "D2D1CreateFactory") ||
        !log::check(d2dFactory_->CreateDevice(dxgi.Get(), &d2d_), "ID2D1Factory1::CreateDevice") ||
        !log::check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())),
                    "DWriteCreateFactory") ||
        !log::check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_)), "CoCreateInstance(WIC)") ||
        !log::check(DCompositionCreateDevice2(d2d_.Get(), IID_PPV_ARGS(&device_)), "DCompositionCreateDevice2") ||
        !log::check(device_->CreateTargetForHwnd(overlay, TRUE, &target_), "CreateTargetForHwnd") ||
        !log::check(device_->CreateVisual(&root_), "CreateVisual")) {
        reset();
        return false;
    }
    // Bottom to top. DWM thumbnails are drawn above all of it.
    backdropLayer_ = visual(root_.Get());
    focusLayer_ = visual(root_.Get());
    labelLayer_ = visual(root_.Get());
    trafficLayer_ = visual(root_.Get());
    stripLayer_ = visual(root_.Get());
    if (!backdropLayer_ || !focusLayer_ || !labelLayer_ || !trafficLayer_ || !stripLayer_) { reset(); return false; }
    target_->SetRoot(root_.Get());
    placeBackdrop(0, 0, 0);
    placeStrip(0, 0);
    if (!commit()) { reset(); return false; }
    log::write("scene renderer ready");
    return true;
}

void SceneRenderer::reset() {
    backdrops_.clear();
    labelFormat_.Reset();
    focus_.Reset(); focus3_.Reset(); focusRaster_ = {};
    traffic_.Reset(); traffic3_.Reset(); trafficRaster_ = {}; trafficHot_ = -2;
    stripVisual_.Reset(); stripVisual3_.Reset(); stripRing_.Reset(); strip_ = {};
    backdropLayer_.Reset(); focusLayer_.Reset(); labelLayer_.Reset(); trafficLayer_.Reset(); stripLayer_.Reset(); root_.Reset();
    target_.Reset(); device_.Reset();
    wic_.Reset(); dwrite_.Reset(); d2d_.Reset(); d2dFactory_.Reset(); d3d_.Reset();
}

ComPtr<IDCompositionVisual2> SceneRenderer::visual(IDCompositionVisual2* parent) {
    ComPtr<IDCompositionVisual2> v;
    if (!log::check(device_->CreateVisual(&v), "CreateVisual")) return nullptr;
    // With a null reference, insertAbove = FALSE puts the visual on top of its siblings (TRUE = bottom),
    // verified by screenshot: the first build with TRUE stacked the backdrop over everything.
    if (parent && !log::check(parent->AddVisual(v.Get(), FALSE, nullptr), "AddVisual")) return nullptr;
    return v;
}

ComPtr<IDCompositionSurface> SceneRenderer::beginSurface(UINT w, UINT h, ComPtr<ID2D1DeviceContext>& dc) {
    ComPtr<IDCompositionSurface> s;
    POINT offset{};
    w = std::max(w, 1u);
    h = std::max(h, 1u);
    if (!log::check(device_->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &s), "CreateSurface") ||
        !log::check(s->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(dc.GetAddressOf()), &offset), "BeginDraw"))
        return nullptr;
    dc->SetDpi(96.f, 96.f);  // draw in physical pixels
    dc->SetTransform(D2D1::Matrix3x2F::Translation(float(offset.x), float(offset.y)));
    // The surface can live in a shared atlas: clear and draw only inside our rectangle.
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, float(w), float(h)), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return s;
}

bool SceneRenderer::endSurface(IDCompositionSurface* s) { return log::check(s->EndDraw(), "EndDraw"); }

ComPtr<ID2D1Bitmap1> SceneRenderer::loadImage(const std::wstring& path, UINT w, UINT h, bool cover) {
    if (path.empty() || !w || !h) return nullptr;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(wic_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame))) {
        log::write("wallpaper: cannot decode %ls", path.c_str());
        return nullptr;
    }
    UINT iw = 0, ih = 0;
    frame->GetSize(&iw, &ih);
    if (!iw || !ih) return nullptr;
    // Cover: scale so the image fills w x h, then crop the center (the "Fill" wallpaper position).
    const double s = cover ? std::max(double(w) / iw, double(h) / ih) : std::min(double(w) / iw, double(h) / ih);
    const UINT sw = std::max(1u, UINT(std::lround(iw * s))), sh = std::max(1u, UINT(std::lround(ih * s)));
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic_->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant))) return nullptr;
    IWICBitmapSource* src = scaler.Get();
    if (cover && (sw > w || sh > h)) {
        const WICRect crop{INT((sw - std::min(sw, w)) / 2), INT((sh - std::min(sh, h)) / 2), INT(std::min(sw, w)), INT(std::min(sh, h))};
        if (FAILED(wic_->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(scaler.Get(), &crop))) return nullptr;
        src = clipper.Get();
    }
    if (FAILED(wic_->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return nullptr;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bmp;
    if (FAILED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) || FAILED(dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp)))
        return nullptr;
    return bmp;
}

ComPtr<ID2D1Bitmap1> SceneRenderer::iconBitmap(HICON icon) {
    if (!icon) return nullptr;
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> bmp;
    // The HICON belongs to another process and may be destroyed at any time: failure just means no icon.
    if (FAILED(wic_->CreateBitmapFromHICON(icon, &wb)) || FAILED(wic_->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)) || FAILED(dc->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp)))
        return nullptr;
    return bmp;
}

// ---------- backdrop ----------

void SceneRenderer::setBackdrops(const std::vector<BackdropSpec>& specs, const BackdropStyle& style) {
    if (!device_) return;
    bool same = style == style_ && specs.size() == backdrops_.size();
    for (std::size_t i = 0; same && i < specs.size(); ++i) same = specs[i] == backdrops_[i].spec;
    if (same) return;
    for (auto& b : backdrops_) backdropLayer_->RemoveVisual(b.container.Get());
    backdrops_.clear();
    style_ = style;

    for (const auto& spec : specs) {
        Backdrop b;
        b.spec = spec;
        b.overscan = float(spec.monitor.w * style.parallax);
        const UINT innerW = UINT(std::ceil(spec.monitor.w * kBackdropScale)), h = UINT(std::ceil(spec.monitor.h * kBackdropScale));
        const UINT over = UINT(std::ceil(b.overscan * kBackdropScale)), w = innerW + 2 * over;

        // Compose wallpaper (+ mirrored copies in the overscan so a parallax shift never shows an edge).
        ComPtr<ID2D1DeviceContext> tmpDc;
        ComPtr<ID2D1Bitmap1> composed;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tmpDc)) ||
            FAILED(tmpDc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &composed)))
            continue;
        tmpDc->SetDpi(96.f, 96.f);
        tmpDc->SetTarget(composed.Get());
        tmpDc->BeginDraw();
        tmpDc->Clear(D2D1::ColorF(GetRValue(spec.color) / 255.f, GetGValue(spec.color) / 255.f, GetBValue(spec.color) / 255.f, 1.f));
        if (auto img = loadImage(spec.wallpaper, innerW, h, true)) {
            const D2D1_SIZE_F sz = img->GetSize();
            const float x0 = float(over);
            tmpDc->DrawBitmap(img.Get(), D2D1::RectF(x0, 0, x0 + sz.width, sz.height));
            // Mirror at both sides: x' = 2*edge - x.
            tmpDc->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1) * D2D1::Matrix3x2F::Translation(2 * x0, 0));
            tmpDc->DrawBitmap(img.Get(), D2D1::RectF(x0, 0, x0 + sz.width, sz.height));
            const float x1 = x0 + sz.width;
            tmpDc->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1) * D2D1::Matrix3x2F::Translation(2 * x1, 0));
            tmpDc->DrawBitmap(img.Get(), D2D1::RectF(x0, 0, x0 + sz.width, sz.height));
            tmpDc->SetTransform(D2D1::Matrix3x2F::Identity());
        }
        tmpDc->EndDraw();
        tmpDc->SetTarget(nullptr);

        ComPtr<ID2D1DeviceContext> dc;
        b.surface = beginSurface(w, h, dc);
        if (!b.surface) continue;
        if (style.blur && style.blurIntensity > 0) {
            ComPtr<ID2D1Effect> blur;
            if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur))) {
                blur->SetInput(0, composed.Get());
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, float(2.0 + 28.0 * style.blurIntensity) * kBackdropScale);
                blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
                dc->DrawImage(blur.Get());
            }
        } else {
            dc->DrawBitmap(composed.Get());
        }
        ComPtr<ID2D1SolidColorBrush> black;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, float(style.dim)), &black);
        dc->FillRectangle(D2D1::RectF(0, 0, float(w), float(h)), black.Get());
        dc->PopAxisAlignedClip();
        if (!endSurface(b.surface.Get())) continue;

        b.container = visual(backdropLayer_.Get());
        b.outgoing = visual(b.container.Get());
        b.incoming = visual(b.container.Get());
        if (!b.container || !b.outgoing || !b.incoming) continue;
        b.container->SetOffsetX(float(spec.monitor.x));
        b.container->SetOffsetY(float(spec.monitor.y));
        ComPtr<IDCompositionRectangleClip> clip;
        if (SUCCEEDED(device_->CreateRectangleClip(&clip))) {
            clip->SetLeft(0.f); clip->SetTop(0.f); clip->SetRight(float(spec.monitor.w)); clip->SetBottom(float(spec.monitor.h));
            b.container->SetClip(clip.Get());
        }
        b.outgoing->SetContent(b.surface.Get());
        b.incoming->SetContent(b.surface.Get());  // same pixels: the crossfade only moves it
        b.outgoing.As(&b.outgoing3);
        b.incoming.As(&b.incoming3);
        backdrops_.push_back(std::move(b));
    }
    strip_ = {};  // the strip's tile previews use the wallpaper: redraw on the next setStrip
    placeBackdrop(0, 0, 0);
}

void SceneRenderer::placeBackdrop(double presence, double transition, int direction) {
    if (!backdropLayer_) return;
    ComPtr<IDCompositionVisual3> layer3;
    if (SUCCEEDED(backdropLayer_.As(&layer3))) setOpacity(layer3.Get(), presence);
    const double q = direction == 0 ? 1.0 : std::clamp(transition, 0.0, 1.0);
    for (auto& b : backdrops_) {
        const float scale = 1.0f / kBackdropScale;
        const double maxShift = b.overscan;
        auto place = [&](IDCompositionVisual2* v, IDCompositionVisual3* v3, double shift, double opacity) {
            v->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(float(-b.overscan + shift), 0.f));
            setOpacity(v3, opacity);
        };
        if (direction == 0) {
            place(b.outgoing.Get(), b.outgoing3.Get(), 0, 1);
            place(b.incoming.Get(), b.incoming3.Get(), 0, 0);
        } else {
            // Old backdrop drifts away slower than the cards; the new one drifts in and fades up.
            place(b.outgoing.Get(), b.outgoing3.Get(), -direction * maxShift * q, 1 - q);
            place(b.incoming.Get(), b.incoming3.Get(), direction * maxShift * (1 - q), q);
        }
    }
}

// ---------- labels ----------

SceneRenderer::Label SceneRenderer::createLabel(const std::wstring& title, HICON icon, float width, float height, float dpiScale) {
    Label label;
    if (!device_ || width < 8 || height < 4) return label;
    label.w = std::ceil(width);
    label.h = std::ceil(height);
    ComPtr<ID2D1DeviceContext> dc;
    auto surface = beginSurface(UINT(label.w), UINT(label.h), dc);
    if (!surface) return label;

    const float iconSize = std::round(16.f * dpiScale), gap = std::round(6.f * dpiScale);
    ComPtr<IDWriteTextLayout> layout;
    if (!labelFormat_ || labelFormatScale_ != dpiScale) {  // one format per DPI, reused by every label
        labelFormat_.Reset();
        dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                  12.5f * dpiScale, L"", &labelFormat_);
        if (labelFormat_) {
            labelFormat_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            ComPtr<IDWriteInlineObject> ellipsis;
            dwrite_->CreateEllipsisTrimmingSign(labelFormat_.Get(), &ellipsis);
            labelFormat_->SetTrimming(&trim, ellipsis.Get());
        }
        labelFormatScale_ = dpiScale;
    }
    IDWriteTextFormat* fmt = labelFormat_.Get();
    if (fmt) {
        const float maxText = std::max(1.f, label.w - iconSize - gap);
        dwrite_->CreateTextLayout(title.c_str(), UINT32(title.size()), fmt, maxText, label.h, &layout);
    }
    DWRITE_TEXT_METRICS tm{};
    if (layout) layout->GetMetrics(&tm);
    const auto iconBmp = iconBitmap(icon);
    const float textW = std::min(tm.widthIncludingTrailingWhitespace, label.w - (iconBmp ? iconSize + gap : 0.f));
    const float groupW = (iconBmp ? iconSize + gap : 0.f) + textW;
    label.groupW = groupW;
    float x = std::round((label.w - groupW) / 2);
    if (iconBmp) {
        dc->DrawBitmap(iconBmp.Get(), D2D1::RectF(x, std::round((label.h - iconSize) / 2), x + iconSize, std::round((label.h - iconSize) / 2) + iconSize),
                       1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        x += iconSize + gap;
    }
    if (layout) {
        layout->SetMaxWidth(std::max(1.f, textW + 1));
        ComPtr<ID2D1SolidColorBrush> shadow, text;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.55f), &shadow);
        dc->CreateSolidColorBrush(white(0.95f), &text);
        const float y = std::round((label.h - tm.height) / 2);
        dc->DrawTextLayout(D2D1::Point2F(x, y + dpiScale), layout.Get(), shadow.Get());  // legibility on bright wallpapers
        dc->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), text.Get());
    }
    dc->PopAxisAlignedClip();
    if (!endSurface(surface.Get())) return {};
    label.visual = visual(labelLayer_.Get());
    if (!label.visual) return {};
    label.visual->SetContent(surface.Get());
    label.visual.As(&label.visual3);
    setOpacity(label.visual3.Get(), 0);
    return label;
}

void SceneRenderer::destroyLabel(Label& label) {
    if (label.visual && labelLayer_) labelLayer_->RemoveVisual(label.visual.Get());
    label = {};
}

void SceneRenderer::placeLabel(Label& label, const Rect& card, double opacity, float gap) {
    if (!label.visual) return;
    label.visual->SetOffsetX(std::round(float(card.cx()) - label.w / 2));
    label.visual->SetOffsetY(std::round(float(card.bottom()) + gap));
    setOpacity(label.visual3.Get(), opacity);
}

// ---------- focus chrome (hover / keyboard selection) ----------

void SceneRenderer::setFocusChrome(const Rect& card, float dpiScale) {
    if (!device_ || card.empty()) return;
    if (focus_ && std::abs(card.w - focusRaster_.w) < 1 && std::abs(card.h - focusRaster_.h) < 1) return;
    const float m = std::round(30.f * dpiScale), radius = 10.f * dpiScale, ring = std::max(2.f, std::round(3.f * dpiScale));
    const UINT w = UINT(std::ceil(card.w + 2 * m)), h = UINT(std::ceil(card.h + 2 * m));

    // Soft shadow from a filled rounded rect, then the ring just outside the card.
    ComPtr<ID2D1DeviceContext> tmpDc;
    ComPtr<ID2D1Bitmap1> shape;
    const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &tmpDc)) || FAILED(tmpDc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, props, &shape)))
        return;
    tmpDc->SetDpi(96.f, 96.f);
    tmpDc->SetTarget(shape.Get());
    tmpDc->BeginDraw();
    tmpDc->Clear(D2D1::ColorF(0, 0, 0, 0));
    ComPtr<ID2D1SolidColorBrush> solid;
    tmpDc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &solid);
    tmpDc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m, m, m + float(card.w), m + float(card.h)), radius, radius), solid.Get());
    tmpDc->EndDraw();
    tmpDc->SetTarget(nullptr);

    ComPtr<ID2D1DeviceContext> dc;
    auto surface = beginSurface(w, h, dc);
    if (!surface) return;
    ComPtr<ID2D1Effect> shadow;
    if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1Shadow, &shadow))) {
        shadow->SetInput(0, shape.Get());
        shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 9.f * dpiScale);
        shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, 0.55f));
        dc->DrawImage(shadow.Get(), D2D1::Point2F(0, 6.f * dpiScale));
    }
    ComPtr<ID2D1SolidColorBrush> ringBrush;
    dc->CreateSolidColorBrush(white(0.92f), &ringBrush);
    const float o = ring / 2 + 2.f * dpiScale;  // a hair outside the thumbnail edge
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m - o, m - o, m + float(card.w) + o, m + float(card.h) + o), radius, radius), ringBrush.Get(), ring);
    dc->PopAxisAlignedClip();
    if (!endSurface(surface.Get())) return;

    if (!focus_) {
        focus_ = visual(focusLayer_.Get());
        if (!focus_) return;
        focus_.As(&focus3_);
        setOpacity(focus3_.Get(), 0);
    }
    focus_->SetContent(surface.Get());
    focusRaster_ = card;
    focusMargin_ = m;
}

void SceneRenderer::placeFocus(const Rect& card, double opacity) {
    if (!focus_ || focusRaster_.empty()) return;
    const float sx = float(card.w / focusRaster_.w), sy = float(card.h / focusRaster_.h);
    focus_->SetTransform(D2D1::Matrix3x2F::Translation(-focusMargin_, -focusMargin_) * D2D1::Matrix3x2F::Scale(sx, sy) *
                         D2D1::Matrix3x2F::Translation(float(card.x), float(card.y)));
    setOpacity(focus3_.Get(), opacity);
}

// ---------- traffic lights ----------

void SceneRenderer::setTrafficLights(const TrafficLights& tl, float dpiScale, int hot) {
    if (!device_ || tl.empty()) return;
    if (traffic_ && hot == trafficHot_ && dpiScale == trafficDpi_ && tl.zone == trafficRaster_) return;
    const UINT w = UINT(std::ceil(tl.zone.w)), h = UINT(std::ceil(tl.zone.h));
    ComPtr<ID2D1DeviceContext> dc;
    auto surface = beginSurface(w, h, dc);
    if (!surface) return;
    TrafficButtonLook look[3];
    if (hot >= 0) look[hot].hover = 1.f;
    drawTrafficCircles(dc.Get(), d2dFactory_.Get(), tl, tl.zone.x, tl.zone.y, dpiScale, look);
    dc->PopAxisAlignedClip();
    if (!endSurface(surface.Get())) return;
    if (!traffic_) {
        traffic_ = visual(trafficLayer_.Get());
        if (!traffic_) return;
        traffic_.As(&traffic3_);
        setOpacity(traffic3_.Get(), 0);
    }
    traffic_->SetContent(surface.Get());
    traffic_->SetOffsetX(std::round(float(tl.zone.x)));
    traffic_->SetOffsetY(std::round(float(tl.zone.y)));
    trafficRaster_ = tl.zone;
    trafficHot_ = hot;
    trafficDpi_ = dpiScale;
}

void SceneRenderer::placeTraffic(double opacity) {
    if (traffic_) setOpacity(traffic3_.Get(), opacity);
}

// ---------- desktop strip ----------

void SceneRenderer::setStrip(const StripContent& s) {
    if (!device_ || (s == strip_ && stripVisual_)) return;
    strip_ = s;
    if (stripVisual_) stripLayer_->RemoveVisual(stripVisual_.Get());
    if (stripRing_) stripLayer_->RemoveVisual(stripRing_.Get());
    stripVisual_.Reset(); stripVisual3_.Reset(); stripRing_.Reset();
    if (s.band.empty() || s.tiles.empty()) return;

    const float d = s.dpiScale, radius = 8.f * d;
    ComPtr<ID2D1DeviceContext> dc;
    auto surface = beginSurface(UINT(std::ceil(s.band.w)), UINT(std::ceil(s.band.h)), dc);
    if (!surface) return;
    auto local = [&](const Rect& r) { return D2D1::RectF(float(r.x - s.band.x), float(r.y - s.band.y), float(r.right() - s.band.x), float(r.bottom() - s.band.y)); };

    const Rect& t0 = s.tiles.front();
    // Tile previews show the wallpaper of the monitor the strip is on.
    BackdropSpec home;
    for (auto& b : backdrops_)
        if (b.spec.monitor.contains(s.band.cx(), s.band.y + 1)) home = b.spec;
    auto preview = loadImage(home.wallpaper, UINT(std::ceil(t0.w)), UINT(std::ceil(t0.h)), true);
    ComPtr<ID2D1SolidColorBrush> fill, border, text, plusBrush;
    dc->CreateSolidColorBrush(D2D1::ColorF(GetRValue(home.color) / 255.f, GetGValue(home.color) / 255.f, GetBValue(home.color) / 255.f, 1.f), &fill);
    dc->CreateSolidColorBrush(white(0.22f), &border);
    dc->CreateSolidColorBrush(white(0.92f), &text);
    dc->CreateSolidColorBrush(white(0.16f), &plusBrush);
    ComPtr<IDWriteTextFormat> fmt;
    dwrite_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.f * d, L"", &fmt);
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }

    for (std::size_t i = 0; i < s.tiles.size(); ++i) {
        const D2D1_RECT_F r = local(s.tiles[i]);
        ComPtr<ID2D1RoundedRectangleGeometry> geo;
        d2dFactory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, radius, radius), &geo);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), fill.Get());
        if (preview && geo) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get()), nullptr);
            dc->DrawBitmap(preview.Get(), r);
            dc->PopLayer();
        }
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), border.Get(), std::max(1.f, d));
        if (fmt && i < s.names.size())
            dc->DrawText(s.names[i].c_str(), UINT32(s.names[i].size()), fmt.Get(), D2D1::RectF(r.left - 20 * d, r.bottom + 3 * d, r.right + 20 * d, r.bottom + 22 * d), text.Get());
    }
    if (!s.plus.empty()) {
        const D2D1_RECT_F r = local(s.plus);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), plusBrush.Get());
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, arm = std::min(r.right - r.left, r.bottom - r.top) * 0.2f;
        dc->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx + arm, cy), text.Get(), 2.f * d);
        dc->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy + arm), text.Get(), 2.f * d);
    }
    dc->PopAxisAlignedClip();
    if (!endSurface(surface.Get())) return;
    stripVisual_ = visual(stripLayer_.Get());
    if (!stripVisual_) return;
    stripVisual_->SetContent(surface.Get());
    stripVisual_->SetOffsetX(float(s.band.x));
    stripVisual_->SetOffsetY(float(s.band.y));
    stripVisual_.As(&stripVisual3_);

    // Highlight ring of the current desktop: its own visual so it can glide between tiles.
    stripRingPad_ = std::round(4.f * d);
    ComPtr<ID2D1DeviceContext> rdc;
    const UINT rw = UINT(std::ceil(t0.w + 2 * stripRingPad_)), rh = UINT(std::ceil(t0.h + 2 * stripRingPad_));
    auto ringSurface = beginSurface(rw, rh, rdc);
    if (!ringSurface) return;
    ComPtr<ID2D1SolidColorBrush> ringBrush;
    rdc->CreateSolidColorBrush(white(0.95f), &ringBrush);
    const float stroke = std::max(2.f, std::round(2.5f * d));
    rdc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(stroke / 2, stroke / 2, rw - stroke / 2, rh - stroke / 2), radius + stripRingPad_, radius + stripRingPad_),
                              ringBrush.Get(), stroke);
    rdc->PopAxisAlignedClip();
    if (!endSurface(ringSurface.Get())) return;
    stripRing_ = visual(stripLayer_.Get());
    if (!stripRing_) return;
    stripRing_->SetContent(ringSurface.Get());
    stripRing_->SetOffsetY(float(t0.y) - stripRingPad_);
}

double SceneRenderer::stripTileX(int index) const {
    if (index < 0 || index >= int(strip_.tiles.size())) return strip_.tiles.empty() ? 0 : strip_.tiles.front().x;
    return strip_.tiles[index].x;
}

void SceneRenderer::placeStrip(double presence, double ringX) {
    if (!stripLayer_) return;
    // Slides down from above the screen while fading in.
    stripLayer_->SetOffsetY(float(-(1.0 - std::clamp(presence, 0.0, 1.0)) * (strip_.band.h * 0.6)));
    ComPtr<IDCompositionVisual3> layer3;
    if (SUCCEEDED(stripLayer_.As(&layer3))) setOpacity(layer3.Get(), presence);
    if (stripRing_) stripRing_->SetOffsetX(float(ringX) - stripRingPad_);
}

void SceneRenderer::trim() {
    // Idle after an exit: give the driver's temporary allocations back (public DXGI API meant for
    // exactly this). Surfaces still in use are not affected.
    ComPtr<IDXGIDevice3> dxgi;
    if (d3d_ && SUCCEEDED(d3d_.As(&dxgi))) dxgi->Trim();
}

bool SceneRenderer::commit() {
    if (!device_) return false;
    if (!log::check(device_->Commit(), "IDCompositionDevice::Commit")) return false;
    // IDCompositionDevice2 has no CheckDeviceState; a removed D3D device means every surface is gone.
    return log::check(d3d_->GetDeviceRemovedReason(), "D3D device removed");
}

}  // namespace mc
