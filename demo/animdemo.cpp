// Animation engine demo: one panel per primitive, all driven by mc::anim and the JSON config.
// Usage: mc_animdemo.exe [--config PATH] [--seconds N]
//   R      reload the config file
//   Esc    quit
//   Drag in the "Gesture" panel; hover the card in the "Scale (hover)" panel.
// Rendering: Direct2D, paced by the DWM compositor clock (same pacing as the app).
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "animation/animation.h"
#include "animation/gesture.h"
#include "animation/visual_motion.h"
#include "config/animation_config.h"

using Microsoft::WRL::ComPtr;
using namespace mc;
using namespace mc::anim;

namespace {

constexpr double kHalfCycle = 1.3;  // s between automatic direction changes

double nowSec() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

D2D1_COLOR_F rgb(unsigned hex, float a = 1.0f) {
    return {((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a};
}
D2D1_RECT_F toD2D(const Rect& r) { return {float(r.x), float(r.y), float(r.right()), float(r.bottom())}; }

const unsigned kPalette[] = {0x0A84FF, 0x30D158, 0xFF9F0A, 0xFF375F, 0xBF5AF2, 0x64D2FF, 0xFFD60A, 0xAC8E68, 0x5E5CE6, 0x66D4CF};

struct Demo {
    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IDWriteTextFormat> title, body;
    float dpiScale = 1.0f;

    std::wstring configPath, configStatus;
    AnimationConfig cfg;

    double start = 0;
    long long lastCycle = -1;
    bool forward = false;

    // Panels' animation state.
    std::array<AnimatedValue, 5> easings;
    std::array<AnimatedValue, 4> springs;
    std::array<VisualMotion, 10> stagger;
    VisualMotion hover;
    bool hovering = false;
    VisualMotion move, moveInterrupted;
    double lastInterrupt = 0;
    bool interruptDir = false;
    AnimatedValue fade, dim;
    AnimatedValue desktop;  // 0..2, desktop index
    int desktopTarget = 0;
    AnimatedValue gesture;  // card position 0..1
    GestureTracker tracker;
    bool dragging = false, userDragged = false;
    double dragStartX = 0, dragStartProgress = 0;
    double autoDragStart = -1, autoFrom = 0;
    std::array<VisualMotion, 4> tiles;
    int sharedIndex = 0;

    std::deque<double> frameTimes;
    Rect panels[9];
    float mouseX = -1, mouseY = -1;

    void loadConfig() {
        AnimationConfig fresh;
        FILE* f = nullptr;
        if (_wfopen_s(&f, configPath.c_str(), L"rb") || !f) {
            cfg = fresh;
            configStatus = L"config: " + configPath + L" not found, defaults";
            return;
        }
        std::string text;
        char buf[4096];
        for (std::size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        fclose(f);
        const auto errs = loadAnimationConfig(text, fresh);
        if (errs.empty()) {
            cfg = fresh;
            configStatus = L"config: " + configPath + L" loaded";
        } else {
            configStatus = L"config error (kept previous): " + std::wstring(errs[0].begin(), errs[0].end());
        }
        tracker = GestureTracker(cfg.gestureRubberBand);
    }

    void layout(float w, float h) {
        const float top = 8, bottom = 40, gap = 12;
        const float pw = (w - gap * 4) / 3, ph = (h - top - bottom - gap * 3) / 3;
        for (int i = 0; i < 9; ++i)
            panels[i] = {gap + (i % 3) * (pw + gap), top + gap + (i / 3) * (ph + gap), pw, ph};
    }
    Rect content(int i) const {  // panel area below the title
        const Rect& p = panels[i];
        return {p.x + 16, p.y + 40, p.w - 32, p.h - 56};
    }

    // ---------- automatic direction changes ----------
    void tick(double now) {
        const long long cycle = (long long)std::floor((now - start) / kHalfCycle);
        if (cycle != lastCycle) {
            lastCycle = cycle;
            forward = !forward;
            onToggle(now);
        }
        // Translation panel: retarget every 0.2 s to show velocity-continuous interruption.
        if (now - lastInterrupt > 0.2) {
            lastInterrupt = now;
            interruptDir = !interruptDir;
            const Rect c = content(4);
            const double s = c.h * 0.22;
            const Rect r{interruptDir ? c.right() - s : c.x, c.y + c.h * 0.62, s, s};
            moveInterrupted.springTo(r, 1.0, now, cfg.exitSpring);
        }
        // Gesture panel: scripted drag when the user is not dragging.
        if (!userDragged && !dragging) autoGesture(now);
    }

    // Places every motion at its resting state with the right base size (first frame, resize).
    void reset() {
        const Rect hc = cardIn(content(3));
        hover = VisualMotion(hc.w, hc.h);
        hover.jump(hc, 1.0);
        const Rect mc = content(4);
        const double s = mc.h * 0.22;
        move = VisualMotion(s, s);
        move.jump({mc.x, mc.y + mc.h * 0.18, s, s}, 1.0);
        moveInterrupted = VisualMotion(s, s);
        moveInterrupted.jump({mc.x, mc.y + mc.h * 0.62, s, s}, 1.0);
        const Rect shc = content(8);
        for (int i = 0; i < 4; ++i) {
            const Rect t = tileRect(shc, i);
            tiles[i] = VisualMotion(t.w, t.h);
            tiles[i].jump(t, 1.0);
        }
        lastCycle = -1;
        forward = false;
    }

    void onToggle(double now) {
        const double t = forward ? 1.0 : 0.0;
        const CubicBezier presets[] = {easing::linear, easing::easeIn, easing::easeOut, easing::easeInOut, easing::emphasized};
        for (int i = 0; i < 5; ++i) easings[i].easeTo(t, now, 0.8, presets[i]);
        const SpringParams sp[] = {{260, 0.3, 1, 0}, {260, 0.6, 1, 0}, cfg.entranceSpring, {260, 1.5, 1, 0}};
        for (int i = 0; i < 4; ++i) springs[i].springTo(t, now, sp[i], 1e-3);

        const Rect sc = content(2);
        const double cell = std::min(sc.w / 5.0, sc.h / 2.0) * 0.8;
        for (int i = 0; i < 10; ++i) {
            const Rect slot{sc.x + (i % 5) * sc.w / 5 + (sc.w / 5 - cell) / 2, sc.y + (i / 5) * sc.h / 2 + (sc.h / 2 - cell) / 2, cell, cell};
            if (forward) {
                stagger[i] = VisualMotion(cell, cell);
                stagger[i].jump(scaled(slot, cfg.minimizedStartScale), 0.0);
                stagger[i].springTo(slot, 1.0, now, cfg.entranceSpring, staggerDelay(i, 10, cfg.staggerStep, cfg.staggerMax));
            } else {
                stagger[i].springTo(scaled(slot, cfg.minimizedStartScale), 0.0, now, cfg.exitSpring);
            }
        }

        if (!hovering) {
            const Rect c = content(3);
            const Rect card = cardIn(c);
            hover.springTo(forward ? scaled(card, cfg.hoverScale) : card, 1.0, now, cfg.hoverSpring);
        }

        const Rect mc = content(4);
        const double s = mc.h * 0.22;
        move.springTo({forward ? mc.right() - s : mc.x, mc.y + mc.h * 0.18, s, s}, 1.0, now, cfg.exitSpring);

        if (forward) {
            fade.easeTo(1.0, now, cfg.dimInDuration, cfg.dimInEasing);
            dim.easeTo(cfg.dimOpacity, now, cfg.dimInDuration, cfg.dimInEasing);
        } else {
            fade.easeTo(0.0, now, cfg.dimOutDuration, cfg.dimOutEasing);
            dim.easeTo(0.0, now, cfg.dimOutDuration, cfg.dimOutEasing);
        }

        desktopTarget = (desktopTarget + 1) % 4;  // 0,1,2,1 ping-pong
        const int idx = desktopTarget == 3 ? 1 : desktopTarget;
        desktop.easeTo(idx, now, cfg.desktopDuration, cfg.desktopEasing);

        const Rect shc = content(8);
        if (forward) sharedIndex = (sharedIndex + 1) % 4;
        for (int i = 0; i < 4; ++i) {
            const Rect tile = tileRect(shc, i);
            if (!forward) tiles[i].springTo(tile, 1.0, now, cfg.exitSpring);
            else if (i == sharedIndex) tiles[i].springTo(shc, 1.0, now, cfg.entranceSpring);
            else tiles[i].springTo(scaled(tile, 0.9), 0.0, now, cfg.entranceSpring);
        }
    }

    static Rect cardIn(const Rect& c) {
        const double h = c.h * 0.6, w = h * 1.5;
        return {c.cx() - w / 2, c.cy() - h / 2, w, h};
    }
    static Rect tileRect(const Rect& c, int i) {
        const double gap = 10, w = (c.w - gap) / 2, h = (c.h - gap) / 2;
        return {c.x + (i % 2) * (w + gap), c.y + (i / 2) * (h + gap), w, h};
    }

    void autoGesture(double now) {
        // Every full cycle: a 0.25 s drag of 0.35 toward the other side, then release (inertia decides).
        const double phase = std::fmod(now - start, kHalfCycle * 2);
        if (autoDragStart < 0 && phase < 0.02) {
            autoDragStart = now;
            autoFrom = std::round(gesture.value(now));
            tracker.begin(now, autoFrom);
        }
        if (autoDragStart >= 0) {
            const double t = now - autoDragStart, dir = autoFrom < 0.5 ? 1.0 : -1.0;
            if (t < 0.25) {
                tracker.update(now, autoFrom + dir * 0.35 * (t / 0.25));
                gesture.jump(tracker.progress(), tracker.velocity());
            } else {
                release(now);
                autoDragStart = -1;
            }
        }
    }
    void release(double now) {
        const auto r = tracker.release(cfg.gestureProjection);
        gesture.jump(r.from, r.velocity);
        gesture.springTo(r.target, now, cfg.gestureSpring, 1e-3);
    }

    // ---------- input ----------
    void onMouse(UINT msg, float x, float y) {
        mouseX = x; mouseY = y;
        const double now = nowSec();
        const Rect g = content(7);
        const double travel = g.w * 0.6;
        if (msg == WM_LBUTTONDOWN && panels[7].contains(x, y)) {
            dragging = userDragged = true;
            autoDragStart = -1;
            dragStartX = x;
            dragStartProgress = gesture.value(now);
            tracker.begin(now, dragStartProgress);
            SetCapture(hwnd);
        } else if (msg == WM_MOUSEMOVE && dragging) {
            tracker.update(now, dragStartProgress + (x - dragStartX) / travel);
            gesture.jump(tracker.progress(), tracker.velocity());
        } else if (msg == WM_LBUTTONUP && dragging) {
            dragging = false;
            ReleaseCapture();
            release(now);
        }
        // Real hover in the scale panel.
        const Rect card = cardIn(content(3));
        const bool inside = card.contains(x, y);
        if (inside != hovering) {
            hovering = inside;
            hover.springTo(inside ? scaled(card, cfg.hoverScale) : card, 1.0, now, cfg.hoverSpring);
        }
    }

    // ---------- drawing ----------
    void fill(const Rect& r, unsigned color, float alpha = 1.0f, float radius = 8.0f) {
        brush->SetColor(rgb(color, alpha));
        rt->FillRoundedRectangle({toD2D(r), radius, radius}, brush.Get());
    }
    void text(const std::wstring& s, const Rect& r, IDWriteTextFormat* fmt, unsigned color = 0xEBEBF5, float alpha = 0.9f) {
        brush->SetColor(rgb(color, alpha));
        rt->DrawText(s.c_str(), UINT32(s.size()), fmt, toD2D(r), brush.Get());
    }
    void dot(double x, double y, float radius, unsigned color) {
        brush->SetColor(rgb(color));
        rt->FillEllipse({{float(x), float(y)}, radius, radius}, brush.Get());
    }

    // Recomputes panel rects; resets the motions when the size changed.
    void updateLayout() {
        const D2D1_SIZE_F size = rt->GetSize();
        if (size.width == laidOutW && size.height == laidOutH) return;
        laidOutW = size.width;
        laidOutH = size.height;
        layout(size.width, size.height);
        reset();
    }
    float laidOutW = -1, laidOutH = -1;

    void render(double now) {
        D2D1_SIZE_F size = rt->GetSize();
        rt->BeginDraw();
        rt->Clear(rgb(0x1C1C1E));
        static const wchar_t* names[] = {L"Easing (linear, in, out, in-out, emphasized)", L"Springs (ζ 0.3, 0.6, entrance cfg, 1.5)",
                                         L"Stagger (entrance cfg)", L"Scale (hover cfg; hover the card)",
                                         L"Translation (top) + retarget every 0.2 s (bottom)", L"Opacity + background dim (dim cfg)",
                                         L"Desktop switch + parallax (desktopTransition cfg)", L"Gesture (drag; inertia + rubber band)",
                                         L"Shared transition (tile → full)"};
        for (int i = 0; i < 9; ++i) {
            fill(panels[i], 0x2C2C2E, 1.0f, 12.0f);
            text(names[i], {panels[i].x + 16, panels[i].y + 10, panels[i].w - 32, 24}, title.Get());
        }

        // 0. Easing
        {
            const Rect c = content(0);
            for (int i = 0; i < 5; ++i) {
                const double y = c.y + c.h * (i + 0.5) / 5;
                brush->SetColor(rgb(0x48484A));
                rt->DrawLine({float(c.x), float(y)}, {float(c.right()), float(y)}, brush.Get(), 1.0f);
                dot(c.x + 10 + (c.w - 20) * easings[i].value(now), y, 9, kPalette[i]);
            }
        }
        // 1. Springs: the track is the middle 55%, leaving room for ~40% overshoot on each side.
        {
            const Rect c = content(1);
            rt->PushAxisAlignedClip(toD2D(panels[1]), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            for (int i = 0; i < 4; ++i) {
                const double y = c.y + c.h * (i + 0.5) / 4, x0 = c.x + c.w * 0.225, x1 = c.right() - c.w * 0.225;
                brush->SetColor(rgb(0x48484A));
                rt->DrawLine({float(x0), float(y)}, {float(x1), float(y)}, brush.Get(), 1.0f);
                dot(x0 + (x1 - x0) * springs[i].value(now), y, 9, kPalette[i + 5]);
            }
            rt->PopAxisAlignedClip();
        }
        // 2. Stagger
        for (int i = 0; i < 10; ++i) fill(stagger[i].rect(now), kPalette[i], float(stagger[i].opacity(now)), 6);
        // 3. Scale
        fill(hover.rect(now), 0x0A84FF, 1.0f, 10);
        // 4. Translation
        fill(move.rect(now), 0x30D158, 1.0f, 8);
        fill(moveInterrupted.rect(now), 0xFF9F0A, 1.0f, 8);
        // 5. Opacity + dim
        {
            const Rect c = content(5);
            for (int i = 0; i < 6; ++i) fill({c.x + (i % 3) * c.w / 3 + 6, c.y + (i / 3) * c.h / 2 + 6, c.w / 3 - 12, c.h / 2 - 12}, kPalette[i], 1.0f, 6);
            fill(c, 0x000000, float(dim.value(now)), 6);
            fill(cardIn(c), 0xFFFFFF, float(std::clamp(fade.value(now), 0.0, 1.0)), 10);
        }
        // 6. Desktop switch + parallax
        {
            const Rect c = content(6);
            rt->PushAxisAlignedClip(toD2D(c), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const double p = desktop.value(now);
            const double bgOffset = parallaxOffset(p, c.w, cfg.desktopParallax);
            for (int i = 0; i < 8; ++i)  // background stripes: slow layer
                fill({c.x + bgOffset + i * c.w / 2, c.y, c.w / 4, c.h}, 0x3A3A3C, 1.0f, 0);
            for (int d = 0; d < 3; ++d) {  // content layer: moves a full desktop per step
                const double x = c.x + parallaxOffset(p - d, c.w, 1.0);
                for (int k = 0; k < 3; ++k)
                    fill({x + 20 + k * (c.w - 40) / 3, c.y + 20 + (k % 2) * 30, (c.w - 40) / 3 - 14, c.h * 0.55}, kPalette[d * 3 + k], 1.0f, 6);
                text(L"Desktop " + std::to_wstring(d + 1), {x + 20, c.bottom() - 28, 200, 24}, body.Get());
            }
            rt->PopAxisAlignedClip();
        }
        // 7. Gesture
        {
            const Rect c = content(7);
            const double travel = c.w * 0.6, s = c.h * 0.35, y = c.cy() - s / 2;
            const double x0 = c.x + (c.w - travel - s) / 2;
            fill({x0, y, travel + s, s}, 0x3A3A3C, 1.0f, 10);
            fill({x0 + travel * gesture.value(now), y, s, s}, dragging ? 0xFF375F : 0xBF5AF2, 1.0f, 10);
            wchar_t buf[96];
            swprintf_s(buf, L"progress %.2f  velocity %.2f/s%s", gesture.value(now), tracker.velocity(), userDragged ? L"" : L"  (scripted)");
            text(buf, {c.x, c.bottom() - 22, c.w, 22}, body.Get());
        }
        // 8. Shared transition: draw the expanding tile last so it is on top.
        for (int i = 0; i < 4; ++i)
            if (i != sharedIndex) fill(tiles[i].rect(now), kPalette[i + 2], float(tiles[i].opacity(now)), 8);
        fill(tiles[sharedIndex].rect(now), kPalette[sharedIndex + 2], float(tiles[sharedIndex].opacity(now)), 8);

        // Status line.
        double fps = 0, worst = 0;
        if (frameTimes.size() > 1) {
            fps = (frameTimes.size() - 1) / (frameTimes.back() - frameTimes.front());
            for (std::size_t i = 1; i < frameTimes.size(); ++i) worst = std::max(worst, frameTimes[i] - frameTimes[i - 1]);
        }
        wchar_t status[512];
        swprintf_s(status, L"%.0f FPS, worst frame %.1f ms (last 1 s)   |   R reload config   Esc quit   |   %s", fps, worst * 1000, configStatus.c_str());
        text(status, {12, size.height - 30, size.width - 24, 24}, body.Get(), 0xEBEBF5, 0.7f);

        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) createTarget();
    }

    void createTarget() {
        rt.Reset();
        brush.Reset();
        RECT rc;
        GetClientRect(hwnd, &rc);
        const UINT dpi = GetDpiForWindow(hwnd);
        dpiScale = dpi / 96.0f;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), float(dpi), float(dpi));
        // Pacing comes from the compositor clock, not from Present blocking.
        auto hprops = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom), D2D1_PRESENT_OPTIONS_IMMEDIATELY);
        if (FAILED(d2d->CreateHwndRenderTarget(props, hprops, &rt))) return;
        rt->CreateSolidColorBrush(rgb(0xFFFFFF), &brush);
    }
};

Demo* g_demo = nullptr;

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE:
        if (g_demo && g_demo->rt) g_demo->rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        return 0;
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_demo) g_demo->createTarget();
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        if (g_demo) g_demo->onMouse(m, GET_X_LPARAM(l) / g_demo->dpiScale, GET_Y_LPARAM(l) / g_demo->dpiScale);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) DestroyWindow(h);
        if (w == 'R' && g_demo) g_demo->loadConfig();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    Demo demo;
    g_demo = &demo;
    double seconds = 0;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    demo.configPath = (n && n < MAX_PATH ? std::wstring(local) : L".") + L"\\MissionControl\\config.json";
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--config" && i + 1 < argc) demo.configPath = argv[++i];
        else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
    }
    LocalFree(argv);
    demo.loadConfig();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MCAnimDemo";
    RegisterClassExW(&wc);
    const UINT dpi = GetDpiForSystem();
    demo.hwnd = CreateWindowExW(0, wc.lpszClassName, L"Mission Control — animation engine demo", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                MulDiv(1500, dpi, 96), MulDiv(900, dpi, 96), nullptr, nullptr, inst, nullptr);
    if (!demo.hwnd) return 1;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, demo.d2d.GetAddressOf())) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(demo.dw.GetAddressOf()))))
        return 1;
    demo.dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"", &demo.title);
    demo.dw->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &demo.body);
    demo.createTarget();
    ShowWindow(demo.hwnd, SW_SHOW);

    using WaitClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    auto waitClock = reinterpret_cast<WaitClockFn>(GetProcAddress(LoadLibraryW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));

    demo.start = nowSec();
    for (MSG msg;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const double now = nowSec();
        if (seconds > 0 && now - demo.start > seconds) { DestroyWindow(demo.hwnd); continue; }
        demo.frameTimes.push_back(now);
        while (demo.frameTimes.size() > 2 && demo.frameTimes.front() < now - 1.0) demo.frameTimes.pop_front();
        if (demo.rt) {
            demo.updateLayout();
            demo.tick(now);
            demo.render(now);
        }
        if (waitClock) waitClock(0, nullptr, INFINITE);
        else DwmFlush();
    }
}
