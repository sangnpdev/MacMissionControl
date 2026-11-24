#pragma once
// Close / minimize / zoom circles shown under the hovered overview card (macOS style). Pure
// geometry and hit testing; drawing is in SceneRenderer, actions are in App.
#include "core/geometry.h"

namespace mc {

enum class TrafficButton { Close = 0, Minimize = 1, Zoom = 2 };

struct TrafficLightsStyle {  // 96-dpi pixels
    double diameter = 12.0;
    double spacing = 20.0;   // between circle centers
    double inset = 12.0;     // card left edge to the first circle's left edge
    double minHit = 20.0;    // hit targets are at least this big
};

struct TrafficLights {
    Rect circle[3];
    Rect hit[3];
    Rect zone;  // hits plus the gap up to the card: keeps the card hovered while the mouse crosses it
    bool empty() const { return zone.empty(); }
    int hitTest(double x, double y) const;  // TrafficButton index, or -1
};

// The row sits `gap` below the card and is `rowHeight` tall. Empty when the card is too narrow
// for three circles (then the card has no buttons).
TrafficLights layoutTrafficLights(const Rect& card, double gap, double rowHeight, double dpiScale, const TrafficLightsStyle& s);

// Circles centered in `area` (used by the Edge PoC to sit inside the native caption-button strip).
// zone = area. Empty when `area` is too small for the three hit targets.
TrafficLights layoutTrafficLightsIn(const Rect& area, double dpiScale, const TrafficLightsStyle& s);

}  // namespace mc
