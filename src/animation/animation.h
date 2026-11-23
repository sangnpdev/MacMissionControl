#pragma once
// Pure, time-based animation primitives. No Win32 dependency.
// Every value is a closed-form function of elapsed time, so results do not depend on frame rate.
#include <cstddef>
#include <variant>

namespace mc::anim {

// CSS-style cubic Bézier timing function with P0=(0,0), P3=(1,1).
struct CubicBezier {
    double x1 = 0, y1 = 0, x2 = 1, y2 = 1;
    double operator()(double t) const;  // t in [0,1] -> eased progress
    bool operator==(const CubicBezier&) const = default;
};

namespace easing {
inline constexpr CubicBezier linear{0.0, 0.0, 1.0, 1.0};
inline constexpr CubicBezier easeIn{0.42, 0.0, 1.0, 1.0};
inline constexpr CubicBezier easeOut{0.0, 0.0, 0.58, 1.0};
inline constexpr CubicBezier easeInOut{0.42, 0.0, 0.58, 1.0};
// Fast start, long soft landing. Default for UI transitions.
inline constexpr CubicBezier emphasized{0.2, 0.0, 0.0, 1.0};
}  // namespace easing

struct SpringParams {
    double stiffness = 300.0;     // k (N/m with unit mass)
    double dampingRatio = 0.85;   // zeta: <1 under-damped, 1 critical, >1 over-damped
    double mass = 1.0;
    double initialVelocity = 0.0; // extra launch velocity, in "whole distance per second" units
    bool operator==(const SpringParams&) const = default;
};

// Damped harmonic oscillator from `from` to `to` with initial velocity `v0` (units/s), solved
// analytically. settleTime() comes from the decay envelope (no sampling), so construction is cheap.
class Spring {
public:
    // eps: displacement tolerance in value units; velocity tolerance is 10 * eps per second.
    Spring(SpringParams p, double from, double to, double v0, double eps);
    // Unit spring 0 -> 1, launched with p.initialVelocity.
    explicit Spring(SpringParams p) : Spring(p, 0.0, 1.0, p.initialVelocity, 1e-3) {}

    double value(double t) const;
    double velocity(double t) const;
    double settleTime() const { return settle_; }
    double target() const { return to_; }

private:
    enum class Mode { Under, Critical, Over } mode_;
    double to_, w0_, a_, b_;  // displacement d(t) = x(t) - to; a_ = d(0)
    double wd_ = 0, decay_ = 0, r1_ = 0, r2_ = 0;
    double settle_ = 0;
};

// A progress curve from 0 to 1: either a timed Bézier or a spring.
class Curve {
public:
    static Curve timed(double durationSec, CubicBezier bez);
    static Curve spring(SpringParams p);

    double value(double t) const;  // clamped to the curve's end once finished
    double duration() const;       // seconds until the curve rests at 1

private:
    struct Timed { double duration; CubicBezier bez; };
    explicit Curve(Timed t) : impl_(t) {}
    explicit Curve(Spring s) : impl_(s) {}
    std::variant<Timed, Spring> impl_;
};

// A curve started after a delay.
struct Track {
    double delay = 0;
    Curve curve = Curve::timed(0.0, easing::linear);

    double progress(double t) const { return t <= delay ? 0.0 : curve.value(t - delay); }
    double endTime() const { return delay + curve.duration(); }
    bool done(double t) const { return t >= endTime(); }
};

// A scalar that can be retargeted at any time. A spring retarget keeps the current velocity, so an
// interrupted animation (open, then close at once) changes direction without a jolt.
// Times are absolute seconds on one clock (the compositor clock).
class AnimatedValue {
public:
    explicit AnimatedValue(double v = 0.0) : from_(v) {}

    // Jump to `v`. `velocity` is not applied now; the next springTo() launches with it (gesture release).
    void jump(double v, double velocity = 0.0);
    // Spring to `target` from the current state. Launch velocity = current velocity
    // + p.initialVelocity * (target - current). `delay` holds the value still before it starts.
    void springTo(double target, double now, const SpringParams& p, double eps, double delay = 0.0);
    // Timed Bézier to `target`. Does not carry velocity.
    void easeTo(double target, double now, double duration, CubicBezier bez, double delay = 0.0);

    double value(double now) const;
    double velocity(double now) const;
    double target() const;
    bool settled(double now) const;

private:
    struct Rest { double velocity; };
    struct Timed { double to, duration; CubicBezier bez; };
    double from_;       // value at start_
    double start_ = 0;  // motion begins here (after any delay)
    std::variant<Rest, Spring, Timed> motion_{Rest{0.0}};
};

// Delay for item `index` of `count`, `step` apart, compressed so the last one starts by `maxTotal`.
double staggerDelay(std::size_t index, std::size_t count, double step, double maxTotal);

// Offset of a layer during a horizontal transition: depth 1 moves with the content, 0 stays still.
inline double parallaxOffset(double progress, double distance, double depth) { return -progress * distance * depth; }

// Resistance past the ends of [lo, hi]: the overshoot approaches `range` but never exceeds it.
double rubberBand(double x, double lo, double hi, double range);

}  // namespace mc::anim
