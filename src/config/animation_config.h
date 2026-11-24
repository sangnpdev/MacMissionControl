#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "animation/animation.h"
#include "window/app_registry.h"

namespace mc {

// Tunable animation parameters. Loaded from JSON (see resources/config.default.json).
struct AnimationConfig {
    // Entrance: real position -> overview slot.
    anim::SpringParams entranceSpring{260.0, 0.86, 1.0, 0.0};
    double staggerStep = 0.012;      // s between consecutive cards
    double staggerMax = 0.08;        // s, cap on the whole stagger span
    double minimizedStartScale = 0.85;
    // Exit and relayout.
    anim::SpringParams exitSpring{320.0, 0.92, 1.0, 0.0};
    // Background dim.
    double dimOpacity = 0.55;        // 0..1 at rest
    double dimInDuration = 0.28;     // s
    double dimOutDuration = 0.22;    // s
    anim::CubicBezier dimInEasing = anim::easing::emphasized;
    anim::CubicBezier dimOutEasing = anim::easing::easeInOut;
    // Hover (Phase 4 UI; used by the demo now).
    double hoverScale = 1.03;
    anim::SpringParams hoverSpring{420.0, 0.8, 1.0, 0.0};
    // Close / minimize / zoom circles under the hovered card.
    bool trafficLights = true;
    double trafficDiameter = 12.0;   // 96-dpi px
    double trafficSpacing = 20.0;    // between circle centers
    // Experimental: traffic lights over every Microsoft Edge window (only Edge, never other apps).
    // Off by default. `edgePocTitle` empty = all Edge windows; otherwise only windows whose title
    // contains it when first seen (test aid). See docs/compatibility/edge.md.
    bool edgePocEnabled = false;
    std::string edgePocTitle;
    std::vector<std::string> edgePocExtraApps;    // exe names with a Windows-drawn frame to opt in (e.g. "mspaint.exe")
    std::vector<std::string> edgePocExcludeApps;  // exe names never covered
    RegistryData edgePocRegistry;                 // mode, per-app and per-window settings (see app_registry.h)
    // Desktop switch (Phase 3 UI; used by the demo now).
    double desktopDuration = 0.45;   // s
    anim::CubicBezier desktopEasing = anim::easing::emphasized;
    double desktopParallax = 0.25;   // background layer depth, 0 = still, 1 = moves with content
    // Gesture-driven transitions.
    double gestureRubberBand = 0.15; // max overshoot past the ends, in progress units
    double gestureProjection = 0.15; // s of velocity projection when deciding commit vs cancel
    anim::SpringParams gestureSpring{300.0, 0.9, 1.0, 0.0};
    // Background blur of the wallpaper backdrop.
    bool blurEnabled = false;
    double blurIntensity = 0.0;      // 0..1
    // System integration (Phase 5). Win+Tab opens this overview instead of Task View; turning it
    // off (or quitting the app) gives Win+Tab back to Windows. Ctrl+Alt+Space always works.
    bool takeoverWinTab = true;
};

// Returns human-readable errors; empty means valid.
std::vector<std::string> validate(const AnimationConfig& c);

// Parses JSON text over the defaults in `out`. Missing keys keep their default; unknown keys and
// wrong types are errors. Runs validate() too. On any error `out` is left unchanged.
std::vector<std::string> loadAnimationConfig(std::string_view jsonText, AnimationConfig& out);

}  // namespace mc
