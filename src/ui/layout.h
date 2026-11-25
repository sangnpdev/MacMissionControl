#pragma once
#include <vector>

#include "core/geometry.h"

namespace mc {

struct LayoutParams {
    double spacing = 16.0;     // gap between cards, physical px
    double padding = 32.0;     // inset from the area edges, physical px
    double maxScale = 0.85;    // cards never grow beyond this fraction of the real size
    double labelHeight = 0.0;  // space reserved under every card for its label, physical px
};

// Adaptive spatial layout (ADR-006): binary space partition seeded by the windows' real positions,
// each card fitted into its own cell. Returns one card rect per input rect, in the same order.
// Aspect ratios are preserved, results never overlap (labels included) and stay inside `area`
// (minus padding). Deterministic.
//
// `previous` (empty, or one rect per window; an empty rect marks a new window) makes it stable:
// cards that already had a slot keep it and new windows go into free space. A full relayout
// happens only when that does not fit, or when closed windows freed much more room.
std::vector<Rect> layoutOverview(const Rect& area, const std::vector<Rect>& windows, const LayoutParams& p,
                                 const std::vector<Rect>& previous = {});

// Desktop strip along the top of a monitor.
struct StripParams {
    double heightFraction = 0.13;  // of the monitor height
    double minHeight = 90.0, maxHeight = 190.0;
    double padding = 12.0;
    double gap = 18.0;
    double nameHeight = 22.0;      // under each tile
    double closeSize = 18.0;       // "remove desktop" button; centered on a tile's top-left corner, so keep it <= 2 * padding
};
struct StripLayout {
    Rect band;                     // whole strip; the overview layout starts below it
    std::vector<Rect> tiles;       // one per desktop, left to right
    Rect plus;                     // "new desktop" button; empty if not shown
    std::vector<Rect> closes;      // "remove desktop" button per tile; empty with fewer than 2 desktops
};
StripLayout layoutStrip(const Rect& monitor, int count, double aspect, bool showPlus, const StripParams& p);

// Keyboard navigation between cards. `current` -1 picks the top-left card. Returns `current` when
// there is nothing in that direction.
enum class NavDir { Left, Right, Up, Down };
int navigate(const std::vector<Rect>& rects, int current, NavDir dir);

}  // namespace mc
