#include "ui/tile_menu_layout.h"

namespace mc {

TileMenuGeometry layoutTileMenu(double s) {
    const double pad = std::round(10 * s), tw = std::round(40 * s), th = std::round(28 * s), gap = std::round(6 * s);
    TileMenuGeometry g;
    const double rowW = 5 * tw + 4 * gap;
    g.panel = {0, 0, 2 * pad + rowW, 2 * pad + 2 * th + gap};
    for (int i = 0; i < 5; ++i) g.tile[i] = {pad + i * (tw + gap), pad, tw, th};
    const double off = pad + (rowW - (4 * tw + 3 * gap)) / 2;
    for (int i = 0; i < 4; ++i) g.tile[5 + i] = {std::round(off + i * (tw + gap)), pad + th + gap, tw, th};
    return g;
}

int tileMenuHit(const TileMenuGeometry& g, double x, double y) {
    for (int i = 0; i < kTileLayoutCount; ++i)
        if (g.tile[i].contains(x, y)) return i;
    return -1;
}

Rect placeTileMenu(const Rect& panel, const Rect& anchor, const Rect& work, double gap) {
    double x = std::round(anchor.cx() - panel.w / 2);
    double y = anchor.bottom() + gap;
    if (y + panel.h > work.bottom()) y = anchor.y - gap - panel.h;
    x = std::clamp(x, work.x, std::max(work.x, work.right() - panel.w));
    y = std::clamp(y, work.y, std::max(work.y, work.bottom() - panel.h));
    return {x, std::round(y), panel.w, panel.h};
}

}  // namespace mc
