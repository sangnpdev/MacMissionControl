#include "app/window_tiler.h"

#include <dwmapi.h>
#include <shellscalingapi.h>

#include "diagnostics/log.h"

namespace mc {
namespace {
bool frameBounds(HWND w, RECT& ef) {
    return SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &ef, sizeof ef));
}
Rect toRect(const RECT& r) { return {double(r.left), double(r.top), double(r.right - r.left), double(r.bottom - r.top)}; }
Rect workAreaOf(HWND w) {
    MONITORINFO mi{sizeof mi};
    const HMONITOR m = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);  // the monitor holding most of the window
    GetMonitorInfoW(m, &mi);
    return toRect(mi.rcWork);
}
bool dpiUnaware(HWND w) {
    const DPI_AWARENESS_CONTEXT ctx = GetWindowDpiAwarenessContext(w);
    return GetAwarenessFromDpiAwarenessContext(ctx) == DPI_AWARENESS_UNAWARE;
}
}  // namespace

bool WindowTiler::canTile(HWND w) {
    if (!w || !IsWindow(w) || !IsWindowVisible(w) || IsIconic(w)) return false;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    return !cloaked && (GetWindowLongPtrW(w, GWL_STYLE) & WS_THICKFRAME) != 0;
}

// Moves the window so that its visible frame is `visible`. The invisible border is measured on the window itself, at
// its current DPI, and scaled to the DPI of the monitor it is going to (a window moved to another monitor is re-scaled
// by Windows after the move, which verify() corrects).
bool WindowTiler::place(HWND w, const Rect& visible, bool sizeToo) {
    RECT wr{}, ef{};
    if (!GetWindowRect(w, &wr) || !frameBounds(w, ef)) return false;
    FrameInsets in{double(ef.left - wr.left), double(ef.top - wr.top), double(wr.right - ef.right), double(wr.bottom - ef.bottom)};
    UINT dpiWin = GetDpiForWindow(w), dpiMon = dpiWin, dummy = 0;
    const POINT c{LONG(visible.cx()), LONG(visible.cy())};
    if (HMONITOR m = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST); m) GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dpiMon, &dummy);
    if (dpiWin && dpiMon) in = scaleInsets(in, double(dpiMon) / double(dpiWin));
    Rect o = outerRect(visible, in);
    // A DPI-unaware window whose frame reaches onto a monitor with another DPI is rescaled by the system (measured on a
    // 96 + 192 DPI pair: 974 px asked, 4792 px got). Its invisible border stays on its own monitor; the visible frame then
    // starts a few pixels inside the screen edge, only on the side that faces the other monitor.
    if (dpiUnaware(w)) {
        MONITORINFO mi{sizeof mi};
        if (GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) o = clampInside(o, toRect(mi.rcMonitor));
    }
    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (sizeToo ? 0 : SWP_NOSIZE);
    return SetWindowPos(w, nullptr, int(o.x), int(o.y), int(o.w), int(o.h), flags) != 0;
}

bool WindowTiler::apply(HWND w, TileLayout layout) {
    if (!canTile(w)) return false;
    w_ = nullptr;
    if (layout == TileLayout::Full) {  // the window's own maximize: same result as the green button
        if (!IsZoomed(w)) PostMessageW(w, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
        log::write("tile: %p %s", static_cast<void*>(w), tileLayoutName(layout));
        return true;
    }
    if (IsZoomed(w)) {  // a maximized window has no free rect: restore first, with a bounded wait
        DWORD_PTR res = 0;
        if (!SendMessageTimeoutW(w, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 250, &res)) return false;
    }
    work_ = workAreaOf(w);
    visible_ = layoutRect(layout, work_);
    if (!place(w, visible_, true)) return false;
    w_ = w;
    layout_ = layout;
    tries_ = 0;
    log::write("tile: %p %s -> %d,%d %dx%d", static_cast<void*>(w), tileLayoutName(layout), int(visible_.x), int(visible_.y), int(visible_.w),
               int(visible_.h));
    return true;
}

bool WindowTiler::verify() {
    if (!w_) return false;
    RECT ef{};
    if (!IsWindow(w_) || IsIconic(w_) || IsZoomed(w_) || !frameBounds(w_, ef)) { w_ = nullptr; return false; }
    const Rect actual = toRect(ef);
    if (sameRect(actual, visible_, 2)) { w_ = nullptr; return false; }
    if (tries_++ == 0) {  // first mismatch: a DPI change after the move resized the window; apply once more at the new DPI
        place(w_, visible_, true);
        return true;
    }
    // The window keeps its own size (min / max track size, fixed aspect): put it inside the work area, centred on its tile.
    const Rect fit = fitWithin(visible_, actual.w, actual.h, work_);
    if (!sameRect(fit, actual, 1)) place(w_, fit, false);
    log::write("tile: %p kept its own size %dx%d (wanted %dx%d)", static_cast<void*>(w_), int(actual.w), int(actual.h), int(visible_.w), int(visible_.h));
    w_ = nullptr;
    return false;
}

}  // namespace mc
