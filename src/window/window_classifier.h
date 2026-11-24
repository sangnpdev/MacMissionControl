#pragma once
// Decides how (and whether) another application's title bar could get macOS traffic lights.
// Pure rules over collected facts, so they are testable. Nothing here draws or touches other
// processes. Scope for now: Microsoft Edge. See docs/compatibility/edge.md for why every result
// is Fallback.
#include <string>

namespace mc {

struct TitleBarFacts {
    std::wstring className;
    std::wstring exeName;       // lower-case file name, e.g. L"msedge.exe"
    bool hasCaption = false;    // WS_CAPTION
    bool hasSysMenu = false;    // WS_SYSMENU
    bool visible = true;
    bool cloaked = false;
    bool coversMonitor = false; // window rect covers its whole monitor
    bool captionBoundsOk = false;  // DWMWA_CAPTION_BUTTON_BOUNDS returned a non-empty rect
};

enum class TitleBarKind { Win32Standard, Chromium, Edge, Borderless, Fullscreen, Unsupported };

// Replacement / Overlay need a way to hide the native buttons or a validated free area, and no such
// way exists without code injection (docs/compatibility/edge.md). Native fallback keeps Windows' buttons.
enum class IntegrationMode { Replacement, ValidatedOverlay, NativeFallback };

struct Classification {
    TitleBarKind kind = TitleBarKind::Unsupported;
    IntegrationMode mode = IntegrationMode::NativeFallback;
    const char* reason = "";
};

Classification classifyTitleBar(const TitleBarFacts& f);

}  // namespace mc
