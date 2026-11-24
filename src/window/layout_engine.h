#pragma once
// Where a window goes for each tiling layout. Pure geometry (no Win32): physical pixels, a work area in, a rect out.
// The Win32 side (monitor, DPI, SetWindowPos) is src/app/window_tiler.cpp. See docs/decisions/ADR-009-window-tiling.md.
#include "core/geometry.h"

namespace mc {

// Order = order in the menu. Thirds and two-thirds fit the same table (a new row in kFractions).
enum class TileLayout { Full, LeftHalf, RightHalf, TopHalf, BottomHalf, TopLeft, TopRight, BottomLeft, BottomRight };
inline constexpr int kTileLayoutCount = 9;

const char* tileLayoutName(TileLayout l);

// The visible rect of the window for `l` inside `work` (the monitor's work area, taskbar excluded). Edges are rounded
// once per edge, so neighbouring layouts (left / right half, the four quarters) share their edges exactly: no gap, no overlap.
Rect layoutRect(TileLayout l, const Rect& work);

// Windows 10/11 frames have an invisible border (about 8 px at 96 DPI) that GetWindowRect includes and the visible
// frame (DWMWA_EXTENDED_FRAME_BOUNDS) does not.
struct FrameInsets {
    double left = 0, top = 0, right = 0, bottom = 0;
};
FrameInsets scaleInsets(const FrameInsets& in, double factor);  // insets measured at one DPI, used at another
// The rect to give SetWindowPos so that the visible frame ends up at `visible`.
Rect outerRect(const Rect& visible, const FrameInsets& in);
// A window that refused the size (min / max track size): its real size w x h is centred on `desired`, then kept inside `work`.
Rect fitWithin(const Rect& desired, double w, double h, const Rect& work);
// `r` cut to `bounds` (the part of an outer rect that lies on the monitor itself).
Rect clampInside(const Rect& r, const Rect& bounds);
bool sameRect(const Rect& a, const Rect& b, double tolerance);

}  // namespace mc
