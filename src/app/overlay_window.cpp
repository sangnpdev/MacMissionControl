#include "app/overlay_window.h"

#include "diagnostics/log.h"

namespace mc {

RECT virtualScreenRect() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

HWND createOverlayWindow(HINSTANCE inst, WNDPROC proc, void* userData) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    if (!RegisterClassExW(&wc)) { log::lastError("RegisterClassEx(overlay)"); return nullptr; }

    const RECT r = virtualScreenRect();
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, kOverlayClass,
                                L"Mission Control", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, inst, userData);
    if (!hwnd) log::lastError("CreateWindowEx(overlay)");
    return hwnd;
}

}  // namespace mc
