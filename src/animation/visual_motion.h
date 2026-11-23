#pragma once
// Translation + uniform scale + opacity of a rectangle with a fixed aspect ratio. Used for window
// cards and for shared-element transitions (a card flying between two layouts).
#include <algorithm>

#include "animation/animation.h"
#include "core/geometry.h"

namespace mc::anim {

class VisualMotion {
public:
    VisualMotion() = default;
    // `base` fixes the aspect ratio and the size at scale 1.
    VisualMotion(double baseW, double baseH) : baseW_(std::max(baseW, 1.0)), baseH_(std::max(baseH, 1.0)) {}

    void jump(const Rect& r, double opacity) {
        cx_.jump(r.cx());
        cy_.jump(r.cy());
        scale_.jump(r.w / baseW_);
        opacity_.jump(opacity);
    }

    // Gesture scrub: place at lerp(a, b, p) moving with dp/dt = pVelocity, so a following springTo
    // continues the finger's motion.
    void scrub(const Rect& a, const Rect& b, double p, double pVelocity, double opacity) {
        const Rect r = lerp(a, b, p);
        cx_.jump(r.cx(), (b.cx() - a.cx()) * pVelocity);
        cy_.jump(r.cy(), (b.cy() - a.cy()) * pVelocity);
        scale_.jump(r.w / baseW_, (b.w - a.w) / baseW_ * pVelocity);
        opacity_.jump(opacity);
    }

    // Retargets all channels; current velocities carry over. Only r's width sets the scale.
    void springTo(const Rect& r, double opacity, double now, const SpringParams& p, double delay = 0.0) {
        cx_.springTo(r.cx(), now, p, kPositionEps, delay);
        cy_.springTo(r.cy(), now, p, kPositionEps, delay);
        scale_.springTo(r.w / baseW_, now, p, kPositionEps / baseW_, delay);
        opacity_.springTo(opacity, now, p, kOpacityEps, delay);
    }

    // Timed Bézier on all channels (desktop transitions). Does not carry velocity.
    void easeTo(const Rect& r, double opacity, double now, double duration, CubicBezier bez, double delay = 0.0) {
        cx_.easeTo(r.cx(), now, duration, bez, delay);
        cy_.easeTo(r.cy(), now, duration, bez, delay);
        scale_.easeTo(r.w / baseW_, now, duration, bez, delay);
        opacity_.easeTo(opacity, now, duration, bez, delay);
    }

    Rect rect(double now) const {
        const double s = scale_.value(now), w = baseW_ * s, h = baseH_ * s;
        return {cx_.value(now) - w * 0.5, cy_.value(now) - h * 0.5, w, h};
    }
    double opacity(double now) const { return std::clamp(opacity_.value(now), 0.0, 1.0); }
    bool settled(double now) const {
        return cx_.settled(now) && cy_.settled(now) && scale_.settled(now) && opacity_.settled(now);
    }

private:
    static constexpr double kPositionEps = 0.5;         // px: DWM rounds to whole pixels, so smaller moves are invisible
    static constexpr double kOpacityEps = 1.0 / 512.0;  // half of one 8-bit opacity step
    double baseW_ = 1, baseH_ = 1;
    AnimatedValue cx_, cy_, scale_, opacity_;
};

}  // namespace mc::anim
