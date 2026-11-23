#pragma once
#include <algorithm>
#include <cmath>

namespace mc {

// Physical-pixel rectangle in double precision. Rounded only when handed to Win32.
struct Rect {
    double x = 0, y = 0, w = 0, h = 0;

    double right() const { return x + w; }
    double bottom() const { return y + h; }
    double cx() const { return x + w * 0.5; }
    double cy() const { return y + h * 0.5; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(double px, double py) const { return px >= x && px < right() && py >= y && py < bottom(); }
    bool intersects(const Rect& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    bool operator==(const Rect&) const = default;
};

inline double lerp(double a, double b, double t) { return a + (b - a) * t; }

inline Rect lerp(const Rect& a, const Rect& b, double t) {
    return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.w, b.w, t), lerp(a.h, b.h, t)};
}

// Scale a rect about its center.
inline Rect scaled(const Rect& r, double s) {
    return {r.cx() - r.w * s * 0.5, r.cy() - r.h * s * 0.5, r.w * s, r.h * s};
}

}  // namespace mc
