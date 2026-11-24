#include "app/app.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <shellscalingapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

#include "app/autostart.h"
#include "app/overlay_window.h"
#include "diagnostics/log.h"

namespace mc {

using Microsoft::WRL::ComPtr;

namespace {

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    std::wstring w(utf8.size(), L' ');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), w.data(), int(w.size())));
    return w;
}
constexpr int kHotkeyId = 1, kHotkeyBridgeWin = 2, kHotkeyBridge = 3;
constexpr UINT_PTR kTimerBenchOpen = 1, kTimerBenchClose = 2, kTimerRefresh = 3, kTimerScrubEnd = 4, kTimerSwipeLock = 5;
constexpr double kScrubTravel = 600.0;  // wheel units for a full scrub (5 mouse-wheel notches)
constexpr int kSwipeThreshold = 240;    // horizontal wheel units for one desktop step
App* g_app = nullptr;  // single instance; needed by the WinEvent callback
}  // namespace

int App::run(HINSTANCE inst, const Options& opt) {
    opt_ = opt;
    g_app = this;
    loadConfig();
    if (!opt_.benchClass.empty()) rules_.allowClasses = {opt_.benchClass};

    wm_.init();
    overlay_ = createOverlayWindow(inst, &App::wndProc, this);
    if (!overlay_) return 1;
    startCompositor();

    // Ctrl+Alt+Space always works (fallback when Win+Tab takeover is off or unavailable).
    // Bench runs leave every hotkey and the hook to the user's own instance.
    if (opt_.benchCycles == 0) {
        if (!RegisterHotKey(overlay_, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE))
            log::lastError("RegisterHotKey(Ctrl+Alt+Space)");
        if (cfg_.takeoverWinTab && !opt_.noTakeover) {
            // The hook turns Win+Tab into Ctrl+Alt+F24 (plus Win while the user holds it); both are ours.
            const bool bridge = RegisterHotKey(overlay_, kHotkeyBridgeWin, MOD_WIN | MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, KeyboardHook::kBridgeKey) &&
                                RegisterHotKey(overlay_, kHotkeyBridge, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, KeyboardHook::kBridgeKey);
            if (!bridge) log::lastError("RegisterHotKey(bridge Ctrl+Alt+F24): Win+Tab takeover disabled");
            else if (!hook_.start(opt_.hookAcceptInjected)) log::write("Win+Tab takeover unavailable: hook not installed");
        } else {
            log::write("Win+Tab takeover off (%s)", opt_.noTakeover ? "--no-takeover" : "config");
        }
        tray_.add(overlay_, inst);
    }

    if (desktops_.init()) desktopState_ = desktops_.read();
    loadUserApps();
    // The user's own switch (settings window / tray) wins over config.json.
    if (userApps_.featureEnabled.value_or(cfg_.edgePocEnabled) && opt_.benchCycles == 0) edgeOverlays_.start(inst, widen(cfg_.edgePocTitle), overlayRules());
    if (opt_.settings) openSettings();

    benchStart_ = GetTickCount64();
    if (opt_.benchCycles > 0 || opt_.show) open();

    // Event-driven loop: window messages, plus the registry event that signals desktop changes.
    for (;;) {
        HANDLE ev = desktops_.changeEvent();
        const DWORD r = MsgWaitForMultipleObjectsEx(ev ? 1 : 0, &ev, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ev && r == WAIT_OBJECT_0) {
            desktops_.rearm();  // re-arm before reading so no change is missed
            onDesktopChanged();
        }
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            if (settings_.preTranslate(&msg)) continue;  // Tab / Esc inside the settings window
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
    }

    // Shutdown order: keyboard hook (Win+Tab back to Windows first) -> compositor (thumbnails, GPU) -> hooks.
    // The overlay is already destroyed.
    settings_.close();
    edgeOverlays_.stop();
    hook_.stop();
    comp_.stop();
    unhookWindowEvents();
    g_app = nullptr;
    return 0;
}

void App::hookWindowEvents() {
    if (!hooks_.empty()) return;
    // Two narrow ranges: destroy/show/hide and cloak/uncloak. Location changes are excluded on
    // purpose: they fire on every mouse move.
    for (auto [first, last] : {std::pair{EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE}, std::pair{EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}})
        if (HWINEVENTHOOK h = SetWinEventHook(first, last, nullptr, &App::winEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS))
            hooks_.push_back(h);
        else
            log::lastError("SetWinEventHook");
}

void App::unhookWindowEvents() {
    for (HWINEVENTHOOK h : hooks_) UnhookWinEvent(h);
    hooks_.clear();
}

void App::onDesktopChanged() {
    auto s = desktops_.read();
    if (!s) return;
    const int before = desktopState_ ? desktopState_->current : -1;
    const DesktopId beforeId = before >= 0 ? desktopState_->desktops[before].id : DesktopId{};
    const bool switched = s->current >= 0 && !(s->desktops[s->current].id == beforeId);
    desktopState_ = s;
    if (state_ == State::Open || state_ == State::Opening) comp_.setStrip(stripContent());  // names, count, current
    if (!switched) return;  // e.g. a rename, or a desktop added elsewhere
    log::write("desktop: now %d of %zu", s->current, s->desktops.size());

    // Continue a multi-step switch one press at a time, after each confirmed change.
    if (pendingDesktop_ >= 0) {
        const int steps = switchSteps(s->current, std::min(pendingDesktop_, int(s->desktops.size()) - 1));
        if (steps == 0 || !desktops_.step(steps > 0 ? 1 : -1)) pendingDesktop_ = -1;
    }
    if (state_ == State::Open || state_ == State::Opening) {
        const int afterIdx = s->current;
        const int beforeIdx = s->indexOf(beforeId);  // -1 if the old desktop was removed
        slideDir_ = beforeIdx < 0 ? 1 : (afterIdx > beforeIdx ? 1 : -1);
        slideUntil_ = GetTickCount64() + ULONGLONG(cfg_.desktopDuration * 1000.0) + 400;  // windows uncloak during this window
        refreshWindows();
    }
}

void App::requestDesktop(int index) {
    if (!desktopState_ || desktopState_->current < 0) return;
    index = std::clamp(index, 0, int(desktopState_->desktops.size()) - 1);
    const int steps = switchSteps(desktopState_->current, index);
    if (steps == 0) return;
    pendingDesktop_ = index;
    if (!desktops_.step(steps > 0 ? 1 : -1)) pendingDesktop_ = -1;
}

void App::refreshWindows() {
    KillTimer(overlay_, kTimerRefresh);
    if (state_ != State::Open && state_ != State::Opening) return;
    auto fresh = wm_.enumerate(rules_);
    std::erase_if(fresh, [&](const WindowInfo& w) { return dismissed_.contains(w.hwnd); });
    auto sameSet = [&] {
        if (fresh.size() != windows_.size()) return false;
        for (auto& w : fresh)
            if (std::none_of(windows_.begin(), windows_.end(), [&](const WindowInfo& o) { return o.hwnd == w.hwnd; })) return false;
        return true;
    };
    if (sameSet()) return;
    windows_ = std::move(fresh);
    if (previous_ && !IsWindow(previous_)) previous_ = nullptr;
    if (focus_ && std::none_of(windows_.begin(), windows_.end(), [&](const WindowInfo& w) { return w.hwnd == focus_; })) focus_ = nullptr;
    const int dir = GetTickCount64() < slideUntil_ ? slideDir_ : 0;
    log::write("windows changed while open: %zu now, slide %d", windows_.size(), dir);
    comp_.relayout(plan(), dir);
}

EdgePoc::Rules App::overlayRules() const {
    EdgePoc::Rules r;
    r.data = cfg_.edgePocRegistry;
    // The older lists in config.json: opt in / out by exe name (an explicit "apps" entry wins).
    for (const auto& e : cfg_.edgePocExtraApps) r.data.apps.try_emplace(lowerExe(widen(e))).first->second.enabled.emplace(true);
    for (const auto& e : cfg_.edgePocExcludeApps) r.data.apps[lowerExe(widen(e))].enabled = false;
    // The user's own choices (tray menu) win over the config file.
    for (const auto& [exe, s] : userApps_.apps) r.data.apps[exe] = s;
    for (const auto& w : userApps_.windows) r.data.windows.push_back(w);
    r.data.mode = userApps_.mode;
    if (userApps_.animations) r.data.animations = userApps_.animations;
    return r;
}

void App::loadUserApps() {
    userApps_ = {};
    userApps_.mode = cfg_.edgePocRegistry.mode;  // until the user changes it, the config file decides
    const std::wstring path = log::directory() + L"\\apps.json";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return;
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    const auto parsed = json::parse(text);
    std::vector<std::string> errs;
    RegistryData d = userApps_;
    if (parsed.value) parseRegistry(*parsed.value, "apps.json", d, errs);
    else errs.push_back("apps.json is not valid JSON: " + parsed.error);
    for (const auto& e : errs) log::write("apps.json: %s", e.c_str());
    if (errs.empty()) userApps_ = std::move(d);  // a file with any error is ignored as a whole, like config.json
}

bool App::saveUserApps() const {
    const std::wstring path = log::directory() + L"\\apps.json", tmp = path + L".tmp";
    const std::string text = toJson(userApps_);
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") || !f) return false;
    const bool wrote = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!wrote || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {  // replace atomically
        DeleteFileW(tmp.c_str());
        log::write("apps.json could not be saved");
        return false;
    }
    return true;
}

void App::setOverlaysRunning(bool on) {
    userApps_.featureEnabled = on;
    saveUserApps();
    if (on) edgeOverlays_.start(GetModuleHandleW(nullptr), widen(cfg_.edgePocTitle), overlayRules());
    else edgeOverlays_.stop();
}

void App::openSettings() {
    SettingsHost host;
    host.detected = [this] { return edgeOverlays_.apps(); };
    host.user = [this]() -> RegistryData& { return userApps_; };
    host.running = [this] { return edgeOverlays_.running(); };
    host.setRunning = [this](bool on) { setOverlaysRunning(on); };
    host.changed = [this] {
        saveUserApps();
        edgeOverlays_.applyRules(overlayRules());  // takes effect at once, only the overlays that change
    };
    settings_.show(GetModuleHandleW(nullptr), std::move(host));
}

void App::loadConfig() {
    const std::wstring path = opt_.configPath.empty() ? log::directory() + L"\\config.json" : opt_.configPath;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) {
        log::write("config: %ls not found, using defaults", path.c_str());
        return;
    }
    std::string text;
    char buf[4096];
    for (std::size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
    fclose(f);
    const auto errs = loadAnimationConfig(text, cfg_);
    for (auto& e : errs) log::write("config error: %s", e.c_str());
    log::write(errs.empty() ? "config: loaded %ls" : "config: %ls rejected, using defaults", path.c_str());
}

void App::startCompositor() {
    const RECT vs = virtualScreenRect();
    origin_ = {vs.left, vs.top};
    log::write("virtual screen [%ld,%ld %ldx%ld], system dpi %u", vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top, GetDpiForSystem());
    SetWindowPos(overlay_, HWND_TOPMOST, vs.left, vs.top, vs.right - vs.left, vs.bottom - vs.top, SWP_NOACTIVATE);
    comp_.start(overlay_, vs.right - vs.left, vs.bottom - vs.top, cfg_, overlay_);
}

Rect App::toOverlay(Rect r) const {
    r.x -= origin_.x;
    r.y -= origin_.y;
    return r;
}

namespace {
float dpiScaleOf(HMONITOR mon) {
    UINT x = 96, y = 96;
    return SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y)) ? x / 96.0f : 1.0f;
}
}  // namespace

StripContent App::stripContent() {
    StripContent s;
    stripLayout_ = {};
    if (!desktopState_ || desktopState_->desktops.empty()) return s;
    HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(primary, &mi)) return s;
    const float d = dpiScaleOf(primary);
    StripParams sp;
    sp.minHeight *= d; sp.maxHeight *= d; sp.padding *= d; sp.gap *= d; sp.nameHeight *= d;
    const Rect work = toOverlay({double(mi.rcWork.left), double(mi.rcWork.top), double(mi.rcWork.right - mi.rcWork.left), double(mi.rcWork.bottom - mi.rcWork.top)});
    const double aspect = double(mi.rcMonitor.right - mi.rcMonitor.left) / std::max(1L, mi.rcMonitor.bottom - mi.rcMonitor.top);
    stripLayout_ = layoutStrip(work, int(desktopState_->desktops.size()), aspect, true, sp);
    s.band = stripLayout_.band;
    s.tiles = stripLayout_.tiles;
    s.plus = stripLayout_.plus;
    s.current = desktopState_->current;
    s.dpiScale = d;
    for (int i = 0; i < int(desktopState_->desktops.size()); ++i) s.names.push_back(desktopState_->displayName(i));
    return s;
}

std::vector<BackdropSpec> App::backdrops() const {
    std::vector<BackdropSpec> out;
    ComPtr<IDesktopWallpaper> dw;
    if (!log::check(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dw)), "CoCreateInstance(DesktopWallpaper)")) {
        // Fallback: one solid backdrop per monitor.
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR, HDC, LPRECT r, LPARAM lp) -> BOOL {
            auto& v = *reinterpret_cast<std::vector<BackdropSpec>*>(lp);
            v.push_back({{double(r->left), double(r->top), double(r->right - r->left), double(r->bottom - r->top)}, L"", GetSysColor(COLOR_DESKTOP)});
            return TRUE;
        }, reinterpret_cast<LPARAM>(&out));
        for (auto& b : out) b.monitor = toOverlay(b.monitor);
        return out;
    }
    COLORREF bg = 0;
    dw->GetBackgroundColor(&bg);
    UINT count = 0;
    dw->GetMonitorDevicePathCount(&count);
    for (UINT i = 0; i < count; ++i) {
        LPWSTR id = nullptr, path = nullptr;
        RECT r{};
        if (FAILED(dw->GetMonitorDevicePathAt(i, &id))) continue;
        if (SUCCEEDED(dw->GetMonitorRECT(id, &r))) {  // fails for monitors that are not attached
            BackdropSpec b;
            b.monitor = toOverlay({double(r.left), double(r.top), double(r.right - r.left), double(r.bottom - r.top)});
            if (SUCCEEDED(dw->GetWallpaper(id, &path)) && path) b.wallpaper = path;
            b.color = bg;
            out.push_back(std::move(b));
        }
        CoTaskMemFree(path);
        CoTaskMemFree(id);
    }
    return out;
}

std::vector<CardTarget> App::plan() {
    // Layout each monitor's windows inside its work area; the primary one also leaves room for the strip.
    std::map<HMONITOR, std::vector<std::size_t>> byMonitor;
    for (std::size_t i = 0; i < windows_.size(); ++i) byMonitor[windows_[i].monitor].push_back(i);
    slots_.assign(windows_.size(), {});
    const HMONITOR primary = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    std::map<HMONITOR, float> labelHeights;
    for (auto& [mon, idx] : byMonitor) {
        const float d = windows_[idx.front()].dpiScale;
        LayoutParams lp = layout_;
        lp.spacing *= d;
        lp.padding *= d;
        lp.labelHeight = std::round(30.f * d);
        labelHeights[mon] = float(lp.labelHeight);
        Rect area = windows_[idx.front()].workArea;
        if (mon == primary && !stripLayout_.band.empty()) {
            area.y += stripLayout_.band.h;
            area.h -= stripLayout_.band.h;
        }
        std::vector<Rect> rects;
        std::vector<Rect> prev;
        for (auto i : idx) {
            rects.push_back(windows_[i].bounds);
            const auto it = lastSlots_.find(windows_[i].hwnd);
            prev.push_back(it == lastSlots_.end() ? Rect{} : it->second);
        }
        const auto out = layoutOverview(area, rects, lp, prev);
        for (std::size_t k = 0; k < idx.size(); ++k) slots_[idx[k]] = out[k];
    }
    lastSlots_.clear();
    for (std::size_t i = 0; i < windows_.size(); ++i) lastSlots_[windows_[i].hwnd] = slots_[i];
    std::vector<CardTarget> cards;
    for (std::size_t i = 0; i < windows_.size(); ++i) {
        const WindowInfo& w = windows_[i];
        slots_[i] = toOverlay(slots_[i]);
        cards.push_back({w.hwnd, w.sourceInWindow, toOverlay(w.bounds), slots_[i], toOverlay(w.workArea), w.minimized, w.title, w.icon,
                         w.dpiScale, labelHeights[w.monitor]});
    }
    return cards;
}

void App::setFocus(HWND hwnd) {
    if (hwnd == focus_) return;
    focus_ = hwnd;
    comp_.focus(hwnd);
}

int App::focusIndex() const {
    for (std::size_t i = 0; i < windows_.size(); ++i)
        if (focus_ && windows_[i].hwnd == focus_) return int(i);
    return -1;
}

TrafficLights App::trafficFor(std::size_t i) const {
    if (!cfg_.trafficLights || i >= windows_.size() || i >= slots_.size()) return {};
    const float d = windows_[i].dpiScale, gap = std::round(6.f * d), label = std::round(30.f * d);  // as plan() / the compositor
    return layoutTrafficLights(slots_[i], gap, label - gap, d, {cfg_.trafficDiameter, cfg_.trafficSpacing});
}

void App::setTrafficHot(int button) {
    if (button == trafficHot_) return;
    trafficHot_ = button;
    comp_.trafficHot(button);
}

// Acts on the exact window of the card. Close and minimize keep the overview open, like macOS.
void App::trafficAction(std::size_t i, int button) {
    const HWND h = windows_[i].hwnd;
    if (!IsWindow(h)) return;
    log::write("traffic light %d on %p", button, static_cast<void*>(h));
    switch (static_cast<TrafficButton>(button)) {
    case TrafficButton::Close:
        PostMessageW(h, WM_CLOSE, 0, 0);  // the window may ask to save; the destroy hook removes its card
        break;
    case TrafficButton::Minimize:
        PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
        dismissed_.insert(h);
        windows_.erase(windows_.begin() + i);
        focus_ = nullptr;
        setTrafficHot(-1);
        comp_.relayout(plan(), 0);
        break;
    case TrafficButton::Zoom:
        PostMessageW(h, WM_SYSCOMMAND, IsZoomed(h) ? SC_RESTORE : SC_MAXIMIZE, 0);
        close(h, "zoom");
        break;
    }
}

int App::cardAt(double x, double y) const {
    for (std::size_t i = 0; i < slots_.size(); ++i)
        if (slots_[i].contains(x, y)) return int(i);
    return -1;
}

void App::open() {
    if (state_ == State::Open || state_ == State::Opening) return;
    if (state_ == State::Idle) previous_ = GetForegroundWindow();
    windows_ = wm_.enumerate(rules_);
    log::write("open: %zu windows", windows_.size());
    for (auto& w : windows_) {
        char title[256 * 3];
        title[WideCharToMultiByte(CP_UTF8, 0, w.title.c_str(), int(w.title.size()), title, sizeof title - 1, nullptr, nullptr)] = 0;
        log::write("  %p min=%d [%.0f,%.0f %.0fx%.0f] %s", static_cast<void*>(w.hwnd), int(w.minimized), w.bounds.x, w.bounds.y, w.bounds.w, w.bounds.h, title);
    }
    dismissed_.clear();
    trafficHot_ = -1;
    lastSlots_.clear();           // a fresh open lays out from scratch
    auto strip = stripContent();  // first: plan() leaves room for the strip
    auto cards = plan();
    focus_ = nullptr;
    scrubbing_ = false;

    SetWindowPos(overlay_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (!SetForegroundWindow(overlay_)) log::write("open: overlay could not take foreground (keyboard input unavailable)");
    comp_.enter(std::move(cards), backdrops(), std::move(strip));
    state_ = State::Opening;
    hookWindowEvents();
}

void App::close(HWND selected, const char* reason, bool fromGesture) {
    if (state_ == State::Idle || state_ == State::Closing) return;
    log::write("close: %s, selected %p", reason, static_cast<void*>(selected));
    state_ = State::Closing;
    pendingDesktop_ = -1;
    scrubbing_ = false;
    KillTimer(overlay_, kTimerScrubEnd);
    unhookWindowEvents();
    // Activate now so the real window is already on top (under the overlay) when the animation lands.
    // Minimized windows are restored at the end instead, to avoid the system restore animation mid-flight.
    if (selected && !IsIconic(selected)) WindowManager::activate(selected);
    comp_.exit(selected, fromGesture);
}

// Vertical scroll (two-finger touchpad swipe or wheel) scrubs the exit: progress follows the
// fingers directly; when the input stops, the tracker's velocity decides close vs. stay.
void App::onScroll(int delta) {
    // Opening too: while windows keep appearing the entrance can keep running, and the gesture must still work.
    if (state_ != State::Open && state_ != State::Opening) return;
    // QPC-based clock: GetTickCount64's ~16 ms steps are too coarse for a velocity estimate.
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!scrubbing_) {
        scrubbing_ = true;
        scrubAccum_ = 0;
        tracker_ = anim::GestureTracker(cfg_.gestureRubberBand);
        tracker_.begin(now, 0.0);
        setFocus(nullptr);
    }
    scrubAccum_ += delta;
    tracker_.update(now, scrubAccum_ / kScrubTravel);
    comp_.scrub(tracker_.progress(), tracker_.velocity());
    SetTimer(overlay_, kTimerScrubEnd, 140, nullptr);  // no input for 140 ms = fingers lifted
}

void App::endScroll() {
    KillTimer(overlay_, kTimerScrubEnd);
    if (!scrubbing_) return;
    scrubbing_ = false;
    const auto r = tracker_.release(cfg_.gestureProjection);
    log::write("gesture release: progress %.2f velocity %.2f -> %s", r.from, r.velocity, r.target >= 1 ? "close" : "stay");
    if (r.target >= 1) close(previous_, "gesture", true);
    else comp_.cancelScrub();
}

// Horizontal scroll switches desktops: one step per swipe, then a short lockout.
void App::onHScroll(int delta) {
    if ((state_ != State::Open && state_ != State::Opening) || !desktopState_) return;
    SetTimer(overlay_, kTimerSwipeLock, 300, nullptr);  // gesture ends after 300 ms without input
    if (swipeLocked_) return;
    hAccum_ += delta;
    if (std::abs(hAccum_) >= kSwipeThreshold) {
        requestDesktop(desktopState_->current + (hAccum_ > 0 ? 1 : -1));
        swipeLocked_ = true;
        hAccum_ = 0;
    }
}

void CALLBACK App::winEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g_app || !hwnd || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    // Top-level windows only (a destroyed window has no ancestor any more, so let it through).
    if (event != EVENT_OBJECT_DESTROY && GetAncestor(hwnd, GA_ROOT) != hwnd) return;
    // Trailing debounce: a desktop switch cloaks every window a few ms *before* the registry says which
    // desktop is current, and a new window flaps cloaked/uncloaked while the shell adopts it. Waiting
    // for 100 ms of quiet lets onDesktopChanged() set the slide direction first and hides the flapping.
    SetTimer(g_app->overlay_, kTimerRefresh, 100, nullptr);
}

LRESULT CALLBACK App::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->overlay_ == hwnd) return self->onMessage(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void App::showTrayMenu(int x, int y) {
    enum : UINT { kOpen = 1, kAutostart, kEdgePoc, kEdgeOff, kEdgeVerifiedOnly, kQuit, kSettings, kEdgeOnBase = 100 };
    const autostart::State as = autostart::state();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpen, L"Open Mission Control\tWin+Tab");
    SetMenuDefaultItem(menu, kOpen, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (as == autostart::State::DisabledByUser)
        AppendMenuW(menu, MF_STRING, kAutostart, L"Start with Windows (turned off in Settings › Startup apps)…");
    else
        AppendMenuW(menu, MF_STRING | (as == autostart::State::On ? MF_CHECKED : 0), kAutostart, L"Start with Windows");
    AppendMenuW(menu, MF_STRING | (edgeOverlays_.running() ? MF_CHECKED : 0), kEdgePoc, L"Traffic lights on app windows (experimental)");
    AppendMenuW(menu, MF_STRING, kSettings, L"    Traffic lights settings...");
    std::vector<std::wstring> offApps;  // apps the user turned off from here: a click turns them back on
    if (edgeOverlays_.running()) {
        const std::wstring last = edgeOverlays_.lastApp();
        const auto eff = overlayRules().data.apps;
        const auto it = eff.find(last);
        if (!last.empty() && !(it != eff.end() && it->second.enabled && !*it->second.enabled))
            AppendMenuW(menu, MF_STRING, kEdgeOff, (L"    Turn off for " + last).c_str());
        for (const auto& [exe, s] : userApps_.apps)
            if (s.enabled && !*s.enabled) offApps.push_back(exe);
        if (!offApps.empty()) {
            HMENU sub = CreatePopupMenu();
            for (std::size_t i = 0; i < offApps.size() && i < 50; ++i) AppendMenuW(sub, MF_STRING, kEdgeOnBase + UINT(i), (L"Turn on for " + offApps[i]).c_str());
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"    Turned-off apps");
        }
        AppendMenuW(menu, MF_STRING | (userApps_.mode == CoverMode::VerifiedOnly ? MF_CHECKED : 0), kEdgeVerifiedOnly, L"    Only apps with a measured profile");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuit, L"Quit");
    SetForegroundWindow(overlay_);  // required so the menu closes when the user clicks elsewhere
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, overlay_, nullptr);
    PostMessageW(overlay_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (cmd) {
    case kOpen:
        if (state_ == State::Idle || state_ == State::Closing) open();
        break;
    case kAutostart:
        // The user's own switch in Settings wins: we never overwrite it, we take them there instead.
        if (as == autostart::State::DisabledByUser) ShellExecuteW(nullptr, L"open", L"ms-settings:startupapps", nullptr, nullptr, SW_SHOWNORMAL);
        else if (as == autostart::State::On) autostart::disable();
        else autostart::enable();
        break;
    case kEdgePoc:
        setOverlaysRunning(!edgeOverlays_.running());
        break;
    case kSettings:
        openSettings();
        break;
    case kEdgeOff: {
        userApps_.apps[edgeOverlays_.lastApp()].enabled = false;
        saveUserApps();
        edgeOverlays_.applyRules(overlayRules());
        break;
    }
    case kEdgeVerifiedOnly:
        userApps_.mode = userApps_.mode == CoverMode::VerifiedOnly ? CoverMode::AllCompatible : CoverMode::VerifiedOnly;
        saveUserApps();
        edgeOverlays_.applyRules(overlayRules());
        break;
    case kQuit:
        DestroyWindow(overlay_);
        break;
    default:
        if (cmd >= kEdgeOnBase && cmd - kEdgeOnBase < offApps.size()) {
            const std::wstring exe = offApps[cmd - kEdgeOnBase];
            const auto& cfgApps = cfg_.edgePocRegistry.apps;
            const auto c = cfgApps.find(exe);
            if (c != cfgApps.end() && c->second.enabled && !*c->second.enabled) userApps_.apps[exe].enabled = true;  // config says off: override
            else userApps_.apps.erase(exe);
            saveUserApps();
            edgeOverlays_.applyRules(overlayRules());
        }
        break;
    }
}

LRESULT App::onMessage(UINT msg, WPARAM wp, LPARAM lp) {
    if (tray_.isTaskbarCreated(msg)) { tray_.readd(); return 0; }  // Explorer restarted
    switch (msg) {
    case Tray::kMsg:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            (state_ == State::Open || state_ == State::Opening) ? close(previous_, "tray") : open();
            break;
        case WM_CONTEXTMENU:
            showTrayMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        }
        return 0;
    case WM_HOTKEY:
        if (wp == kHotkeyId || wp == kHotkeyBridge || wp == kHotkeyBridgeWin) {
            const char* why = wp == kHotkeyId ? "hotkey" : "win+tab";
            if (state_ == State::Open || state_ == State::Opening) close(previous_, why);
            else { log::write("open: %s", why); open(); }
        }
        return 0;
    case WM_KEYDOWN: {
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wp == VK_ESCAPE) close(previous_, "esc");
        else if (wp == VK_RETURN) close(focus_ ? focus_ : previous_, "enter");
        else if (state_ != State::Open && state_ != State::Opening) {}
        else if (ctrl && (wp == VK_LEFT || wp == VK_RIGHT) && desktopState_)
            requestDesktop(desktopState_->current + (wp == VK_RIGHT ? 1 : -1));
        else if (!ctrl && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN)) {
            const NavDir dir = wp == VK_LEFT ? NavDir::Left : wp == VK_RIGHT ? NavDir::Right : wp == VK_UP ? NavDir::Up : NavDir::Down;
            int cur = -1;
            for (std::size_t i = 0; i < windows_.size(); ++i)
                if (windows_[i].hwnd == focus_) cur = int(i);
            const int next = navigate(slots_, cur, dir);
            if (next >= 0) setFocus(windows_[next].hwnd);
        }
        else if (!ctrl && wp >= '1' && wp <= '9') requestDesktop(int(wp - '1'));
        else if (ctrl && wp == 'N') desktops_.create();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (state_ != State::Open && state_ != State::Opening) return 0;
        // Only real movement changes the focus, so a keyboard selection survives a still mouse.
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (pt.x == lastMouse_.x && pt.y == lastMouse_.y) return 0;
        lastMouse_ = pt;
        if (const int fi = focusIndex(); fi >= 0) {
            const TrafficLights tl = trafficFor(fi);
            if (!tl.empty() && tl.zone.contains(pt.x, pt.y)) {  // on the buttons: keep the card hovered
                setTrafficHot(tl.hitTest(pt.x, pt.y));
                return 0;
            }
        }
        setTrafficHot(-1);
        const int i = cardAt(pt.x, pt.y);
        setFocus(i >= 0 ? windows_[i].hwnd : nullptr);
        return 0;
    }
    case WM_MOUSEWHEEL:
        onScroll(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_MOUSEHWHEEL:
        onHScroll(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER && state_ == State::Idle) comp_.prepare(backdrops());
        return 0;
    case kMsgOpenSettings:
        openSettings();
        return 0;
    case kMsgDesktopCommand:
        log::write("desktop command %llu", static_cast<unsigned long long>(wp));
        if (wp == kDesktopNew) desktops_.create();
        else if (!desktopState_) {}
        else if (wp == kDesktopNext) requestDesktop(desktopState_->current + 1);
        else if (wp == kDesktopPrev) requestDesktop(desktopState_->current - 1);
        else if (wp >= 1 && wp <= 64) requestDesktop(int(wp) - 1);
        return 0;
    case WM_LBUTTONUP: {
        if (state_ != State::Open && state_ != State::Opening) return 0;
        const double x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        for (std::size_t i = 0; i < stripLayout_.tiles.size(); ++i)
            if (stripLayout_.tiles[i].contains(x, y)) { requestDesktop(int(i)); return 0; }
        if (stripLayout_.plus.contains(x, y)) { desktops_.create(); return 0; }
        if (stripLayout_.band.contains(x, y)) return 0;  // empty strip area: stay open
        if (const int fi = focusIndex(); fi >= 0) {
            const TrafficLights tl = trafficFor(fi);
            if (!tl.empty() && tl.zone.contains(x, y)) {
                if (const int b = tl.hitTest(x, y); b >= 0) trafficAction(fi, b);
                return 0;
            }
        }
        const int i = cardAt(x, y);
        close(i >= 0 ? windows_[i].hwnd : previous_, "click");
        return 0;
    }
    case CompositorThread::kMsgReady:
        comp_.prepare(backdrops());  // decode wallpapers now, not on the first open
        if (state_ == State::Idle) trimWorkingSet();
        return 0;
    case CompositorThread::kMsgEnterDone:
        if (state_ == State::Opening) state_ = State::Open;
        if (opt_.benchCycles > 0) { collectBenchSamples(); SetTimer(overlay_, kTimerBenchClose, 400, nullptr); }
        return 0;
    case CompositorThread::kMsgExitDone: {
        if (state_ != State::Closing) return 0;  // reopened mid-exit
        HWND selected = reinterpret_cast<HWND>(wp);
        if (selected && IsIconic(selected)) WindowManager::activate(selected);
        ShowWindow(overlay_, SW_HIDE);
        state_ = State::Idle;
        windows_.clear();
        slots_.clear();
        if (rebuildPending_) {  // the display changed while the overview was open
            rebuildPending_ = false;
            startCompositor();
        }
        hook_.reinstall();  // re-arm in case Windows dropped the hook (it does so silently on a timeout)
        trimWorkingSet();
        if (opt_.benchCycles > 0) {
            collectBenchSamples();
            const bool timeUp = opt_.benchSeconds > 0 && GetTickCount64() - benchStart_ >= ULONGLONG(opt_.benchSeconds) * 1000;
            if (++benchCycle_ < opt_.benchCycles && !timeUp) SetTimer(overlay_, kTimerBenchOpen, 300, nullptr);
            else { writeBenchCsv(); DestroyWindow(overlay_); }
        }
        return 0;
    }
    case WM_TIMER:
        KillTimer(overlay_, wp);
        if (wp == kTimerBenchOpen) open();
        if (wp == kTimerBenchClose) close(previous_, "bench");
        if (wp == kTimerRefresh) refreshWindows();
        if (wp == kTimerScrubEnd) endScroll();
        if (wp == kTimerSwipeLock) { swipeLocked_ = false; hAccum_ = 0; }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        // Monitors, resolution or scaling changed: every rect we hold is stale. Close an open overview
        // (cards would fly to wrong places) and rebuild the scene for the new virtual screen once idle.
        log::write("display change (msg 0x%X)", msg);
        hook_.reinstall();
        if (state_ == State::Idle) startCompositor();
        else { rebuildPending_ = true; close(previous_, "display change"); }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) {
            // After sleep the GPU device may be gone and Windows may have dropped the hook.
            log::write("resume from sleep (0x%llX)", static_cast<unsigned long long>(wp));
            hook_.reinstall();
            if (state_ == State::Idle) startCompositor();
            else { rebuildPending_ = true; close(previous_, "resume"); }
        }
        return TRUE;
    case WM_DESTROY:
        hook_.stop();  // give Win+Tab back to Windows first
        tray_.remove();
        comp_.stop();  // thumbnails must be unregistered while their destination window still exists
        for (int id : {kHotkeyId, kHotkeyBridgeWin, kHotkeyBridge}) UnregisterHotKey(overlay_, id);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(overlay_, msg, wp, lp);
}

void App::trimWorkingSet() {
    // Idle pages go to the standby list; they fault back in cheaply on the next open (latency is
    // measured by the benchmark). Private bytes do not change.
    if (opt_.noTrim) return;
    if (!SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1))) log::lastError("SetProcessWorkingSetSize");
}

// Samples are streamed to disk each transition so long soak runs measure the app, not the sample buffer.
void App::collectBenchSamples() {
    if (!benchFile_) {
        DWM_TIMING_INFO ti{sizeof ti};
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        refreshMs_ = SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) ? 1000.0 * double(ti.qpcRefreshPeriod) / double(f.QuadPart) : 0.0;
        if (_wfopen_s(&benchFile_, opt_.benchCsv.c_str(), L"w") || !benchFile_) { log::write("bench: cannot write csv"); benchFile_ = nullptr; return; }
        fprintf(benchFile_, "cycle,kind,cards,t_ms,interval_ms,work_ms,latency_ms,refresh_ms\n");
    }
    static const char* kinds[] = {"none", "enter", "exit", "relayout"};
    for (auto& s : comp_.takeSamples()) {
        fprintf(benchFile_, "%d,%s,%d,%.3f,%.3f,%.4f,%.3f,%.3f\n", benchCycle_, kinds[s.kind], s.cards, s.tMs, s.intervalMs, s.workMs, s.latencyMs, refreshMs_);
        ++benchFrames_;
    }
}

void App::writeBenchCsv() {
    if (!benchFile_) return;
    fclose(benchFile_);
    benchFile_ = nullptr;
    log::write("bench: wrote %zu frames, refresh %.3f ms", benchFrames_, refreshMs_);
}

}  // namespace mc
