#pragma once
#include <windows.h>
#include <shobjidl.h>  // CLSID_VirtualDesktopManager lives here, not in shobjidl_core.h
#include <wrl/client.h>

#include <string>
#include <vector>

#include "core/geometry.h"
#include "window/window_filter.h"

namespace mc {

struct WindowInfo {
    HWND hwnd = nullptr;
    Rect bounds;          // visible frame on screen (restored rect if minimized), physical px
    RECT sourceInWindow{};// visible frame relative to the full window rect (thumbnail rcSource)
    bool minimized = false;
    HMONITOR monitor = nullptr;
    Rect workArea;        // work area of `monitor`
    Rect monitorRect;     // full rect of `monitor`
    float dpiScale = 1.0f;// effective DPI of `monitor` / 96
    std::wstring title;
    HICON icon = nullptr; // not owned
};

class WindowManager {
public:
    // Requires COM initialized on the calling thread. Virtual-desktop filtering is skipped if unavailable.
    void init();

    // Top-to-bottom Z-order.
    std::vector<WindowInfo> enumerate(const WindowFilterRules& rules) const;

    // Restores a minimized window and brings it to the foreground. Logs on failure.
    static bool activate(HWND hwnd);

private:
    Microsoft::WRL::ComPtr<IVirtualDesktopManager> vdm_;
};

}  // namespace mc
