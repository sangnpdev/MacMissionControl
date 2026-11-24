#pragma once
#include <windows.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "animation/gesture.h"
#include "config/animation_config.h"
#include "desktop/virtual_desktop_provider.h"
#include "app/tray.h"
#include "input/keyboard_hook.h"
#include "rendering/compositor_thread.h"
#include <map>

#include "ui/layout.h"
#include "window/window_filter.h"
#include "window/window_manager.h"

namespace mc {

struct Options {
    bool show = false;          // open once at startup
    int benchCycles = 0;        // >0: benchmark mode
    std::wstring benchCsv;
    std::wstring benchClass;    // restrict overview to this window class
    bool noTrim = false;        // diagnostics: keep the working set when idle
    std::wstring configPath;    // empty = %LOCALAPPDATA%\MissionControl\config.json
    int benchSeconds = 0;       // >0: stop the benchmark after this many seconds (soak)
    bool noTakeover = false;    // leave Win+Tab to Windows regardless of config
    bool hookAcceptInjected = false;  // tests only: let synthetic Win+Tab trigger the hook
};

class App {
public:
    // Sent by a second instance (`--desktop ...`) to the running one. wParam: kDesktopNext,
    // kDesktopPrev, kDesktopNew, or a 1-based desktop number.
    static constexpr UINT kMsgDesktopCommand = WM_APP + 11;
    static constexpr WPARAM kDesktopNext = 1000, kDesktopPrev = 1001, kDesktopNew = 1002;

    int run(HINSTANCE inst, const Options& opt);

private:
    enum class State { Idle, Opening, Open, Closing };

    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static void CALLBACK winEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp);

    void open();
    void close(HWND selected, const char* reason, bool fromGesture = false);
    Rect toOverlay(Rect screenRect) const;
    StripContent stripContent();             // also updates stripLayout_ (hit-testing, layout area)
    std::vector<BackdropSpec> backdrops() const;
    void setFocus(HWND hwnd);
    int cardAt(double x, double y) const;
    void onScroll(int delta);
    void endScroll();
    void onHScroll(int delta);
    void showTrayMenu(int x, int y);
    void hookWindowEvents();    // only while the overview is open
    void unhookWindowEvents();
    void refreshWindows();      // re-enumerate; relayout if the set of windows changed
    void onDesktopChanged();
    void requestDesktop(int index);
    std::vector<CardTarget> plan();  // layout of windows_ into slots_, in overlay coordinates
    void startCompositor();
    void trimWorkingSet();
    void loadConfig();
    void collectBenchSamples();
    void writeBenchCsv();

    HWND overlay_ = nullptr;
    POINT origin_{};  // virtual-screen origin = overlay client (0,0)
    State state_ = State::Idle;
    HWND previous_ = nullptr;
    HWND focus_ = nullptr;          // hovered or keyboard-selected card
    POINT lastMouse_{-1, -1};
    StripLayout stripLayout_;       // overlay coordinates
    bool scrubbing_ = false;
    double scrubAccum_ = 0;
    anim::GestureTracker tracker_;
    int hAccum_ = 0;
    bool swipeLocked_ = false;
    std::vector<HWINEVENTHOOK> hooks_;

    KeyboardHook hook_;
    Tray tray_;
    bool rebuildPending_ = false;   // display changed / resumed while open: rebuild the scene after the exit
    ULONGLONG benchStart_ = 0;
    VirtualDesktopProvider desktops_;
    std::optional<DesktopState> desktopState_;
    int pendingDesktop_ = -1;      // target of a multi-step switch, -1 = none
    int slideDir_ = 0;             // direction of the last desktop switch
    ULONGLONG slideUntil_ = 0;     // window changes before this tick slide with the switch

    AnimationConfig cfg_;
    WindowFilterRules rules_;
    LayoutParams layout_;
    std::map<HWND, Rect> lastSlots_;  // screen coordinates; keeps cards in place when windows come and go
    WindowManager wm_;
    CompositorThread comp_;
    std::vector<WindowInfo> windows_;
    std::vector<Rect> slots_;  // overlay coordinates, parallel to windows_

    Options opt_;
    int benchCycle_ = 0;
    FILE* benchFile_ = nullptr;
    double refreshMs_ = 0;
    std::size_t benchFrames_ = 0;
};

}  // namespace mc
