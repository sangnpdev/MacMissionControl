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
    if (exe.empty()) return {false, 0, false, 0, 0, "unknown process"};
    if (contains(excludeExes, exe)) return {false, 0, false, 0, 0, "excluded in config"};
    if (!f.visible || f.cloaked) return {false, 0, false, 0, 0, "not visible"};
    // Apps that draw their own buttons and report neither caption bounds nor a hit-test answer: measured on Windows 11
    // 26200 at 96 DPI with the hover highlight (Zalo, Photos) or the glyph spacing (Docker Desktop, Warp).
    struct Fixed { const wchar_t *exe, *cls; double leftFromRight, rightGap, height; const char* why; };
    static const Fixed fixed[] = {
        {L"zalo.exe", L"Chrome_WidgetWin_1", 157, 0, 24, "Zalo (measured)"},                       // 3 buttons ~52x23, flush right
        {L"discord.exe", L"Chrome_WidgetWin_1", 106, 0, 33, "Discord (measured)"},                    // 32x31 buttons, 1 px from the top and right
        {L"docker desktop.exe", L"Chrome_WidgetWin_1", 148, 10, 62, "Docker Desktop (measured)"},  // 46 px buttons in a 62 px header
        {L"warp.exe", L"Window Class", 138, 0, 35, "Warp (measured)"},                            // 46x35 buttons, flush right
        {L"photos.exe", L"WinUIDesktopWin32WindowClass", 145, 1, 48, "Photos (measured)"},        // 48x47 buttons
    };
    if (f.hasCaption)
        for (const Fixed& x : fixed)
            if (exe == x.exe && f.className == x.cls) return {true, x.height, true, x.leftFromRight, x.rightGap, x.why};
    if (!f.hasCaption || !f.hasSysMenu) return {false, 0, false, 0, 0, "no standard caption (borderless, fullscreen or custom frame)"};
    if (!f.captionBoundsOk) return {false, 0, false, 0, 0, "Windows reports no caption buttons (app draws its own)"};

    if (f.className == L"Chrome_WidgetWin_1") {
        // Chromium draws the buttons itself: 40 logical px tall (measured on Edge 158 and Chrome 154), while the
        // DWM bounds say 30. Other Chromium/Electron apps (VS Code, Discord...) are not measured.
        if (exe == L"msedge.exe" || exe == L"chrome.exe") return {true, 40.0, false, 0, 0, "Chromium browser"};
        return {false, 0, false, 0, 0, "Chromium/Electron app without a measured profile"};
    }
    // Windows Terminal draws a taller title bar of its own: the DWM bounds say 30, the native Close button is 40 logical
    // px tall (measured with the hover highlight), and WM_NCHITTEST answers nothing.
    if (f.className == L"CASCADIA_HOSTING_WINDOW_CLASS" && exe == L"windowsterminal.exe") return {true, 40.0, false, 0, 0, "Windows Terminal (measured)"};
    static const std::vector<std::wstring> builtin{L"explorer.exe", L"notepad.exe"};
    if (contains(builtin, exe)) return {true, 0.0, false, 0, 0, "Windows-drawn frame (measured)"};
    if (contains(extraExes, exe)) return {true, 0.0, false, 0, 0, "Windows-drawn frame (opted in by config)"};
    return {false, 0, false, 0, 0, "no verified profile for this app"};
}

}  // namespace mc
