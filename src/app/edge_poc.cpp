#include "app/edge_poc.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "diagnostics/log.h"
#include "rendering/traffic_lights_draw.h"
#include "window/titlebar_profile.h"
#include "window/window_classifier.h"

namespace mc {

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kMsgPlace = WM_APP + 41;
constexpr UINT_PTR kTimerAnim = 1, kTimerColor = 2, kTimerPlace = 3, kTimerGone = 4, kTimerMove = 5;
constexpr UINT kMsgConsider = WM_APP + 43;
constexpr UINT_PTR kTimerRetry = 1, kTimerReorder = 2;
constexpr double kMoveLead = 0.008;  // s: compensates the latency of moving our window (see startMove)
constexpr wchar_t kClass[] = L"MCEdgeTrafficLightsPoc";
constexpr wchar_t kManagerClass[] = L"MCEdgeTrafficLightsManager";
EdgeOverlays* g_overlays = nullptr;  // one instance only; the WinEvent callback has no user data

std::wstring exeNameOf(DWORD pid) {
    std::wstring out;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
            out.assign(buf, n);
            out = out.substr(out.find_last_of(L'\\') + 1);
            for (auto& c : out) c = wchar_t(towlower(c));
        }
        CloseHandle(h);
    }
    return out;
}
}  // namespace

struct EdgePoc::Gdi {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> rt;
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;
    ~Gdi() { release(); }
    void release() {
        if (dc) { SelectObject(dc, old); DeleteDC(dc); dc = nullptr; }
        if (bmp) { DeleteObject(bmp); bmp = nullptr; }
        w = h = 0;
    }
    bool size(int nw, int nh) {
        if (nw == w && nh == h && bmp) return true;
        release();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = nw;
        bi.bmiHeader.biHeight = -nh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dc || !bmp) { release(); return false; }
        old = static_cast<HBITMAP>(SelectObject(dc, bmp));
        w = nw;
        h = nh;
        return true;
    }
};

double EdgePoc::now() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(qpcFreq_.QuadPart);
}

// Windows refuses WM_CLOSE / WM_SYSCOMMAND from a lower-integrity process, so elevated windows are skipped.
// Failing to open the token is treated the same way (it fails for higher-integrity processes).
static bool tokenReadable(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    HANDLE t = nullptr;
    const bool ok = OpenProcessToken(p, TOKEN_QUERY, &t) != 0;
    if (t) CloseHandle(t);
    CloseHandle(p);
    return ok;
}

bool EdgePoc::coverable(HWND h, const Rules& rules, double* heightLogical, bool* maybeLater) {
    if (maybeLater) *maybeLater = false;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || GetParent(h)) return false;  // unowned top-level windows only
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;  // never our own windows (the overview)
    wchar_t cls[64];
    GetClassNameW(h, cls, 64);
    TitleBarFacts facts;
    facts.className = cls;
    facts.exeName = exeNameOf(pid);
    const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    facts.hasCaption = (st & WS_CAPTION) == WS_CAPTION;
    facts.hasSysMenu = (st & WS_SYSMENU) != 0;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    facts.cloaked = cloaked != 0;
    RECT cb{};
    const bool bounds = SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb)) && cb.right > cb.left;
    facts.captionBoundsOk = bounds;
    StripProfile p = stripProfileFor(facts, rules.extraExes, rules.excludeExes);
    if (!p.coverable && !bounds) {  // would it qualify with bounds? then a fresh window may just not be laid out yet
        facts.captionBoundsOk = true;
        if (stripProfileFor(facts, rules.extraExes, rules.excludeExes).coverable && maybeLater) *maybeLater = true;
        return false;
    }
    if (!p.coverable || !tokenReadable(pid)) return false;
    if (heightLogical) *heightLogical = p.heightLogical;
    return true;
}

bool EdgePoc::attach(HINSTANCE inst, HWND target, HWND owner, double heightLogical) {
    if (running() || !target) return false;
    heightLogical_ = heightLogical;
    QueryPerformanceFrequency(&qpcFreq_);
    inst_ = inst;
    target_ = target;
    owner_ = owner;
    GetWindowThreadProcessId(target_, &pid_);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &EdgePoc::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);  // fails harmlessly when registered by an earlier start
    overlay_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
    // Owned by the Edge window: the window manager itself keeps an owned window above its owner, whatever
    // Edge does to its own Z-order (it raises itself with no event when the page is clicked), and hides or
    // minimizes it with the owner. This changes only our window; nothing is written into Edge.
    if (overlay_) {
        SetLastError(0);
        owned_ = SetWindowLongPtrW(overlay_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(target_)) != 0 || GetLastError() == 0;
        if (!owned_) log::write("edge overlay: could not set the owner, falling back to Z-order tracking");
    }
    gdi_ = new Gdi;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f,
        D2D1_RENDER_TARGET_USAGE_NONE);
    if (!overlay_ || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, gdi_->factory.GetAddressOf())) ||
        FAILED(gdi_->factory->CreateDCRenderTarget(&props, &gdi_->rt))) {
        log::write("edge overlay: could not create overlay");
        stop();
        return false;
    }

    if ((process_ = OpenProcess(SYNCHRONIZE, FALSE, pid_)) != nullptr)
        RegisterWaitForSingleObject(&wait_, process_, [](void* hwnd, BOOLEAN) { PostMessageW(static_cast<HWND>(hwnd), kMsgPlace, 0, 0); },
                                    overlay_, INFINITE, WT_EXECUTEONLYONCE);  // pool thread only posts a message
    active_ = GetForegroundWindow() == target_;
    inactive_.jump(active_ ? 0.0 : 1.0);
    place();  // also schedules the color samples
    log::write("edge overlay: attached to %p", static_cast<void*>(target_));
    return true;
}

void EdgePoc::stop() {
    if (wait_) { UnregisterWaitEx(wait_, INVALID_HANDLE_VALUE); wait_ = nullptr; }  // blocks until a running callback returned
    if (process_) { CloseHandle(process_); process_ = nullptr; }
    if (overlay_) {
        KillTimer(overlay_, kTimerAnim);
        KillTimer(overlay_, kTimerColor);
        KillTimer(overlay_, kTimerPlace);
        KillTimer(overlay_, kTimerGone);
        KillTimer(overlay_, kTimerMove);
        HWND w = overlay_;
        overlay_ = nullptr;  // before DestroyWindow: its messages must see a stopped object
        DestroyWindow(w);
    }
    delete gdi_;
    gdi_ = nullptr;
    if (target_) log::write("edge overlay: detached from %p", static_cast<void*>(target_));
    target_ = nullptr;
    animating_ = placePending_ = moving_ = false;
    hot_ = pressed_ = -1;
}

void EdgePoc::requestPlace() {
    if (!overlay_ || placePending_) return;  // coalesce bursts (a window drag fires hundreds of events)
    placePending_ = true;
    PostMessageW(overlay_, kMsgPlace, 0, 0);
}

// The application is active when the foreground window is the target or a window owned by it: Chromium shows its
// menus, bubbles and IME windows as separate top-level windows owned by the browser window, and those can take the
// foreground while the user is still in the browser (measured: with the strict "foreground == target" test the dots
// turned grey as soon as such a window was activated and came back only when it closed).
bool EdgePoc::isActive(HWND fg) const {
    return fg && (fg == target_ || GetAncestor(fg, GA_ROOTOWNER) == target_);
}

void EdgePoc::setActive(bool active) {
    if (active == active_) return;
    active_ = active;
    inactive_.easeTo(active ? 0.0 : 1.0, now(), 0.18, anim::easing::easeInOut);
    scheduleAnimation();
    scheduleColorSamples(80);  // Edge repaints its title bar (active / inactive colors differ) a moment later
    if (active) SetTimer(overlay_, kTimerPlace, 60, nullptr);  // Edge may finish raising itself after the event
}

void EdgePoc::onForeground(HWND fg) {
    if (!overlay_) return;
    setActive(isActive(fg));
    requestPlace();  // any foreground change can reshuffle Z-order around Edge
}

// Edge changes its title bar color a variable time after activation, and a single early sample kept the old
// color (measured: navy patch on a grey inactive bar). Sample at 80 ms and three more times, ending after ~1.4 s.
void EdgePoc::scheduleColorSamples(int firstDelayMs) {
    if (!overlay_) return;
    colorChecks_ = 3;
    SetTimer(overlay_, kTimerColor, UINT(firstDelayMs), nullptr);
}

void EdgePoc::sampleColor() {
    if (rect_.empty() || !IsWindow(target_)) return;
    // The title bar right next to the strip, at its middle height.
    HDC screen = GetDC(nullptr);
    const COLORREF c = GetPixel(screen, int(rect_.x - std::round(4 * dpi_)), int(rect_.cy()));
    ReleaseDC(nullptr, screen);
    if (c != CLR_INVALID) bg_ = c;
}

void EdgePoc::place() {
    placePending_ = false;
    setActive(isActive(GetForegroundWindow()));  // heals a missed or misleading foreground event (place() runs on many events)
    if (!IsWindow(target_)) {
        const HWND gone = target_;
        stop();
        if (owner_) PostMessageW(owner_, kMsgReap, reinterpret_cast<WPARAM>(gone), 0);  // the owner frees us, never this call
        return;
    }
    if (IsIconic(target_)) { ShowWindow(overlay_, SW_HIDE); return; }
    if (!IsWindowVisible(target_)) {
        // A closing Edge window is hidden first and destroyed a moment later with no event we get while
        // other Edge windows keep the process alive: look again a few times, then give up (a later show re-places).
        ShowWindow(overlay_, SW_HIDE);
        if (goneChecks_++ < 5) SetTimer(overlay_, kTimerGone, 300, nullptr);
        return;
    }
    goneChecks_ = 0;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(target_, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    RECT wr{}, cb{}, ef{};
    if (cloaked || !GetWindowRect(target_, &wr) || FAILED(DwmGetWindowAttribute(target_, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb)) ||
        cb.right <= cb.left) {
        ShowWindow(overlay_, SW_HIDE);
        return;
    }
    DwmGetWindowAttribute(target_, DWMWA_EXTENDED_FRAME_BOUNDS, &ef, sizeof ef);
    // Exactly the native button strip, but never into the invisible resize border above the frame.
    // Maximized: the window extends past the screen by the frame width and the buttons start below that border
    // (measured on Edge, Chrome, Explorer, Notepad: window top + 8 px at 96 DPI).
    // Height: Windows-drawn frames match the DWM bounds. Chromium draws 40 logical px buttons while the bounds say
    // 30 (measured with the hover highlight on Close), so its profile carries the real height.
    const UINT dpiUi = GetDpiForWindow(target_);
    const bool zoomedNow = IsZoomed(target_) != 0;
    const LONG frame = LONG(GetSystemMetricsForDpi(SM_CYFRAME, dpiUi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpiUi));
    // The strip lives inside the 1 px accent border Windows draws around a restored window (measured: native Close
    // starts 1 px below the frame at 96 DPI, 2 px at 192): stay inside it on top and on the right, keep the bottom edge.
    const LONG border = zoomedNow ? 0 : std::max<LONG>(1, std::lround(dpiUi / 96.0));
    const LONG topEdge = zoomedNow ? wr.top + frame : std::max<LONG>(wr.top + cb.top, ef.top);
    const LONG bottom = topEdge + (heightLogical_ > 0 ? LONG(std::lround(heightLogical_ * dpiUi / 96.0)) : cb.bottom - cb.top);
    const LONG top = topEdge + border;
    // A window without minimize/maximize buttons (dialogs) gets a narrow strip from Windows, too narrow for three
    // dots: grow it to the width of the standard three-button group, toward the left (empty title bar area).
    LONG left = wr.left + cb.left;
    const LONG right = std::min<LONG>(wr.left + cb.right, ef.right - border), minW = LONG(std::lround(146.0 * dpiUi / 96.0));
    if (right - left < minW) left = std::max<LONG>(right - minW, wr.left);
    const Rect r{double(left), double(top), double(right - left), double(bottom - top)};
    if (r.empty()) { ShowWindow(overlay_, SW_HIDE); return; }
    const float dpi = float(GetDpiForWindow(target_)) / 96.f;
    const bool zoomed = IsZoomed(target_) != 0;
    const LONG_PTR style = GetWindowLongPtrW(target_, GWL_STYLE);
    const bool en[3] = {true, (style & WS_MINIMIZEBOX) != 0, (style & WS_MAXIMIZEBOX) != 0};
    const bool enChanged = !std::equal(std::begin(en), std::end(en), std::begin(enabled_));
    std::copy(std::begin(en), std::end(en), std::begin(enabled_));
    const bool changed = !(r == rect_) || dpi != dpi_ || zoomed != zoomed_ || enChanged || !IsWindowVisible(overlay_);
    // Maximize / restore: the window jumps to its final rect at once while Windows animates a picture of it for
    // ~0.3 s, so a real window placed at the final rect would be ahead of it. Follow the animation instead.
    ANIMATIONINFO ai{sizeof ai};  // "Animate windows when minimizing and maximizing": off = the window just jumps
    const bool windowsAnimate = SystemParametersInfoW(SPI_GETANIMATION, sizeof ai, &ai, 0) && ai.iMinAnimate != 0;
    if (owned_ && windowsAnimate && zoomed != zoomed_ && !rect_.empty() && IsWindowVisible(overlay_) && !moving_) {
        RECT cur{};
        GetWindowRect(overlay_, &cur);
        startMove({double(cur.left), double(cur.top), r.w, r.h}, r);
    } else if (moving_) {
        moveTo_ = r;  // the target moved again (drag during the animation): retarget, keep the progress
    }
    rect_ = r;
    dpi_ = dpi;
    zoomed_ = zoomed;

    // Directly above the target in Z-order, not topmost: windows in front of Edge stay in front of us.
    // Edge's own helper windows (IME, tooltips) are owned by it and sit above it: step over those too.
    auto ownedByTarget = [&](HWND w) {
        for (HWND o = GetWindow(w, GW_OWNER); o; o = GetWindow(o, GW_OWNER))
            if (o == target_) return true;
        return false;
    };
    if (owned_) {
        // While following the animation the tick owns the position; this call only keeps the overlay shown and sized.
        SetWindowPos(overlay_, nullptr, int(r.x), int(r.y), int(r.w), int(r.h), SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW | (moving_ ? SWP_NOMOVE : 0));
    } else {
        HWND above = GetWindow(target_, GW_HWNDPREV);
        while (above && (above == overlay_ || ownedByTarget(above))) above = GetWindow(above, GW_HWNDPREV);
        SetWindowPos(overlay_, above ? above : HWND_TOP, int(r.x), int(r.y), int(r.w), int(r.h), SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (changed) {
        // The title bar color does not change with the geometry, so keep the current color and look again only after
        // the maximize / restore animation is over: a sample taken while it runs hit the animation's grey / black
        // (measured: wrong color at ~100 ms after the click, correct again at ~350 ms).
        scheduleColorSamples(550);
        tl_ = layoutTrafficLightsIn({0, 0, r.w, r.h}, dpi, {});
        render();
    }
}

void EdgePoc::render() {
    if (!gdi_ || !gdi_->rt || !overlay_) return;
    const int w = int(rect_.w), h = int(rect_.h);
    if (w <= 0 || h <= 0 || !gdi_->size(w, h)) return;
    const RECT rc{0, 0, w, h};
    if (FAILED(gdi_->rt->BindDC(gdi_->dc, &rc))) return;
    const double t = now();
    ID2D1RenderTarget* rt = gdi_->rt.Get();
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    ComPtr<ID2D1SolidColorBrush> bg;
    rt->CreateSolidColorBrush(D2D1::ColorF(GetRValue(bg_) / 255.f, GetGValue(bg_) / 255.f, GetBValue(bg_) / 255.f, 1.f), &bg);
    // Restored Win11 windows round their top-right corner: overdraw left/bottom so only that corner shows.
    // The overlay sits inside the 1 px border, so its corner follows the inner curve of the window's 8 px radius.
    const float rad = zoomed_ ? 0.f : std::max(0.f, 8.f * dpi_ - std::max(1.f, std::round(dpi_)));
    if (bg) rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(-rad, 0, float(w), float(h) + rad), rad, rad), bg.Get());
    TrafficButtonLook look[3];
    for (int i = 0; i < 3; ++i) {
        look[i].hover = float(hover_[i].value(t));
        look[i].pressed = float(press_[i].value(t));
        look[i].disabled = !enabled_[i];
    }
    drawTrafficCircles(rt, gdi_->factory.Get(), tl_, 0, 0, dpi_, look, float(inactive_.value(t)));
    if (FAILED(rt->EndDraw())) return;
    HDC screen = GetDC(nullptr);
    POINT src{0, 0};
    SIZE sz{w, h};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(overlay_, screen, nullptr, &sz, gdi_->dc, &src, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void EdgePoc::scheduleAnimation() {
    if (animating_ || !overlay_) return;
    animating_ = true;
    SetTimer(overlay_, kTimerAnim, 15, nullptr);  // only while something is moving
}

void EdgePoc::animate() {
    const double t = now();
    bool settled = inactive_.settled(t);
    for (int i = 0; i < 3; ++i) settled = settled && hover_[i].settled(t) && press_[i].settled(t);
    render();
    if (settled) {
        KillTimer(overlay_, kTimerAnim);
        animating_ = false;
    }
}

// Measured by capturing a bright test window at ~30 fps (maximize and restore alike): the picture starts moving
// after ~70 ms and covers its distance in ~200 ms with a strong ease-out; fitted with cubic-bezier(0.5, 0.2, 0, 1)
// (rmse 2% of the travel). Our own window lags the animation by about a frame (timer + present), so it starts
// kMoveLead earlier than the fit.
void EdgePoc::startMove(const Rect& from, const Rect& to) {
    moveFrom_ = from;
    moveTo_ = to;
    moving_ = true;
    moveP_.jump(0.0);
    moveP_.easeTo(1.0, now(), 0.200, anim::CubicBezier{0.5, 0.2, 0.0, 1.0}, 0.070 - kMoveLead);
    SetTimer(overlay_, kTimerMove, 4, nullptr);  // runs only for this transition (about 0.26 s)
    moveTick();
}

void EdgePoc::moveTick() {
    if (!moving_ || !overlay_) return;
    const double t = now(), p = moveP_.value(t);
    const bool done = moveP_.settled(t);
    const Rect at = done ? moveTo_ : Rect{lerp(moveFrom_.x, moveTo_.x, p), lerp(moveFrom_.y, moveTo_.y, p), moveTo_.w, moveTo_.h};
    SetWindowPos(overlay_, nullptr, int(std::lround(at.x)), int(std::lround(at.y)), int(moveTo_.w), int(moveTo_.h),
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    if (done) {
        KillTimer(overlay_, kTimerMove);
        moving_ = false;
    }
}

void EdgePoc::onHover(int index) {
    if (index >= 0 && !enabled_[index]) index = -1;  // a disabled dot does not react
    if (index == hot_) return;
    const double t = now();
    if (hot_ >= 0) hover_[hot_].easeTo(0.0, t, 0.12, anim::easing::easeOut);
    if (index >= 0) hover_[index].easeTo(1.0, t, 0.12, anim::easing::easeOut);
    hot_ = index;
    scheduleAnimation();
}

void EdgePoc::onPress(int index, bool down) {
    if (index < 0 || !enabled_[index]) return;
    press_[index].easeTo(down ? 1.0 : 0.0, now(), 0.09, anim::easing::easeOut);
    scheduleAnimation();
}

// Goes to the exact target window. PostMessage so a hung Edge never blocks the app.
void EdgePoc::act(int button) {
    if (!IsWindow(target_) || !enabled_[button]) return;
    log::write("edge overlay: button %d", button);
    switch (static_cast<TrafficButton>(button)) {
    case TrafficButton::Close: PostMessageW(target_, WM_CLOSE, 0, 0); break;
    case TrafficButton::Minimize: PostMessageW(target_, WM_SYSCOMMAND, SC_MINIMIZE, 0); break;
    case TrafficButton::Zoom: PostMessageW(target_, WM_SYSCOMMAND, IsZoomed(target_) ? SC_RESTORE : SC_MAXIMIZE, 0); break;
    }
}

LRESULT CALLBACK EdgePoc::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<EdgePoc*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->onMessage(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT EdgePoc::onMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!overlay_) return DefWindowProcW(h, msg, wp, lp);  // creating or stopped
    switch (msg) {
    case kMsgPlace:
        place();
        return 0;
    case WM_TIMER:
        if (wp == kTimerAnim) animate();
        else if (wp == kTimerMove) moveTick();
        else if (wp == kTimerPlace) { KillTimer(h, kTimerPlace); place(); }
        else if (wp == kTimerGone) { KillTimer(h, kTimerGone); place(); }
        else if (wp == kTimerColor) {
            KillTimer(h, kTimerColor);
            const COLORREF before = bg_;
            sampleColor();
            if (bg_ != before) render();
            if (colorChecks_-- > 0) SetTimer(h, kTimerColor, 220 * (3 - colorChecks_), nullptr);  // 220, 440, 660 ms later
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // never take focus from Edge
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        onHover(tl_.hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (pressed_ < 0) onHover(-1);
        return 0;
    case WM_LBUTTONDOWN:
        pressed_ = tl_.hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (pressed_ >= 0 && !enabled_[pressed_]) pressed_ = -1;  // disabled dot: swallow the click, do nothing
        if (pressed_ >= 0) { SetCapture(h); onPress(pressed_, true); }
        return 0;  // the rest of the strip swallows the click: the native buttons below stay unreachable
    case WM_LBUTTONUP: {
        const int was = pressed_, over = tl_.hitTest(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        pressed_ = -1;
        if (GetCapture() == h) ReleaseCapture();
        onPress(was, false);
        onHover(over);
        if (was >= 0 && was == over) act(was);  // released on the same button
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ >= 0) { onPress(pressed_, false); pressed_ = -1; }
        return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------- EdgeOverlays ----------

bool EdgeOverlays::start(HINSTANCE inst, const std::wstring& titleContains, EdgePoc::Rules rules) {
    if (running()) return true;
    inst_ = inst;
    title_ = titleContains;
    rules_ = std::move(rules);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &EdgeOverlays::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kManagerClass;
    RegisterClassExW(&wc);  // fails harmlessly when registered by an earlier start
    msg_ = CreateWindowExW(0, kManagerClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
    if (!msg_) {
        log::write("edge overlays: could not create the message window");
        return false;
    }
    g_overlays = this;
    // Two global hooks, both rare: a window being shown (new Edge windows) and the foreground change.
    showHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &EdgeOverlays::eventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &EdgeOverlays::eventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {  // one pass at start
            reinterpret_cast<EdgeOverlays*>(lp)->consider(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
    log::write("edge overlays: started, %zu window(s)", overlays_.size());
    return true;
}

void EdgeOverlays::stop() {
    if (!msg_) return;
    if (showHook_) { UnhookWinEvent(showHook_); showHook_ = nullptr; }
    if (foregroundHook_) { UnhookWinEvent(foregroundHook_); foregroundHook_ = nullptr; }
    if (reorderHook_) { UnhookWinEvent(reorderHook_); reorderHook_ = nullptr; }
    for (auto& [pid, hooks] : pidHooks_)
        for (auto& h : hooks.hook)
            if (h) UnhookWinEvent(h);
    pidHooks_.clear();
    overlays_.clear();  // each overlay releases its window and objects
    retries_.clear();
    HWND w = msg_;
    msg_ = nullptr;  // before DestroyWindow: its messages must see a stopped object
    DestroyWindow(w);
    if (g_overlays == this) g_overlays = nullptr;
    log::write("edge overlays: stopped");
}

void EdgeOverlays::consider(HWND w) {
    if (!running() || overlays_.count(w) || !IsWindow(w)) return;
    bool later = false;
    double heightLogical = 0;
    if (!EdgePoc::coverable(w, rules_, &heightLogical, &later)) {
        if (later && ++retries_[w] <= 3) SetTimer(msg_, kTimerRetry, 150, nullptr);  // caption buttons not laid out yet
        else retries_.erase(w);
        return;
    }
    retries_.erase(w);
    if (!title_.empty()) {
        wchar_t text[512];
        GetWindowTextW(w, text, 512);
        if (!wcsstr(text, title_.c_str())) return;
    }
    auto poc = std::make_unique<EdgePoc>();
    if (!poc->attach(inst_, w, msg_, heightLogical)) return;
    addPidHooks(poc->pid());
    overlays_.emplace(w, std::move(poc));
    watchReorder();
}

void EdgeOverlays::onForeground(HWND fg) {
    for (auto& [w, poc] : overlays_) poc->onForeground(fg);
    consider(fg);  // an Edge window that was cloaked or not ready when it was shown
    if (overlays_.count(fg)) watchReorder();
}

// Edge raises a window it just created or activated above the overlay a moment later (measured: 0.3-1.2 s
// after the window appeared), and that Z change comes with no event of its own for that window. Z-order
// changes of top-level windows do raise EVENT_OBJECT_REORDER, but system-wide and constantly (about 20 per
// second while idle on the test machine), so it is only listened to for a short time after an Edge window
// appeared or was activated, never while idle.
void EdgeOverlays::watchReorder() {
    if (!reorderHook_)
        reorderHook_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, &EdgeOverlays::eventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    SetTimer(msg_, kTimerReorder, 2500, nullptr);  // restarts the countdown when armed again
}

void EdgeOverlays::addPidHooks(DWORD pid) {
    PidHooks& p = pidHooks_[pid];
    if (p.refs++ > 0) return;
    // Edge process only: location changes are far too chatty to hook system-wide.
    // CLOAKED / UNCLOAKED: the shell keeps a new window cloaked for a moment, and cloaks windows of other
    // virtual desktops; without the uncloak event a window shown that way would stay without its overlay.
    const struct { DWORD lo, hi; } ranges[4] = {
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}};
    for (int i = 0; i < 4; ++i)
        p.hook[i] = SetWinEventHook(ranges[i].lo, ranges[i].hi, nullptr, &EdgeOverlays::eventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
}

void EdgeOverlays::releasePidHooks(DWORD pid) {
    auto it = pidHooks_.find(pid);
    if (it == pidHooks_.end() || --it->second.refs > 0) return;
    for (auto& h : it->second.hook)
        if (h) UnhookWinEvent(h);
    pidHooks_.erase(it);
}

void CALLBACK EdgeOverlays::eventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    EdgeOverlays* m = g_overlays;
    if (!m || !m->msg_) return;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        m->onForeground(hwnd);
        return;
    }
    if (event == EVENT_OBJECT_REORDER) {
        for (auto& [w, poc] : m->overlays_) poc->requestPlace();  // coalesced per overlay
        return;
    }
    if (!hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    auto it = m->overlays_.find(hwnd);
    if (it != m->overlays_.end()) {  // an Edge window we cover: re-place it
        it->second->requestPlace();
        return;
    }
    if (event != EVENT_OBJECT_SHOW) return;
    // Cheap filter before anything else: only top-level windows with a title bar are candidates; the real checks
    // (app profile, elevation, bounds) run on the UI thread in consider().
    const LONG_PTR st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((st & WS_CAPTION) == WS_CAPTION && !(st & WS_CHILD)) PostMessageW(m->msg_, kMsgConsider, reinterpret_cast<WPARAM>(hwnd), 0);
}

LRESULT CALLBACK EdgeOverlays::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<EdgeOverlays*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->onMessage(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT EdgeOverlays::onMessage(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!msg_) return DefWindowProcW(h, msg, wp, lp);  // creating or stopped
    switch (msg) {
    case kMsgConsider:
        consider(reinterpret_cast<HWND>(wp));
        return 0;
    case EdgePoc::kMsgReap: {
        auto it = overlays_.find(reinterpret_cast<HWND>(wp));
        if (it != overlays_.end() && !it->second->running()) {  // a reused handle may already have a live overlay
            const DWORD pid = it->second->pid();
            overlays_.erase(it);
            releasePidHooks(pid);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerReorder) {
            KillTimer(h, kTimerReorder);
            if (reorderHook_) { UnhookWinEvent(reorderHook_); reorderHook_ = nullptr; }
        } else if (wp == kTimerRetry) {
            KillTimer(h, kTimerRetry);
            std::vector<HWND> pending;
            for (auto& [w, n] : retries_) pending.push_back(w);
            for (HWND w : pending) consider(w);  // re-arms the timer when one is still not ready
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace mc
