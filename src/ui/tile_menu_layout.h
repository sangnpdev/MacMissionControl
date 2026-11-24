#pragma once
// Geometry of the tiling menu (the popup under the green traffic light). Pure: drawing is src/app/tiling_menu.cpp.
#include "core/geometry.h"
#include "window/layout_engine.h"

namespace mc {

struct TileMenuGeometry {
    Rect panel;                       // at (0, 0), physical px
    Rect tile[kTileLayoutCount];      // panel-local; index = TileLayout
};

// Row 1: Fill + the four halves. Row 2: the four quarters, centred. `dpiScale` 1.0 = 96 DPI.
TileMenuGeometry layoutTileMenu(double dpiScale);
int tileMenuHit(const TileMenuGeometry& g, double x, double y);  // TileLayout index, or -1

// Screen rect of the menu: centred under `anchor` (the green dot horizontally, the whole strip vertically), `gap` below
// it; flipped above when there is no room below; always inside `work`.
Rect placeTileMenu(const Rect& panel, const Rect& anchor, const Rect& work, double gap);

}  // namespace mc
