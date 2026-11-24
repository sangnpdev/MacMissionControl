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

namespace mc {

// Overlay over ONE window. Created and owned by EdgeOverlays, which feeds it the window events.
class EdgePoc {
public:
    ~EdgePoc() { stop(); }
    // Which apps may be covered and which never (exe names, lower-case); see titlebar_profile.h.
    struct Rules {
        std::vector<std::wstring> extraExes, excludeExes;
    };
    // True for a visible, unowned top-level window of a verified app with caption buttons. `heightLogical` gets the
    // strip height of its profile (0 = DWM bounds). `maybeLater` is set when the window would qualify but its
    // caption-button bounds are not available yet (a fresh window): retry shortly.
    static bool coverable(HWND w, const Rules& rules, double* heightLogical, bool* maybeLater = nullptr);
    // `owner` receives kMsgReap with the target in WPARAM once the target is gone (posted, never sent).
    bool attach(HINSTANCE inst, HWND target, HWND owner, double heightLogical);
    void stop();  // immediate: timers, overlay window and GDI/D2D objects are released
    bool running() const { return overlay_ != nullptr; }
    DWORD pid() const { return pid_; }
    void requestPlace();            // coalesced; the geometry is read when the message is handled
    void onForeground(HWND fg);     // active / inactive look

    static constexpr UINT kMsgReap = WM_APP + 42;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(HWND self, UINT msg, WPARAM wp, LPARAM lp);

    void place();        // geometry + z-order; hides while the target is minimized or cloaked
    void sampleColor();  // title bar color next to the buttons
    void scheduleColorSamples();  // several samples over the next ~1.4 s
    void render();
    void onHover(int index);
    void onPress(int index, bool down);
    void act(int button);
    void animate();
    void scheduleAnimation();
    double now() const;

    HWND target_ = nullptr, overlay_ = nullptr, owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    DWORD pid_ = 0;
    HANDLE process_ = nullptr, wait_ = nullptr;  // wakes us when Edge exits (no DESTROY event is delivered then)
    bool placePending_ = false, active_ = false, animating_ = false;
    double heightLogical_ = 0.0;  // native strip height of the app's profile; 0 = DWM bounds
    bool enabled_[3] = {true, true, true};  // close / minimize / zoom as the window's style allows
    bool owned_ = false;  // overlay is an owned window of the target (system keeps it above Edge)
    int goneChecks_ = 0;  // re-checks of a hidden target (see place())
    int colorChecks_ = 0; // color samples left (see scheduleColorSamples())
    Rect rect_;          // overlay rect, screen px
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
    };
    HINSTANCE inst_ = nullptr;
    HWND msg_ = nullptr;  // message-only window: new-window candidates, reaping, retry timer
    std::wstring title_;
    EdgePoc::Rules rules_;
    std::unordered_map<HWND, std::unique_ptr<EdgePoc>> overlays_;
    std::unordered_map<DWORD, PidHooks> pidHooks_;
    std::unordered_map<HWND, int> retries_;  // fresh windows without caption-button bounds yet
    HWINEVENTHOOK showHook_ = nullptr, foregroundHook_ = nullptr, reorderHook_ = nullptr;
};

}  // namespace mc
