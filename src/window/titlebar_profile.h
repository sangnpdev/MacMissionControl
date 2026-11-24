#pragma once
// Which windows may get the experimental traffic lights, and how tall their native button strip really is.
// Pure rules over collected facts (see docs/compatibility/*.md for the measurements behind each profile).
// Only verified apps are covered: a built-in list, plus apps the user adds in config.
#include <string>
#include <vector>

#include "window/window_classifier.h"

namespace mc {

struct StripProfile {
    bool coverable = false;
    // Height of the native button strip in logical px (96 DPI). 0 = trust DWMWA_CAPTION_BUTTON_BOUNDS,
    // which is exact for frames drawn by Windows (measured on Explorer and Notepad).
    double heightLogical = 0.0;
    // Fixed strip for apps that report nothing (no caption bounds, no hit-test): measured position of the native buttons,
    // relative to the frame's right and top edges, logical px.
    bool fixedStrip = false;
    double leftFromRight = 0.0;  // frame right edge to the left edge of the strip
    double rightGap = 0.0;       // frame right edge to the right edge of the strip
    const char* why = "";
};

// `extraExes` (lower-case file names) opts in apps that use a Windows-drawn frame. It never applies to
// Chromium-class windows: their buttons are drawn by the app and need a measured profile.
// `excludeExes` always wins. File names are compared case-insensitively.
StripProfile stripProfileFor(const TitleBarFacts& f, const std::vector<std::wstring>& extraExes,
                             const std::vector<std::wstring>& excludeExes);

}  // namespace mc
