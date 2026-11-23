#pragma once
// Win+Tab takeover (Phase 5): a WH_KEYBOARD_LL hook on its own thread. On Win+Tab it swallows the
// Tab and injects the "bridge" chord Ctrl+Alt+F24, which the app has registered with RegisterHotKey
// (with and without MOD_WIN). The resulting WM_HOTKEY gives the overlay foreground rights, and the
// injected key also stops the shell from opening Start when Win is released.
// Safety: no system setting changes. If this process crashes or exits, Windows removes the hook; if
// the hook thread ever stalls past LowLevelHooksTimeout, Windows removes it silently. Either way
// Win+Tab opens Windows Task View again.
#include <windows.h>

#include <atomic>
#include <thread>

#include "input/wintab_detector.h"

namespace mc {

class KeyboardHook {
public:
    static constexpr WORD kBridgeKey = VK_F24;  // modifiers: Ctrl+Alt (Win too when the user holds it)

    ~KeyboardHook() { stop(); }
    // acceptInjected: tests only, see WinTabDetector.
    bool start(bool acceptInjected);
    void stop();
    void reinstall();  // after resume / display change / overview close: re-arm in case Windows dropped it
    bool active() const { return installed_; }

private:
    static LRESULT CALLBACK proc(int code, WPARAM wp, LPARAM lp);
    void run(bool acceptInjected);
    static void sendBridge();

    std::thread thread_;
    std::atomic<DWORD> threadId_{0};
    std::atomic<bool> installed_{false};
    HANDLE ready_ = nullptr;
};

}  // namespace mc
