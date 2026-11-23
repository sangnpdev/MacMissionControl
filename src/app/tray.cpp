#include "app/tray.h"

#include <cwchar>

#include "diagnostics/log.h"

namespace mc {

bool Tray::add(HWND owner, HINSTANCE inst) {
    if (added_) return true;
    owner_ = owner;
    inst_ = inst;
    if (!icon_) {
        const UINT dpi = GetDpiForSystem();
        icon_ = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi),
                                              GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_DEFAULTCOLOR));
    }
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = owner;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kMsg;
    nid.hIcon = icon_;
    wcscpy_s(nid.szTip, L"Mission Control (Win+Tab)");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        log::write("tray: Shell_NotifyIcon(NIM_ADD) failed");  // e.g. Explorer not running yet; retried on TaskbarCreated
        return false;
    }
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    added_ = true;
    return true;
}

void Tray::remove() {
    if (added_) {
        NOTIFYICONDATAW nid{sizeof nid};
        nid.hWnd = owner_;
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        added_ = false;
    }
    if (icon_) { DestroyIcon(icon_); icon_ = nullptr; }
}

}  // namespace mc
