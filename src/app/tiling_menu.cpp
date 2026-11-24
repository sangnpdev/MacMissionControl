#include "app/tiling_menu.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "diagnostics/log.h"

namespace mc {
namespace {
constexpr wchar_t kClass[] = L"MCTilingMenu";
constexpr UINT_PTR kTimerAnim = 1, kTimerClose = 2, kTimerVerify = 3;
constexpr UINT kCloseGraceMs = 200;  // from the green dot to the menu the mouse crosses a few empty pixels
constexpr double kOpenSec = 0.17, kCloseSec = 0.12, kHoverSec = 0.12, kPressSec = 0.08;

D2D1_COLOR_F rgba(int r, int g, int b, float a = 1.f) { return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a); }
D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
}  // namespace

double TilingMenu::now() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq_.QuadPart);
}

// Reduced Motion / animations off: jump to the end value, the next render shows the final state at once.
void TilingMenu::animTo(anim::AnimatedValue& v, double target, double seconds, anim::CubicBezier bez) {
    if (animate_) v.easeTo(target, now(), seconds, bez);
    else v.jump(target);
}

void TilingMenu::init(HINSTANCE inst, ID2D1Factory* factory) {
    inst_ = inst;
    factory_ = factory;
    QueryPerformanceFrequency(&freq_);
}

bool TilingMenu::ensureWindow() {
    if (wnd_) return true;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &TilingMenu::wndProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);  // fails harmlessly when registered before
    wnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                           inst_, this);
    return wnd_ != nullptr;
}

void TilingMenu::destroy() {
    if (wnd_) {
        KillTimer(wnd_, kTimerAnim);
        KillTimer(wnd_, kTimerClose);
        KillTimer(wnd_, kTimerVerify);
        HWND w = wnd_;
        wnd_ = nullptr;  // before DestroyWindow: its messages must see a stopped object
        DestroyWindow(w);
    }
    releaseDib();
    rt_.Reset();
    factory_.Reset();
    state_ = State::Closed;
    target_ = notify_ = nullptr;
    animating_ = closeArmed_ = false;
}

bool TilingMenu::sizeDib(int w, int h) {
    if (bmp_ && w == bw_ && h == bh_) return true;
    releaseDib();
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dc_ = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc_ || !bmp_) { releaseDib(); return false; }
    old_ = static_cast<HBITMAP>(SelectObject(dc_, bmp_));
    bw_ = w;
    bh_ = h;
    return true;
}

void TilingMenu::releaseDib() {
    if (dc_) { SelectObject(dc_, old_); DeleteDC(dc_); dc_ = nullptr; }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
    bw_ = bh_ = 0;
}

void TilingMenu::open(const Anchor& a) {
    if (!a.target || !factory_ || !ensureWindow()) return;
    if (state_ != State::Closed && target_ == a.target) {
        if (state_ == State::Closing && !picked_) {  // the mouse came back
            state_ = State::Opening;
            animTo(openP_, 1.0, kOpenSec, anim::easing::emphasized);
            schedule();
        }
        return;
    }
    if (state_ != State::Closed) finishClose();  // another window's menu: gone at once, one menu instance at all times
    target_ = a.target;
    notify_ = a.notify;
    dpi_ = a.dpi;
    dark_ = a.dark;
    animate_ = a.animate;
    geo_ = layoutTileMenu(dpi_);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(a.target, MONITOR_DEFAULTTONEAREST), &mi);
    const Rect work{double(mi.rcWork.left), double(mi.rcWork.top), double(mi.rcWork.right - mi.rcWork.left), double(mi.rcWork.bottom - mi.rcWork.top)};
    const Rect anchor{a.dot.x, a.strip.y, a.dot.w, a.strip.h};
    screen_ = placeTileMenu(geo_.panel, anchor, work, std::round(4 * dpi_));
    if (!sizeDib(int(screen_.w), int(screen_.h))) { target_ = notify_ = nullptr; return; }
    if (!rt_) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f,
            D2D1_RENDER_TARGET_USAGE_NONE);
        if (FAILED(factory_->CreateDCRenderTarget(&props, &rt_))) { target_ = notify_ = nullptr; return; }
    }
    hot_ = pressed_ = -1;
    for (int i = 0; i < kTileLayoutCount; ++i) { hover_[i].jump(0.0); press_[i].jump(0.0); }
    overAnchor_ = true;
    overMenu_ = closeArmed_ = picked_ = false;
    openP_.jump(0.0);
    state_ = State::Opening;
    animTo(openP_, 1.0, kOpenSec, anim::easing::emphasized);
    // Position first, draw, then show: the first visible frame already has content.
    SetWindowPos(wnd_, HWND_TOPMOST, int(screen_.x), int(screen_.y), int(screen_.w), int(screen_.h), SWP_NOACTIVATE);
    render();
    ShowWindow(wnd_, SW_SHOWNOACTIVATE);
    schedule();
    log::write("tile menu: open for %p (animate=%d, dpi=%.2f)", static_cast<void*>(target_), int(animate_), dpi_);
}

void TilingMenu::close(HWND target, bool immediate) {
    if (!wnd_ || state_ == State::Closed || target != target_) return;
    KillTimer(wnd_, kTimerClose);
    closeArmed_ = false;
    if (immediate || !animate_) { finishClose(); return; }
    if (state_ == State::Closing) return;
    state_ = State::Closing;
    animTo(openP_, 0.0, kCloseSec, anim::easing::easeOut);
    schedule();
}

void TilingMenu::finishClose() {
    if (!wnd_) return;
    KillTimer(wnd_, kTimerAnim);
    KillTimer(wnd_, kTimerClose);
    animating_ = closeArmed_ = false;
    ShowWindow(wnd_, SW_HIDE);
    state_ = State::Closed;
    hot_ = pressed_ = -1;
    if (GetCapture() == wnd_) ReleaseCapture();
    const HWND n = notify_;
    target_ = notify_ = nullptr;
    if (n && IsWindow(n)) PostMessageW(n, kMsgClosed, 0, 0);  // the dot goes back to its resting look
}

void TilingMenu::anchorHover(HWND target, bool over) {
    if (!wnd_ || state_ == State::Closed || target != target_) return;
    overAnchor_ = over;
    evalClose();
}

// The menu stays while the mouse is over the green dot / strip or over the menu; otherwise it closes after a short
// grace (one one-shot timer, armed once). Coming back before it fires cancels it.
void TilingMenu::evalClose() {
    if (overAnchor_ || overMenu_) {
        if (closeArmed_) { KillTimer(wnd_, kTimerClose); closeArmed_ = false; }
        if (state_ == State::Closing && !picked_) {  // the mouse came back (not after a pick: the click closes for good)
            state_ = State::Opening;
            animTo(openP_, 1.0, kOpenSec, anim::easing::emphasized);
            schedule();
        }
    } else if (!closeArmed_ && state_ != State::Closing) {
        closeArmed_ = true;
        SetTimer(wnd_, kTimerClose, kCloseGraceMs, nullptr);
    }
}

void TilingMenu::setHot(int i) {
    if (i == hot_) return;
    if (hot_ >= 0) animTo(hover_[hot_], 0.0, kHoverSec, anim::easing::easeOut);
    if (i >= 0) animTo(hover_[i], 1.0, kHoverSec, anim::easing::easeOut);
    hot_ = i;
    schedule();
}

void TilingMenu::press(int i, bool down) {
    if (i < 0) return;
    animTo(press_[i], down ? 1.0 : 0.0, kPressSec, anim::easing::easeOut);
    schedule();
}

// Closes first, then tiles: the action does not wait for any animation. The tiler's result is checked 150 ms later.
void TilingMenu::pick(int i) {
    const HWND t = target_;
    const TileLayout l = static_cast<TileLayout>(i);
    picked_ = true;
    close(t);
    if (tiler_.apply(t, l)) SetTimer(wnd_, kTimerVerify, 150, nullptr);
}

void TilingMenu::schedule() {
    if (animating_ || !wnd_) return;
    animating_ = true;
    SetTimer(wnd_, kTimerAnim, 15, nullptr);  // only while something moves
}

void TilingMenu::animate() {
    const double t = now();
    bool settled = openP_.settled(t);
    for (int i = 0; i < kTileLayoutCount; ++i) settled = settled && hover_[i].settled(t) && press_[i].settled(t);
    if (state_ == State::Closing && openP_.settled(t)) { finishClose(); return; }
    render();
    if (settled) {
        KillTimer(wnd_, kTimerAnim);
        animating_ = false;
        if (state_ == State::Opening) state_ = State::Open;
    }
}

void TilingMenu::render() {
    if (!wnd_ || !rt_ || !dc_) return;
    const double t = now();
    const float p = float(std::clamp(openP_.value(t), 0.0, 1.0));
    const float s = float(dpi_);
    const RECT rc{0, 0, bw_, bh_};
    if (FAILED(rt_->BindDC(dc_, &rc))) return;
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
    // Grows from 95% about the top centre (the side that touches the dot) while it fades in.
    const float sc = 0.95f + 0.05f * p;
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(float(bw_) / 2, 0.f)));

    const D2D1_COLOR_F panel = dark_ ? rgba(40, 40, 43, 0.97f) : rgba(246, 246, 248, 0.97f);
    const D2D1_COLOR_F edge = dark_ ? rgba(255, 255, 255, 0.14f) : rgba(0, 0, 0, 0.14f);
    const D2D1_COLOR_F ink = dark_ ? rgba(150, 150, 156) : rgba(120, 120, 126);  // icon outline and region
    const D2D1_COLOR_F accent = rgba(10, 132, 255), accentDown = rgba(0, 105, 220), white = rgba(255, 255, 255);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> br;
    rt_->CreateSolidColorBrush(panel, &br);
    if (!br) { rt_->EndDraw(); return; }
    const float rad = 10.f * s;
    const D2D1_ROUNDED_RECT pr = D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, float(bw_) - 0.5f, float(bh_) - 0.5f), rad, rad);
    rt_->FillRoundedRectangle(pr, br.Get());
    br->SetColor(edge);
    rt_->DrawRoundedRectangle(pr, br.Get(), std::max(1.f, s));

    for (int i = 0; i < kTileLayoutCount; ++i) {
        const Rect& r = geo_.tile[i];
        const float hv = float(std::clamp(hover_[i].value(t), 0.0, 1.0)), pv = float(std::clamp(press_[i].value(t), 0.0, 1.0));
        const D2D1_RECT_F tr = D2D1::RectF(float(r.x), float(r.y), float(r.right()), float(r.bottom()));
        if (hv > 0.f) {  // highlight
            D2D1_COLOR_F c = mix(accent, accentDown, pv);
            c.a = hv;
            br->SetColor(c);
            rt_->FillRoundedRectangle(D2D1::RoundedRect(tr, 5.f * s, 5.f * s), br.Get());
        }
        // Icon: the screen outline and the region the layout fills, from the same layoutRect the tiler uses.
        const float in = 4.f * s;
        const D2D1_RECT_F scr = D2D1::RectF(tr.left + in, tr.top + in, tr.right - in, tr.bottom - in);
        const Rect box{scr.left, scr.top, scr.right - scr.left, scr.bottom - scr.top};
        const Rect reg = layoutRect(static_cast<TileLayout>(i), box);
        const float g = 1.5f * s;
        br->SetColor(mix(ink, white, hv));
        rt_->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(float(reg.x) + g, float(reg.y) + g, float(reg.right()) - g, float(reg.bottom()) - g), 1.5f * s, 1.5f * s),
            br.Get());
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(scr.left + 0.5f, scr.top + 0.5f, scr.right - 0.5f, scr.bottom - 0.5f), 2.5f * s, 2.5f * s),
                                  br.Get(), std::max(1.f, 1.2f * s));
    }
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (FAILED(rt_->EndDraw())) return;

    HDC screen = GetDC(nullptr);
    POINT src{0, 0};
    SIZE sz{bw_, bh_};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(std::lround(255.0 * p)), AC_SRC_ALPHA};
    UpdateLayeredWindow(wnd_, screen, nullptr, &sz, dc_, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK TilingMenu::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<TilingMenu*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->onMessage(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT TilingMenu::onMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!wnd_) return DefWindowProcW(h, msg, wp, lp);  // creating or destroyed
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == kTimerAnim) {
            animate();
        } else if (wp == kTimerClose) {
            KillTimer(h, kTimerClose);
            closeArmed_ = false;
            if (!overAnchor_ && !overMenu_) close(target_);
        } else if (wp == kTimerVerify) {
            KillTimer(h, kTimerVerify);
            if (tiler_.verify()) SetTimer(h, kTimerVerify, 150, nullptr);
        }
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        overMenu_ = true;
        setHot(tileMenuHit(geo_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        evalClose();
        return 0;
    }
    case WM_MOUSELEAVE:
        overMenu_ = false;
        if (pressed_ < 0) setHot(-1);
        evalClose();
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = tileMenuHit(geo_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (pressed_ >= 0) { SetCapture(h); press(pressed_, true); }
        return 0;
    case WM_LBUTTONUP: {
        const int was = pressed_, over = tileMenuHit(geo_, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        pressed_ = -1;
        if (GetCapture() == h) ReleaseCapture();
        press(was, false);
        if (was >= 0 && was == over) pick(was);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ >= 0) { press(pressed_, false); pressed_ = -1; }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace mc
