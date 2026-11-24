#pragma once
// Applies a TileLayout to a window of another process: monitor, work area (taskbar excluded), DPI, invisible frame
// border, maximized state. Win32 side of window/layout_engine. UI thread only. No injection: SetWindowPos and
// WM_SYSCOMMAND to the exact window, the same calls the system makes for Snap.
#include <windows.h>

#include "window/layout_engine.h"

namespace mc {

class WindowTiler {
public:
    // Visible, not minimized, not cloaked, resizable (WS_THICKFRAME).
    static bool canTile(HWND w);
    // Starts the move. The window moves asynchronously (SWP_ASYNCWINDOWPOS: a hung app never blocks us), so the result
    // is checked by verify() about 150 ms later. False when the window cannot be tiled.
    bool apply(HWND w, TileLayout layout);
    // Checks the pending tile once: re-applies it if a DPI change on a mixed-DPI move altered the size, then lets a
    // window that keeps its own min / max size stay inside the work area. True while another call is needed.
    bool verify();
    bool pending() const { return w_ != nullptr; }

private:
    HWND w_ = nullptr;
    TileLayout layout_ = TileLayout::Full;
    Rect visible_, work_;
    int tries_ = 0;
    bool place(HWND w, const Rect& visible, bool sizeToo);
};

}  // namespace mc
