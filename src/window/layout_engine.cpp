#include "window/layout_engine.h"

namespace mc {
namespace {
struct Fractions {
    double l, t, r, b;
};
constexpr Fractions kFractions[kTileLayoutCount] = {
    {0, 0, 1, 1},      // Full
    {0, 0, .5, 1},     // LeftHalf
    {.5, 0, 1, 1},     // RightHalf
    {0, 0, 1, .5},     // TopHalf
    {0, .5, 1, 1},     // BottomHalf
    {0, 0, .5, .5},    // TopLeft
    {.5, 0, 1, .5},    // TopRight
    {0, .5, .5, 1},    // BottomLeft
    {.5, .5, 1, 1},    // BottomRight
};
constexpr const char* kNames[kTileLayoutCount] = {"Fill",       "Left Half",   "Right Half",   "Top Half",     "Bottom Half",
                                                  "Top Left",   "Top Right",   "Bottom Left",  "Bottom Right"};
}  // namespace

const char* tileLayoutName(TileLayout l) {
    const int i = static_cast<int>(l);
    return i >= 0 && i < kTileLayoutCount ? kNames[i] : "";
}

Rect layoutRect(TileLayout l, const Rect& work) {
    const int i = static_cast<int>(l);
    if (i < 0 || i >= kTileLayoutCount) return work;
    const Fractions& f = kFractions[i];
    const double x0 = std::round(work.x + work.w * f.l), x1 = std::round(work.x + work.w * f.r);
    const double y0 = std::round(work.y + work.h * f.t), y1 = std::round(work.y + work.h * f.b);
    return {x0, y0, x1 - x0, y1 - y0};
}

FrameInsets scaleInsets(const FrameInsets& in, double factor) {
    return {std::round(in.left * factor), std::round(in.top * factor), std::round(in.right * factor), std::round(in.bottom * factor)};
}

Rect outerRect(const Rect& v, const FrameInsets& in) {
    return {v.x - in.left, v.y - in.top, v.w + in.left + in.right, v.h + in.top + in.bottom};
}

Rect fitWithin(const Rect& desired, double w, double h, const Rect& work) {
    double x = desired.x + (desired.w - w) * 0.5, y = desired.y + (desired.h - h) * 0.5;
    x = std::clamp(x, work.x, std::max(work.x, work.right() - w));
    y = std::clamp(y, work.y, std::max(work.y, work.bottom() - h));
    return {std::round(x), std::round(y), w, h};
}

Rect clampInside(const Rect& r, const Rect& b) {
    const double x0 = std::max(r.x, b.x), y0 = std::max(r.y, b.y), x1 = std::min(r.right(), b.right()), y1 = std::min(r.bottom(), b.bottom());
    return {x0, y0, std::max(0.0, x1 - x0), std::max(0.0, y1 - y0)};
}

bool sameRect(const Rect& a, const Rect& b, double tol) {
    return std::abs(a.x - b.x) <= tol && std::abs(a.y - b.y) <= tol && std::abs(a.w - b.w) <= tol && std::abs(a.h - b.h) <= tol;
}

}  // namespace mc
