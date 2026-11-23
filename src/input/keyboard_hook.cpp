#include "input/keyboard_hook.h"

#include <vector>

#include "diagnostics/log.h"

namespace mc {

namespace {
constexpr UINT kMsgReinstall = WM_APP + 1;  // thread message to the hook thread
// The hook procedure has no context parameter; one hook per process.
WinTabDetector* g_detector = nullptr;
HHOOK g_hook = nullptr;
}  // namespace

bool KeyboardHook::start(bool acceptInjected) {
    stop();
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_) { log::lastError("CreateEvent(hook ready)"); return false; }
    thread_ = std::thread([this, acceptInjected] { run(acceptInjected); });
    WaitForSingleObject(ready_, 2000);
    return installed_;
}

void KeyboardHook::stop() {
    if (!thread_.joinable()) return;
    PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    thread_.join();
    CloseHandle(ready_);
    ready_ = nullptr;
}

void KeyboardHook::reinstall() {
    if (threadId_) PostThreadMessageW(threadId_, kMsgReinstall, 0, 0);
}

void KeyboardHook::run(bool acceptInjected) {
    threadId_ = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // create the queue before anyone posts to it
    WinTabDetector detector(acceptInjected);
    g_detector = &detector;
    auto install = [&] {
        if (g_hook) UnhookWindowsHookEx(g_hook);
        g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, &KeyboardHook::proc, GetModuleHandleW(nullptr), 0);
        installed_ = g_hook != nullptr;
        if (!g_hook) log::lastError("SetWindowsHookEx(WH_KEYBOARD_LL)");
    };
    install();
    log::write("keyboard hook %s%s", installed_ ? "installed" : "NOT installed", acceptInjected ? " (accepting injected input: test mode)" : "");
    SetEvent(ready_);

    // The hook procedure runs on this thread, inside GetMessage: keep this loop free of other work.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (msg.message == kMsgReinstall && !msg.hwnd) install();

    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    g_detector = nullptr;
    installed_ = false;
    threadId_ = 0;
    log::write("keyboard hook removed");
}

void KeyboardHook::sendBridge() {
    // Ctrl+Alt+F24; modifiers the user already holds are left alone (same rule as desktop shortcuts).
    std::vector<INPUT> in;
    auto key = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    };
    const bool ctrlHeld = GetAsyncKeyState(VK_CONTROL) < 0, altHeld = GetAsyncKeyState(VK_MENU) < 0;
    if (!ctrlHeld) key(VK_CONTROL, false);
    if (!altHeld) key(VK_MENU, false);
    key(kBridgeKey, false);
    key(kBridgeKey, true);
    if (!altHeld) key(VK_MENU, true);
    if (!ctrlHeld) key(VK_CONTROL, true);
    if (SendInput(UINT(in.size()), in.data(), sizeof(INPUT)) != in.size()) log::lastError("SendInput(bridge chord)");
}

LRESULT CALLBACK KeyboardHook::proc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_detector) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const KeyEvent e{k->vkCode, wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN, (k->flags & LLKHF_INJECTED) != 0};
        switch (g_detector->onKey(e)) {
        case WinTabDetector::Action::Trigger:
            sendBridge();
            return 1;
        case WinTabDetector::Action::Swallow:
            return 1;
        case WinTabDetector::Action::Pass:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

}  // namespace mc
