#include "window/window_manager.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include "diagnostics/log.h"

namespace mc {

namespace {
Rect toRect(const RECT& r) { return {double(r.left), double(r.top), double(r.right - r.left), double(r.bottom - r.top)}; }

// App icon without blocking on hung windows: the class icon first (no message), then WM_GETICON
// with a short timeout that gives up at once on a hung window.
HICON windowIcon(HWND hwnd) {
    if (auto h = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON))) return h;
    for (WPARAM kind : {WPARAM(ICON_BIG), WPARAM(ICON_SMALL2)}) {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, kind, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 20, &result) && result)
            return reinterpret_cast<HICON>(result);
    }
    return reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
}
}  // namespace

void WindowManager::init() {
    const HRESULT hr = CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm_));
    if (!log::check(hr, "CoCreateInstance(VirtualDesktopManager)")) vdm_.Reset();
}

std::vector<WindowInfo> WindowManager::enumerate(const WindowFilterRules& rules) const {
    struct Ctx { const WindowManager* self; const WindowFilterRules* rules; std::vector<WindowInfo> out; DWORD pid; };
    Ctx ctx{this, &rules, {}, GetCurrentProcessId()};

    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        auto& c = *reinterpret_cast<Ctx*>(lp);
        WindowFacts f;
        f.visible = IsWindowVisible(hwnd) != FALSE;
        if (!f.visible) return TRUE;  // cheap early exit for the bulk of hidden windows

        DWORD cloaked = 0;
        f.cloaked = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked != 0;
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        f.toolWindow = (ex & WS_EX_TOOLWINDOW) != 0;
        f.appWindow = (ex & WS_EX_APPWINDOW) != 0;
        f.noActivate = (ex & WS_EX_NOACTIVATE) != 0;
        f.hasOwner = GetWindow(hwnd, GW_OWNER) != nullptr;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        f.ownProcess = pid == c.pid;
        if (c.self->vdm_) {
            BOOL on = TRUE;
            // Failure (e.g. window closing) keeps the window: hiding a real window is worse than showing an extra one.
            if (SUCCEEDED(c.self->vdm_->IsWindowOnCurrentVirtualDesktop(hwnd, &on))) f.onCurrentDesktop = on != FALSE;
        }

        WindowInfo info;
        info.hwnd = hwnd;
        info.minimized = IsIconic(hwnd) != FALSE;
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        if (info.minimized) {
            WINDOWPLACEMENT wp{sizeof wp};
            GetWindowPlacement(hwnd, &wp);
            info.monitor = MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(info.monitor, &mi);
            // rcNormalPosition is in workspace coordinates unless the window is a tool window.
            RECT r = wp.rcNormalPosition;
            if (!f.toolWindow) OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
            info.bounds = toRect(r);
            info.sourceInWindow = {0, 0, 0, 0};  // whole source
        } else {
            RECT efb{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &efb, sizeof efb))) efb = wr;
            info.bounds = toRect(efb);
            info.sourceInWindow = {efb.left - wr.left, efb.top - wr.top, efb.right - wr.left, efb.bottom - wr.top};
            info.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        }
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(info.monitor, &mi);
        info.workArea = toRect(mi.rcWork);
        info.monitorRect = toRect(mi.rcMonitor);
        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(GetDpiForMonitor(info.monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY))) info.dpiScale = dpiX / 96.0f;

        f.width = int(info.bounds.w);
        f.height = int(info.bounds.h);
        wchar_t buf[256];
        f.className.assign(buf, GetClassNameW(hwnd, buf, 256));
        const int len = GetWindowTextW(hwnd, buf, 256);
        f.title.assign(buf, len > 0 ? len : 0);

        if (includeWindow(f, *c.rules)) {
            info.title = std::move(f.title);
            info.icon = windowIcon(hwnd);
            c.out.push_back(std::move(info));
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    return std::move(ctx.out);
}

bool WindowManager::activate(HWND hwnd) {
    if (!IsWindow(hwnd)) {
        log::write("activate: window %p no longer exists", static_cast<void*>(hwnd));
        return false;
    }
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    if (!SetForegroundWindow(hwnd)) {
        log::write("activate: SetForegroundWindow(%p) refused", static_cast<void*>(hwnd));
        return false;
    }
    return true;
}

}  // namespace mc
