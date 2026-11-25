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
#include <set>

#include "app/edge_poc.h"
#include "app/settings_window.h"
#include "ui/layout.h"
#include "ui/traffic_lights.h"
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
    bool settings = false;      // open the settings window at startup
    bool hookAcceptInjected = false;  // tests only: let synthetic Win+Tab trigger the hook
};

class App {
public:
    // Sent by a second instance (`--desktop ...`) to the running one. wParam: kDesktopNext,
    // kDesktopPrev, kDesktopNew, or a 1-based desktop number.
    static constexpr UINT kMsgDesktopCommand = WM_APP + 11;
    static constexpr UINT kMsgOpenSettings = WM_APP + 12;  // `MissionControl.exe --settings` on a running instance
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
    int focusIndex() const;                         // index in windows_ of focus_, or -1
    TrafficLights trafficFor(std::size_t i) const;  // buttons under card i (empty if disabled or too narrow)
    void setTrafficHot(int button);
    void trafficAction(std::size_t i, int button);
    void onScroll(int delta);
    void endScroll();
    void onHScroll(int delta);
    void showTrayMenu(int x, int y);
    void hookWindowEvents();    // only while the overview is open
    void unhookWindowEvents();
    void refreshWindows();      // re-enumerate; relayout if the set of windows changed
    void onDesktopChanged();
    void requestDesktop(int index);
    void removeDesktop(int index);  // switches to it first when it is not current (the shell removes only the current one)
    int stripTileAt(double x, double y) const;  // tile (or its close button) under the point, or -1
    std::vector<CardTarget> plan();  // layout of windows_ into slots_, in overlay coordinates
    void startCompositor();
    void trimWorkingSet();
    void loadConfig();
    EdgePoc::Rules overlayRules() const;  // config + the user's own settings (apps.json)
    void loadUserApps();
    bool saveUserApps() const;
    void openSettings();
    void setOverlaysRunning(bool on);
    void collectBenchSamples();
    void writeBenchCsv();

    HWND overlay_ = nullptr;
    POINT origin_{};  // virtual-screen origin = overlay client (0,0)
    State state_ = State::Idle;
    HWND previous_ = nullptr;
    HWND focus_ = nullptr;          // hovered or keyboard-selected card
    POINT lastMouse_{-1, -1};
    int trafficHot_ = -1;           // hovered traffic-light button of the focused card
    std::set<HWND> dismissed_;      // minimized from the overview: hidden until the next open
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
    DesktopId removeId_;           // desktop to remove once it is current; null = none
    DesktopId returnId_;           // desktop to go back to after that removal
    int stripHot_ = -1;            // strip tile under the mouse (shows its close button)
    int slideDir_ = 0;             // direction of the last desktop switch
    ULONGLONG slideUntil_ = 0;     // window changes before this tick slide with the switch

    AnimationConfig cfg_;
    WindowFilterRules rules_;
    LayoutParams layout_;
    RegistryData userApps_;           // changed from the tray menu, saved to %LOCALAPPDATA%\MissionControl\apps.json
    SettingsWindow settings_;
    EdgeOverlays edgeOverlays_;       // experimental, off unless enabled in config or the tray menu
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
