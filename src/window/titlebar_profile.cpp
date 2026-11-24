#include "window/titlebar_profile.h"

#include <algorithm>
#include <cwctype>

namespace mc {

namespace {
std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}
bool contains(const std::vector<std::wstring>& v, const std::wstring& exe) {
    return std::any_of(v.begin(), v.end(), [&](const std::wstring& e) { return lower(e) == exe; });
}
}  // namespace

StripProfile stripProfileFor(const TitleBarFacts& f, const std::vector<std::wstring>& extraExes,
                             const std::vector<std::wstring>& excludeExes) {
    const std::wstring exe = lower(f.exeName);
    if (exe.empty()) return {false, 0, "unknown process"};
    if (contains(excludeExes, exe)) return {false, 0, "excluded in config"};
    if (!f.visible || f.cloaked) return {false, 0, "not visible"};
    if (!f.hasCaption || !f.hasSysMenu) return {false, 0, "no standard caption (borderless, fullscreen or custom frame)"};
    if (!f.captionBoundsOk) return {false, 0, "Windows reports no caption buttons (app draws its own)"};

    if (f.className == L"Chrome_WidgetWin_1") {
        // Chromium draws the buttons itself: 40 logical px tall (measured on Edge 158 and Chrome 154), while the
        // DWM bounds say 30. Other Chromium/Electron apps (VS Code, Discord...) are not measured.
        if (exe == L"msedge.exe" || exe == L"chrome.exe") return {true, 40.0, "Chromium browser"};
        return {false, 0, "Chromium/Electron app without a measured profile"};
    }
    static const std::vector<std::wstring> builtin{L"explorer.exe", L"notepad.exe"};
    if (contains(builtin, exe)) return {true, 0.0, "Windows-drawn frame (measured)"};
    if (contains(extraExes, exe)) return {true, 0.0, "Windows-drawn frame (opted in by config)"};
    return {false, 0, "no verified profile for this app"};
}

}  // namespace mc
