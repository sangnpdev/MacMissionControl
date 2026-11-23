#pragma once
#include <string>
#include <vector>

namespace mc {

// Everything the filter needs, collected by WindowManager. Pure data so the rules are testable.
struct WindowFacts {
    bool visible = false;
    bool cloaked = false;
    bool toolWindow = false;  // WS_EX_TOOLWINDOW
    bool appWindow = false;   // WS_EX_APPWINDOW forces inclusion of owned/tool windows
    bool noActivate = false;  // WS_EX_NOACTIVATE
    bool hasOwner = false;
    bool ownProcess = false;
    bool onCurrentDesktop = true;
    int width = 0, height = 0;  // visible frame bounds
    std::wstring className;
    std::wstring title;
};

struct WindowFilterRules {
    int minWidth = 32, minHeight = 32;
    bool requireTitle = true;
    std::vector<std::wstring> denyClasses{L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"};
    std::vector<std::wstring> allowClasses;  // empty = all; benchmarks use this to isolate test windows
};

bool includeWindow(const WindowFacts& f, const WindowFilterRules& rules);

}  // namespace mc
