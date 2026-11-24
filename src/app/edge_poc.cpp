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
#include "window/app_registry.h"
#include "window/compat_engine.h"
#include "window/titlebar_profile.h"
#include "window/window_classifier.h"

namespace mc {

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kMsgPlace = WM_APP + 41;
constexpr UINT_PTR kTimerAnim = 1, kTimerColor = 2, kTimerPlace = 3, kTimerGone = 4, kTimerMove = 5;
constexpr UINT kMsgConsider = WM_APP + 43;
constexpr UINT kMsgPidGone = WM_APP + 44;  // a covered process exited (posted by the process wait)
constexpr UINT_PTR kTimerRetry = 1, kTimerReorder = 2;
constexpr double kMoveLead = 0.008;  // s: compensates the latency of moving our window (see startMove)
constexpr wchar_t kClass[] = L"MCEdgeTrafficLightsPoc";
constexpr wchar_t kManagerClass[] = L"MCEdgeTrafficLightsManager";
EdgeOverlays* g_overlays = nullptr;  // one instance only; the WinEvent callback has no user data

std::wstring exePathOf(DWORD pid) {
    std::wstring out;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = DWORD(std::size(buf));
        if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
        CloseHandle(h);
    }
    return out;
}

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

// One Direct2D factory for every overlay: a factory per window cost about a megabyte each (measured: 30 overlays
// went from 44 to 88 MB). Used on the UI thread only.
static ComPtr<ID2D1Factory>& sharedFactory() {
    static ComPtr<ID2D1Factory> f;
    return f;
}
static bool ensureSharedFactory() {
    auto& f = sharedFactory();
    return f || SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, f.GetAddressOf()));
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

// ---------- strip probe ----------
// Where does the window say its caption buttons are? Two sources, neither needs code in the other process:
//  * DWMWA_CAPTION_BUTTON_BOUNDS (exact for frames drawn by Windows, wrong for apps that draw their own buttons);
//  * WM_NCHITTEST, a query message the system itself sends on every mouse move, answered with HTMINBUTTON /
//    HTMAXBUTTON / HTCLOSE. Sent with a 60 ms timeout so a hung app never blocks us, a few dozen messages once per
//    window (and again on zoom / DPI change). Never sent to windows the user turned off.
// Measured against the real buttons (docs/compatibility/edge.md, "Geometry sources").
static int hitTestAt(HWND h, int x, int y, bool* timedOut) {
    DWORD_PTR res = 0;
    const LRESULT ok = SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM(x & 0xFFFF, y & 0xFFFF), SMTO_ABORTIFHUNG, 60, &res);
    *timedOut = ok == 0;
    return int(res);
}

static Probe probeStrip(HWND h) {
    Probe p;
    const double d = double(GetDpiForWindow(h)) / 96.0;
    RECT cb{};
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb)) && cb.right > cb.left)
        p.dwm = {true, (cb.right - cb.left) / d, (cb.bottom - cb.top) / d};
    RECT ef{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &ef, sizeof ef)) && !GetWindowRect(h, &ef)) return p;
    // One row through the buttons, from the right edge leftwards: where do close / maximize / minimize answer?
    const int y = ef.top + int(std::lround(12 * d));
    int closeL = -1, closeR = -1, maxL = -1, minL = -1, timeouts = 0;
    for (int x = ef.right - 1; x > ef.right - int(std::lround(240 * d)); x -= 2) {
        bool to = false;
        const int r = hitTestAt(h, x, y, &to);
        if (to) { if (++timeouts >= 2) return p; continue; }
        if (r == HTCLOSE) { if (closeR < 0) closeR = x; closeL = x; }
        else if (r == HTMAXBUTTON) maxL = x;
        else if (r == HTMINBUTTON) minL = x;
        else if (minL >= 0) break;  // left the buttons after the minimize button
    }
    if (closeL < 0 || maxL < 0 || minL < 0) return p;
    // One column through the middle of the close button: top and bottom of the buttons.
    const int cx = (closeL + closeR) / 2;
    int top = -1, bottom = -1;
    for (int yy = ef.top; yy < ef.top + int(std::lround(90 * d)); yy += 2) {
        bool to = false;
        const int r = hitTestAt(h, cx, yy, &to);
        if (to) break;
        if (r == HTCLOSE) { if (top < 0) top = yy; bottom = yy; }
        else if (top >= 0) break;
    }
    if (top < 0) return p;
    p.hit = {true, (ef.right - minL) / d, (bottom + 2 - ef.top) / d, (top - ef.top) / d};
    return p;
}

bool EdgePoc::evaluate(HWND h, const Rules& rules, Cover* out, bool* maybeLater) {
    if (maybeLater) *maybeLater = false;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || GetParent(h)) return false;  // unowned top-level windows only
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false;  // never our own windows (the overview)
    wchar_t cls[64], title[256];
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, title, 256);
    TitleBarFacts facts;
    facts.className = cls;
    facts.exeName = exeNameOf(pid);
    DWORD appPid = pid;
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        // A UWP app's frame belongs to ApplicationFrameHost.exe; the app itself is the process of its CoreWindow child.
        struct Find { DWORD pid = 0; } f;
        EnumChildWindows(h, [](HWND c, LPARAM lp) -> BOOL {
            wchar_t cc[64];
            GetClassNameW(c, cc, 64);
            if (wcscmp(cc, L"Windows.UI.Core.CoreWindow") != 0) return TRUE;
            GetWindowThreadProcessId(c, &reinterpret_cast<Find*>(lp)->pid);
            return FALSE;
        }, reinterpret_cast<LPARAM>(&f));
        if (f.pid) { appPid = f.pid; facts.exeName = exeNameOf(f.pid); }
    }
    const LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
    facts.hasCaption = (st & WS_CAPTION) == WS_CAPTION;
    facts.hasSysMenu = (st & WS_SYSMENU) != 0;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    facts.cloaked = cloaked != 0;
    RECT cb{};
    facts.captionBoundsOk = SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb)) && cb.right > cb.left;

    // The user's settings come first: nothing is probed for a window that is turned off.
    if (out) {
        out->exe = facts.exeName;
        out->path = exePathOf(appPid);
        out->className = cls;
        out->title = title;
        out->pid = pid;
    }
    const Policy policy = resolvePolicy(rules.data, facts.exeName, cls, title);
    if (policy.enabled && !*policy.enabled) {
        if (out) out->policyOff = true;
        return false;
    }
    Probe probe;
    Assessment a = assess(facts, probe);
    if (a.source != StripSource::Verified && a.verdict != Verdict::Excluded && facts.hasCaption) {
        probe = probeStrip(h);
        a = assess(facts, probe);
    }
    const Decision dec = decide(a, policy, rules.data.mode, isListedApp(facts.exeName));
    if (out) {
        out->a = a;
        out->why = dec.why;
        out->heightLogical = policy.heightLogical ? *policy.heightLogical : a.heightLogical;
    }
    static const char* const verdicts[] = {"Supported", "Partial", "Unsupported", "Excluded"};
    static const char* const sources[] = {"none", "verified profile", "dwm+hit-test", "hit-test", "dwm bounds"};
    log::write("compat: %ls [%ls] %s via %s -> %s (%s)", facts.exeName.c_str(), cls, verdicts[int(a.verdict)], sources[int(a.source)],
               dec.cover ? "cover" : "keep native", dec.why);
    if (!dec.cover) {
        // Nothing located at all on a window that looks normal: it may just not be laid out yet.
        if (maybeLater && a.verdict == Verdict::Unsupported && facts.hasCaption && !probe.dwm.ok && !probe.hit.ok) *maybeLater = true;
        return false;
    }
    if (!tokenReadable(pid)) return false;
    return true;
}

bool EdgePoc::attach(HINSTANCE inst, HWND target, HWND owner, const Cover& cover) {
    if (running() || !target) return false;
    assess_ = cover.a;
    exe_ = cover.exe;
    heightLogical_ = cover.heightLogical;
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
    if (!overlay_ || !ensureSharedFactory() || (gdi_->factory = sharedFactory(), FAILED(gdi_->factory->CreateDCRenderTarget(&props, &gdi_->rt)))) {
        log::write("edge overlay: could not create overlay");
        stop();
        return false;
    }

    active_ = GetForegroundWindow() == target_;
    inactive_.jump(active_ ? 0.0 : 1.0);
    place();  // also schedules the color samples
    log::write("edge overlay: attached to %p", static_cast<void*>(target_));
    return true;
}

void EdgePoc::stop() {
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
    HDC screen = GetDC(nullptr);
    // Near the top of the bar: icons, avatars and tabs sit in its middle (measured on Warp: an avatar next to the buttons).
    const COLORREF c = GetPixel(screen, int(rect_.x - std::round(4 * dpi_)), int(rect_.y + std::round(4 * dpi_)));
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
    const bool haveCb = SUCCEEDED(DwmGetWindowAttribute(target_, DWMWA_CAPTION_BUTTON_BOUNDS, &cb, sizeof cb)) && cb.right > cb.left;
    if (cloaked || !GetWindowRect(target_, &wr) || (!haveCb && !assess_.useHit && !assess_.useFixed)) {
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
    const double dd = double(dpiUi) / 96.0;
    const bool zoomedNow = IsZoomed(target_) != 0;
    const LONG frame = LONG(GetSystemMetricsForDpi(SM_CYFRAME, dpiUi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpiUi));
    // The strip lives inside the 1 px accent border Windows draws around a restored window (measured: native Close
    // starts 1 px below the frame at 96 DPI, 2 px at 192): stay inside it on top and on the right, keep the bottom edge.
    const LONG border = zoomedNow ? 0 : std::max<LONG>(1, std::lround(dpiUi / 96.0));
    const LONG minW = LONG(std::lround(146.0 * dpiUi / 96.0));
    LONG top, bottom, left, right;
    if (assess_.useFixed) {
        // A measured app that reports nothing: the strip sits at a fixed place from the frame's right and top edges.
        bottom = ef.top + LONG(std::lround(heightLogical_ * dd));
        top = ef.top + border;
        left = ef.right - LONG(std::lround(assess_.fixedLeftFromRight * dd));
        right = ef.right - std::max<LONG>(border, LONG(std::lround(assess_.fixedRightGap * dd)));
    } else if (assess_.useHit) {
        // Windows reports no caption bounds (VS Code, UWP): the buttons are where the window's hit-test says. The
        // hit-test geometry depends on zoom and DPI, so it is measured again when either changes.
        if (hitDpi_ != dpiUi || hitZoomed_ != zoomedNow) {
            const Probe pr = probeStrip(target_);
            if (pr.hit.ok) {
                assess_.hitLeftFromRight = pr.hit.leftFromRight;
                assess_.hitHeight = pr.hit.height;
                assess_.hitTop = pr.hit.top;
            }
            hitDpi_ = dpiUi;
            hitZoomed_ = zoomedNow;
        }
        bottom = ef.top + LONG(std::lround(assess_.hitHeight * dd));
        top = zoomedNow ? ef.top + LONG(std::lround(assess_.hitTop * dd)) : ef.top + border;
        left = ef.right - LONG(std::lround(assess_.hitLeftFromRight * dd));
        right = ef.right - border;
        if (right - left < minW) left = std::max<LONG>(right - minW, wr.left);
    } else {
        const LONG topEdge = zoomedNow ? wr.top + frame : std::max<LONG>(wr.top + cb.top, ef.top);
        bottom = topEdge + (heightLogical_ > 0 ? LONG(std::lround(heightLogical_ * dpiUi / 96.0)) : cb.bottom - cb.top);
        top = topEdge + border;
        // A window without minimize/maximize buttons (dialogs) gets a narrow strip from Windows, too narrow for three
        // dots: grow it to the width of the standard three-button group, toward the left (empty title bar area).
        left = wr.left + cb.left;
        right = std::min<LONG>(wr.left + cb.right, ef.right - border);
        if (right - left < minW) left = std::max<LONG>(right - minW, wr.left);
    }
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
    for (auto& [pid, hooks] : pidHooks_) {
        for (auto& h : hooks.hook)
            if (h) UnhookWinEvent(h);
        freePidWait(hooks);
    }
    pidHooks_.clear();
    overlays_.clear();  // each overlay releases its window and objects
    sharedFactory().Reset();
    retries_.clear();
    rejected_.clear();
    seen_.clear();
    HWND w = msg_;
    msg_ = nullptr;  // before DestroyWindow: its messages must see a stopped object
    DestroyWindow(w);
    if (g_overlays == this) g_overlays = nullptr;
    log::write("edge overlays: stopped");
}

void EdgeOverlays::consider(HWND w) {
    if (!running() || overlays_.count(w) || !IsWindow(w)) return;
    if (auto it = rejected_.find(w); it != rejected_.end() && GetTickCount64() - it->second < 60000) return;  // judged recently
    if (!title_.empty()) {
        wchar_t text[512];
        GetWindowTextW(w, text, 512);
        if (!wcsstr(text, title_.c_str())) return;
    }
    bool later = false;
    EdgePoc::Cover cover;
    const bool ok = EdgePoc::evaluate(w, rules_, &cover, &later);
    if (!later) noteApp(cover);
    if (!ok) {
        if (later && ++retries_[w] <= 3) {
            SetTimer(msg_, kTimerRetry, 150, nullptr);  // caption buttons not laid out yet
        } else {
            retries_.erase(w);
            if (rejected_.size() >= 256)  // bounded: forget windows that are gone, then the oldest verdicts
                std::erase_if(rejected_, [](const auto& kv) { return !IsWindow(kv.first); });
            if (rejected_.size() >= 256) rejected_.clear();
            rejected_[w] = GetTickCount64();
        }
        return;
    }
    retries_.erase(w);
    rejected_.erase(w);
    auto poc = std::make_unique<EdgePoc>();
    if (!poc->attach(inst_, w, msg_, cover)) return;
    addPidHooks(poc->pid());
    overlays_.emplace(w, std::move(poc));
    watchReorder();
}

void EdgeOverlays::noteApp(const EdgePoc::Cover& c) {
    if (c.exe.empty()) return;  // filtered before the process was looked at
    if (seen_.size() >= 512 && !seen_.count(c.exe)) {  // bounded: drop the app seen longest ago
        auto oldest = std::min_element(seen_.begin(), seen_.end(), [](const auto& a, const auto& b) { return a.second.lastSeen < b.second.lastSeen; });
        seen_.erase(oldest);
    }
    AppInfo& i = seen_[c.exe];
    i.exe = c.exe;
    i.lastSeen = GetTickCount64();
    if (c.policyOff) {  // a window turned off is not assessed: keep the last verdict, refresh only what the window shows
        if (i.path.empty()) i.path = c.path;
        return;
    }
    // One app has several windows (Explorer also owns hidden shell windows): keep the verdict of its best window,
    // and describe that window, so a hidden helper window does not turn "Supported" into "Not supported".
    auto rank = [](Verdict v) { return v == Verdict::Supported ? 3 : v == Verdict::Partial ? 2 : v == Verdict::Unsupported ? 1 : 0; };
    if (!i.known || rank(c.a.verdict) >= rank(i.verdict)) {
        i.path = c.path;
        i.className = c.className;
        i.title = c.title;
        i.pid = c.pid;
        i.verdict = c.a.verdict;
        i.source = c.a.source;
        i.reason = c.why;
        i.heightLogical = c.heightLogical;
        i.known = true;
    }
}

std::vector<AppInfo> EdgeOverlays::apps() const {
    std::vector<AppInfo> out;
    out.reserve(seen_.size());
    for (const auto& [exe, info] : seen_) {
        AppInfo a = info;
        for (const auto& [w, poc] : overlays_)
            if (poc->exe() == exe) ++a.windows;
        out.push_back(std::move(a));
    }
    return out;
}

void EdgeOverlays::applyRules(EdgePoc::Rules rules) {
    if (!running()) return;
    rules_ = std::move(rules);
    rejected_.clear();
    retries_.clear();
    std::vector<HWND> drop;
    for (auto& [w, poc] : overlays_) {
        EdgePoc::Cover c;
        if (!EdgePoc::evaluate(w, rules_, &c)) drop.push_back(w);
    }
    for (HWND w : drop) {
        const DWORD pid = overlays_[w]->pid();
        overlays_.erase(w);  // the overlay releases its window
        releasePidHooks(pid);
    }
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            reinterpret_cast<EdgeOverlays*>(lp)->consider(h);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(this));
}

void EdgeOverlays::onForeground(HWND fg) {
    if (fg && IsWindow(fg) && (GetWindowLongPtrW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) {  // for "turn off for this app"
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid && pid != GetCurrentProcessId()) {
            const std::wstring exe = exeNameOf(pid);
            if (!exe.empty() && !isSystemShellProcess(exe)) lastApp_ = exe;
        }
    }
    for (auto& [w, poc] : overlays_) poc->onForeground(fg);
    // A window that was cloaked or not ready (no caption bounds yet) when it was shown was cached as rejected for 60 s;
    // the user activating it is a new chance (Tauri windows such as DBX settle after their first show).
    if (fg) rejected_.erase(fg);
    consider(fg);
    if (overlays_.count(fg)) watchReorder();
}

// Edge raises a window it just created or activated above the overlay a moment later (measured: 0.3-1.2 s
// after the window appeared), and that Z change comes with no event of its own for that window. Z-order
// changes of top-level windows do raise EVENT_OBJECT_REORDER, but system-wide and constantly (about 20 per
// second while idle on the test machine), so it is only listened to for a short time after an Edge window
// appeared or was activated, never while idle.
void EdgeOverlays::watchReorder() {
    // Overlays are owned windows: the window manager keeps them above their target, so Z-order needs no events.
    // Only an overlay whose owner could not be set still needs them.
    const bool needed = std::any_of(overlays_.begin(), overlays_.end(), [](const auto& kv) { return !kv.second->owned(); });
    if (!needed) return;
    if (!reorderHook_)
        reorderHook_ = SetWinEventHook(EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, nullptr, &EdgeOverlays::eventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    SetTimer(msg_, kTimerReorder, 2500, nullptr);  // restarts the countdown when armed again
}

namespace {
struct PidWaitCtx {
    HWND msg;
    DWORD pid;
};
VOID CALLBACK pidExited(PVOID ctx, BOOLEAN) {  // pool thread: only posts a message
    const auto* c = static_cast<PidWaitCtx*>(ctx);
    PostMessageW(c->msg, kMsgPidGone, WPARAM(c->pid), 0);
}
}  // namespace

void EdgeOverlays::freePidWait(PidHooks& p) {
    if (p.wait) { UnregisterWaitEx(p.wait, INVALID_HANDLE_VALUE); p.wait = nullptr; }  // blocks until a running callback returned
    if (p.process) { CloseHandle(p.process); p.process = nullptr; }
    delete static_cast<PidWaitCtx*>(p.waitCtx);
    p.waitCtx = nullptr;
}

void EdgeOverlays::addPidHooks(DWORD pid) {
    PidHooks& p = pidHooks_[pid];
    if (p.refs++ > 0) return;
    // Target process only: location changes are far too chatty to hook system-wide.
    // CLOAKED / UNCLOAKED: the shell keeps a new window cloaked for a moment, and cloaks windows of other
    // virtual desktops; without the uncloak event a window shown that way would stay without its overlay.
    const struct { DWORD lo, hi; } ranges[4] = {
        {EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}};
    for (int i = 0; i < 4; ++i)
        p.hook[i] = SetWinEventHook(ranges[i].lo, ranges[i].hi, nullptr, &EdgeOverlays::eventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
    // One wait per process, not per window: wakes us when the process exits (no DESTROY event arrives then).
    if ((p.process = OpenProcess(SYNCHRONIZE, FALSE, pid)) != nullptr) {
        p.waitCtx = new PidWaitCtx{msg_, pid};
        if (!RegisterWaitForSingleObject(&p.wait, p.process, pidExited, p.waitCtx, INFINITE, WT_EXECUTEONLYONCE)) freePidWait(p);
    }
}

void EdgeOverlays::releasePidHooks(DWORD pid) {
    auto it = pidHooks_.find(pid);
    if (it == pidHooks_.end() || --it->second.refs > 0) return;
    for (auto& h : it->second.hook)
        if (h) UnhookWinEvent(h);
    freePidWait(it->second);
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
    case kMsgPidGone:
        for (auto& [w, poc] : overlays_)
            if (poc->pid() == DWORD(wp)) poc->requestPlace();  // each notices its window is gone and asks to be reaped
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
