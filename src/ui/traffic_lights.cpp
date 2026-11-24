#include "ui/traffic_lights.h"

#include <algorithm>

namespace mc {

int TrafficLights::hitTest(double x, double y) const {
    if (zone.empty()) return -1;
    for (int i = 0; i < 3; ++i)
        if (hit[i].contains(x, y)) return i;
    return -1;
}

TrafficLights layoutTrafficLights(const Rect& card, double gap, double rowHeight, double dpi, const TrafficLightsStyle& s) {
    TrafficLights t;
    const double d = s.diameter * dpi, step = s.spacing * dpi, inset = s.inset * dpi;
    const double hitSize = std::max(s.minHit * dpi, d);
    const double width = inset + 2 * step + d;
    if (card.empty() || rowHeight <= 0 || card.w < width + inset) return t;
    const double cy = card.bottom() + gap + rowHeight / 2;
    for (int i = 0; i < 3; ++i) {
        const double cx = card.x + inset + d / 2 + step * i;
        t.circle[i] = {cx - d / 2, cy - d / 2, d, d};
        t.hit[i] = {cx - hitSize / 2, cy - hitSize / 2, hitSize, hitSize};
    }
    t.zone = {t.hit[0].x, card.bottom(), t.hit[2].right() - t.hit[0].x, gap + rowHeight};
    return t;
}

TrafficLights layoutTrafficLightsIn(const Rect& area, double dpi, const TrafficLightsStyle& s) {
    TrafficLights t;
    const double d = s.diameter * dpi, step = s.spacing * dpi, hitSize = std::max(s.minHit * dpi, d);
    const double groupW = 2 * step + d;
    if (area.empty() || area.w < 2 * step + hitSize || area.h < hitSize * 0.5) return t;
    const double cy = area.cy();
    const double hitH = std::min(hitSize, area.h);  // a short strip clips the hit target vertically
    for (int i = 0; i < 3; ++i) {
        const double cx = area.cx() - groupW / 2 + d / 2 + step * i;
        t.circle[i] = {cx - d / 2, cy - d / 2, d, d};
        t.hit[i] = {cx - hitSize / 2, cy - hitH / 2, hitSize, hitH};
    }
    t.zone = area;
    return t;
}

}  // namespace mc
