#include "window/window_classifier.h"

namespace mc {

Classification classifyTitleBar(const TitleBarFacts& f) {
    if (!f.visible || f.cloaked) return {TitleBarKind::Unsupported, IntegrationMode::NativeFallback, "not visible"};
    if (f.coversMonitor && !f.hasCaption) return {TitleBarKind::Fullscreen, IntegrationMode::NativeFallback, "fullscreen: never touched"};
    if (!f.hasCaption) return {TitleBarKind::Borderless, IntegrationMode::NativeFallback, "no caption: nothing to replace"};
    if (f.className == L"Chrome_WidgetWin_1") {
        // Chromium draws its own caption buttons on the right and answers WM_NCHITTEST (Snap Layout).
        // They cannot be hidden or moved from outside, and the top-left holds browser UI.
        if (f.exeName == L"msedge.exe")
            return {TitleBarKind::Edge, IntegrationMode::NativeFallback, "Edge draws its own buttons; a second set would duplicate them"};
        return {TitleBarKind::Chromium, IntegrationMode::NativeFallback, "Chromium app: same limits as Edge, not evaluated"};
    }
    if (f.hasSysMenu && f.captionBoundsOk) return {TitleBarKind::Win32Standard, IntegrationMode::NativeFallback, "native buttons cannot be replaced from outside"};
    return {TitleBarKind::Unsupported, IntegrationMode::NativeFallback, "unknown title bar"};
}

}  // namespace mc
