#pragma once
// Compatibility engine for the experimental traffic lights: given what Windows tells us about a window (facts +
// the result of the strip probe), decide whether the native caption buttons can be covered and with which geometry.
// Pure rules, no <windows.h>. Evidence behind each rule: docs/compatibility/edge.md ("Geometry sources").
#include <string>
#include <vector>

#include "window/window_classifier.h"

namespace mc {

enum class Verdict { Supported, Partial, Unsupported, Excluded };
enum class StripSource { None, Verified, Combined, HitTest, DwmBounds };

// What the probe found, in logical px (96 DPI) so it survives DPI changes.
struct DwmStrip {  // DWMWA_CAPTION_BUTTON_BOUNDS
    bool ok = false;
    double width = 0, height = 0;
};
struct HitStrip {  // WM_NCHITTEST scan: all three of HTMINBUTTON / HTMAXBUTTON / HTCLOSE answered
    bool ok = false;
    double leftFromRight = 0;  // from the frame's right edge to the left edge of the minimize button
    double height = 0;         // from the frame's top edge to the bottom of the buttons
    double top = 0;            // from the frame's top edge to the top of the buttons
};
struct Probe {
    DwmStrip dwm;
    HitStrip hit;
};

struct Assessment {
    Verdict verdict = Verdict::Unsupported;
    StripSource source = StripSource::None;
    bool useHit = false;          // geometry from the hit-test strip (the DWM bounds are missing)
    double heightLogical = 0;     // 0 = height of the DWM bounds
    double hitLeftFromRight = 0, hitHeight = 0, hitTop = 0;
    bool useFixed = false;        // measured fixed strip (profile): fixedLeftFromRight / fixedRightGap / heightLogical
    double fixedLeftFromRight = 0, fixedRightGap = 0;
    const char* reason = "";
    bool covers() const { return verdict == Verdict::Supported || verdict == Verdict::Partial; }
};

// Verified profiles (measured per app) win; then the two geometry sources are combined:
//   both agree -> Supported (union, so neither a classic hit-test nor too-short DWM bounds leave native pixels)
//   hit-test only (VS Code, UWP) -> Supported
//   DWM bounds only (unknown app) -> Partial: DWM bounds are exact for Windows-drawn frames, but an app that draws
//   taller buttons of its own (Windows Terminal) would leave the lower part of the native buttons visible
//   neither -> Unsupported (the native buttons stay)
// System shell processes are Excluded here; elevated, owned, fullscreen and own-process windows are filtered before.
Assessment assess(const TitleBarFacts& f, const Probe& p);

// `exe` is a lower-case file name.
bool isSystemShellProcess(const std::wstring& exe);

}  // namespace mc
