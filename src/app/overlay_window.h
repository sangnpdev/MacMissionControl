#pragma once
#include <windows.h>

namespace mc {

inline constexpr wchar_t kOverlayClass[] = L"MissionControlOverlay";

// Borderless, topmost, no redirection bitmap (content comes from DirectComposition + DWM
// thumbnails). WS_EX_TOOLWINDOW keeps it out of Alt+Tab and our own enumeration.
HWND createOverlayWindow(HINSTANCE inst, WNDPROC proc, void* userData);

// Virtual-screen rect in physical pixels (process is Per-Monitor-V2 aware).
RECT virtualScreenRect();

}  // namespace mc
