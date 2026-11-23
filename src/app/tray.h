#pragma once
// Notification-area icon: shows the app is running and gives a menu (open, start with Windows, quit).
#include <windows.h>
#include <shellapi.h>

namespace mc {

class Tray {
public:
    static constexpr UINT kMsg = WM_APP + 30;  // callback message (NOTIFYICON_VERSION_4 semantics)

    bool add(HWND owner, HINSTANCE inst);
    void remove();
    // Explorer restarted: the icon must be added again.
    bool isTaskbarCreated(UINT msg) const { return msg == taskbarCreated_ && msg != 0; }
    void readd() { if (owner_) { added_ = false; add(owner_, inst_); } }

private:
    HWND owner_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HICON icon_ = nullptr;
    bool added_ = false;
    UINT taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
};

}  // namespace mc
