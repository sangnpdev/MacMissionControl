#include "rendering/traffic_lights_draw.h"

#include <algorithm>
#include <wrl/client.h>

namespace mc {

using Microsoft::WRL::ComPtr;

namespace {
D2D1_COLOR_F mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
}  // namespace

void drawTrafficCircles(ID2D1RenderTarget* rt, ID2D1Factory* factory, const TrafficLights& tl, double originX, double originY,
                        float dpiScale, const TrafficButtonLook look[3], float inactive) {
    static const D2D1_COLOR_F colors[3] = {D2D1::ColorF(0xFF5F57), D2D1::ColorF(0xFEBC2E), D2D1::ColorF(0x28C840)};
    const D2D1_COLOR_F grey = D2D1::ColorF(0.62f, 0.62f, 0.62f, 1.f), black = D2D1::ColorF(0, 0, 0, 1.f);
    ComPtr<ID2D1SolidColorBrush> fill, rim, glyph;
    rt->CreateSolidColorBrush(colors[0], &fill);
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.18f), &rim);
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &glyph);
    if (!fill || !rim || !glyph) return;
    float groupHover = 0.f;
    for (int i = 0; i < 3; ++i)
        if (!look[i].disabled) groupHover = std::max(groupHover, look[i].hover);  // glyphs show on all three, like macOS
    groupHover *= 1.f - inactive;
    const float sw = std::max(1.f, 1.3f * dpiScale);
    for (int i = 0; i < 3; ++i) {
        const Rect& c = tl.circle[i];
        const float cx = float(c.cx() - originX), cy = float(c.cy() - originY), d = float(c.w);
        const float r = d / 2 * (1.f + 0.08f * look[i].hover * (1.f - inactive));
        D2D1_COLOR_F col = mix(mix(colors[i], grey, inactive), black, 0.22f * look[i].pressed);
        if (look[i].disabled) col = D2D1::ColorF(0.62f, 0.62f, 0.62f, 0.45f);  // like macOS: a dim grey dot
        fill->SetColor(col);
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), fill.Get());
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r - 0.5f, r - 0.5f), rim.Get(), 1.f);
        if (groupHover <= 0.01f || look[i].disabled) continue;
        glyph->SetOpacity(groupHover);
        const float q = d * 0.24f;
        if (i == 0) {
            rt->DrawLine(D2D1::Point2F(cx - q, cy - q), D2D1::Point2F(cx + q, cy + q), glyph.Get(), sw);
            rt->DrawLine(D2D1::Point2F(cx - q, cy + q), D2D1::Point2F(cx + q, cy - q), glyph.Get(), sw);
        } else if (i == 1) {
            rt->DrawLine(D2D1::Point2F(cx - q, cy), D2D1::Point2F(cx + q, cy), glyph.Get(), sw);
        } else {
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(factory->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                const float k = d * 0.34f;  // triangle leg
                sink->BeginFigure(D2D1::Point2F(cx + q, cy - q), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cx + q - k, cy - q));
                sink->AddLine(D2D1::Point2F(cx + q, cy - q + k));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->BeginFigure(D2D1::Point2F(cx - q, cy + q), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cx - q + k, cy + q));
                sink->AddLine(D2D1::Point2F(cx - q, cy + q - k));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                rt->FillGeometry(path.Get(), glyph.Get());
            }
        }
    }
}

}  // namespace mc
