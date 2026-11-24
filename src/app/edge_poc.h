#pragma once
// EXPERIMENTAL (docs/compatibility/edge.md). Covers the native caption buttons of windows of verified apps
// (Edge, Chrome, Explorer, Notepad, plus apps opted in by config) with macOS traffic lights. Off by default. No injection, no subclassing, no style changes: per
// Edge window a layered window placed directly above it in Z-order that follows it through WinEvent
// hooks. Everything runs on the UI thread of the app.
#include <windows.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "animation/animation.h"
#include "ui/traffic_lights.h"
#include "window/app_registry.h"
#include "window/compat_engine.h"

namespace mc {

// Overlay over ONE window. Created and owned by EdgeOverlays, which feeds it the window events.
// What the app knows about one application it has seen (for the settings window). One entry per exe, bounded.
struct AppInfo {
    std::wstring exe, path, className, title;  // path / class / title of the latest window
    DWORD pid = 0;
    Verdict verdict = Verdict::Unsupported;
    StripSource source = StripSource::None;
    std::string reason;
    double heightLogical = 0;
    bool known = false;  // assessed at least once (an app turned off before it was ever seen has no verdict)
    int windows = 0;     // windows covered right now
    ULONGLONG lastSeen = 0;
};

class EdgePoc {
public:
    ~EdgePoc() { stop(); }
    // Everything that decides which windows are covered: per-app / per-window settings and the mode.
    struct Rules {
        RegistryData data;
    };
    // The outcome of evaluating one window.
    struct Cover {
        Assessment a;
        double heightLogical = 0;  // native strip height to cover (0 = DWM bounds), after the user's override
        std::wstring exe, path, className, title;  // filled whenever the process is known, covered or not
        DWORD pid = 0;
        bool policyOff = false;  // the user turned this window off: it was not assessed
        std::string why;         // the decision's reason
    };
    // True when `w` is a visible, unowned top-level window that the compatibility engine and the user's settings
    // allow to cover. `maybeLater` is set when the caption buttons could not be located yet (a fresh window):
    // retry shortly. Elevated windows, our own windows and system shell processes never qualify.
    static bool evaluate(HWND w, const Rules& rules, Cover* out, bool* maybeLater = nullptr);
    // `owner` receives kMsgReap with the target in WPARAM once the target is gone (posted, never sent).
    bool attach(HINSTANCE inst, HWND target, HWND owner, const Cover& cover);
    void stop();  // immediate: timers, overlay window and GDI/D2D objects are released
    bool running() const { return overlay_ != nullptr; }
    DWORD pid() const { return pid_; }
    bool owned() const { return owned_; }  // false: Z-order must be tracked by events
    const std::wstring& exe() const { return exe_; }
    void requestPlace();            // coalesced; the geometry is read when the message is handled
    void onForeground(HWND fg);     // active / inactive look

    static constexpr UINT kMsgReap = WM_APP + 42;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(HWND self, UINT msg, WPARAM wp, LPARAM lp);

    bool isActive(HWND fg) const;  // fg is the target or owned by it
    void setActive(bool active);
    void place();        // geometry + z-order; hides while the target is minimized or cloaked
    void sampleColor();  // title bar color next to the buttons
    void scheduleColorSamples(int firstDelayMs);  // several samples over the next ~1.4 s after the first
    void render();
    void onHover(int index);
    void onPress(int index, bool down);
    void act(int button);
    void animate();
    void scheduleAnimation();
    void startMove(const Rect& from, const Rect& to);  // follow Windows' maximize / restore animation
    void moveTick();
    double now() const;

    HWND target_ = nullptr, overlay_ = nullptr, owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    DWORD pid_ = 0;
    bool placePending_ = false, active_ = false, animating_ = false;
    std::wstring exe_;
    Assessment assess_;           // how this window's buttons are located (hit-test geometry is refreshed on zoom / DPI change)
    bool hitZoomed_ = false;
    UINT hitDpi_ = 0;
    double heightLogical_ = 0.0;  // native strip height of the app's profile; 0 = DWM bounds
    bool enabled_[3] = {true, true, true};  // close / minimize / zoom as the window's style allows
    bool owned_ = false;  // overlay is an owned window of the target (system keeps it above Edge)
    int goneChecks_ = 0;  // re-checks of a hidden target (see place())
    int colorChecks_ = 0; // color samples left (see scheduleColorSamples())
    Rect rect_;          // overlay rect, screen px (final position; during a move the window is between)
    bool moving_ = false;  // following the maximize / restore animation
    Rect moveFrom_, moveTo_;
    anim::AnimatedValue moveP_;  // 0..1 progress of that animation
    float dpi_ = 1.f;
    bool zoomed_ = false;
    COLORREF bg_ = RGB(32, 32, 32);
    TrafficLights tl_;   // overlay-local coordinates
    int hot_ = -1, pressed_ = -1;
    anim::AnimatedValue hover_[3], press_[3], inactive_;
    LARGE_INTEGER qpcFreq_{};

    struct Gdi;          // DIB section + D2D DC render target (pimpl keeps d2d out of the header)
    Gdi* gdi_ = nullptr;
};

// Keeps one EdgePoc per covered window: the windows that exist at start, and every such window that is
// shown later (one global EVENT_OBJECT_SHOW hook, filtered by the WS_CAPTION style; no polling). WinEvent hooks
// for location / move / minimize / destroy are installed per target process, shared by its windows.
class EdgeOverlays {
public:
    ~EdgeOverlays() { stop(); }
    // `titleContains` empty: every covered window. Otherwise only windows whose title contains it when
    // they are first seen (test aid; the title is the active tab, so it is not tracked afterwards).
    bool start(HINSTANCE inst, const std::wstring& titleContains, EdgePoc::Rules rules = {});
    void stop();  // immediate
    bool running() const { return msg_ != nullptr; }
    std::wstring lastApp() const { return lastApp_; }  // exe of the app the user last worked in (not ours)
    std::vector<AppInfo> apps() const;                  // every app seen since start, newest verdict
    // New rules (settings changed): drop overlays that no longer qualify, then look at all windows again.
    void applyRules(EdgePoc::Rules rules);

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK eventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    LRESULT onMessage(HWND self, UINT msg, WPARAM wp, LPARAM lp);

    void consider(HWND w);  // attach when `w` is coverable and passes the title filter
    void onForeground(HWND fg);
    void watchReorder();  // listen for Z-order changes for a short while
    void addPidHooks(DWORD pid);
    void releasePidHooks(DWORD pid);

    struct PidHooks {
        HWINEVENTHOOK hook[4]{};
        int refs = 0;
        HANDLE process = nullptr, wait = nullptr;  // one wait per process (Edge sends no DESTROY when it exits)
        void* waitCtx = nullptr;
    };
    static void freePidWait(PidHooks& p);
    HINSTANCE inst_ = nullptr;
    HWND msg_ = nullptr;  // message-only window: new-window candidates, reaping, retry timer
    std::wstring title_;
    EdgePoc::Rules rules_;
    std::unordered_map<HWND, std::unique_ptr<EdgePoc>> overlays_;
    std::unordered_map<DWORD, PidHooks> pidHooks_;
    std::unordered_map<HWND, int> retries_;  // fresh windows without caption-button bounds yet
    std::unordered_map<HWND, ULONGLONG> rejected_;  // windows judged not coverable (tick): not re-probed for 60 s; bounded
    std::wstring lastApp_;
    std::unordered_map<std::wstring, AppInfo> seen_;  // by exe; at most 512
    void noteApp(const EdgePoc::Cover& c);
    HWINEVENTHOOK showHook_ = nullptr, uncloakHook_ = nullptr, foregroundHook_ = nullptr, reorderHook_ = nullptr;
};

}  // namespace mc
