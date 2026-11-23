#include "animation/animation.h"

#include <algorithm>
#include <cmath>

namespace mc::anim {

namespace {
double bezierCoord(double t, double p1, double p2) {
    // B(t) = 3(1-t)^2 t p1 + 3(1-t) t^2 p2 + t^3
    const double u = 1.0 - t;
    return 3.0 * u * u * t * p1 + 3.0 * u * t * t * p2 + t * t * t;
}
double bezierSlope(double t, double p1, double p2) {
    const double u = 1.0 - t;
    return 3.0 * u * u * p1 + 6.0 * u * t * (p2 - p1) + 3.0 * t * t * (1.0 - p2);
}
// Time for an envelope E * exp(-rate * t) to fall below eps.
double decayTime(double envelope, double rate, double eps) {
    return envelope > eps ? std::log(envelope / eps) / rate : 0.0;
}
constexpr double kMaxSettle = 60.0;  // guards degenerate parameters; validation keeps real configs far below
}  // namespace

double CubicBezier::operator()(double x) const {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    // Solve bezierCoord(t, x1, x2) == x for t: Newton first, bisection fallback.
    double t = x;
    for (int i = 0; i < 8; ++i) {
        const double err = bezierCoord(t, x1, x2) - x;
        if (std::abs(err) < 1e-7) return bezierCoord(t, y1, y2);
        const double d = bezierSlope(t, x1, x2);
        if (std::abs(d) < 1e-6) break;
        t -= err / d;
    }
    double lo = 0.0, hi = 1.0;
    t = x;
    for (int i = 0; i < 60; ++i) {
        const double v = bezierCoord(t, x1, x2);
        if (std::abs(v - x) < 1e-7) break;
        (v < x ? lo : hi) = t;
        t = 0.5 * (lo + hi);
    }
    return bezierCoord(t, y1, y2);
}

// Displacement d(t) = x(t) - to, d(0) = from - to, d'(0) = v0.
//   under-damped:  d = e^(-decay t) (a cos(wd t) + b sin(wd t))
//   critical:      d = (a + b t) e^(-w0 t)
//   over-damped:   d = a e^(r1 t) + b e^(r2 t)          (r1 is the slow root)
Spring::Spring(SpringParams p, double from, double to, double v0, double eps) : to_(to) {
    const double mass = p.mass > 0 ? p.mass : 1.0;
    w0_ = std::sqrt(std::max(p.stiffness, 1e-6) / mass);
    const double zeta = std::max(p.dampingRatio, 1e-4);
    const double d0 = from - to;
    eps = std::max(eps, 1e-12);
    const double epsV = eps * 10.0;
    double tD = 0, tV = 0;

    if (std::abs(zeta - 1.0) < 1e-6) {
        mode_ = Mode::Critical;
        a_ = d0;
        b_ = v0 + w0_ * d0;
        // t e^(-w0 t / 2) <= 2 / (e w0) bounds the linear term, leaving a pure exponential at half rate.
        const double e = std::exp(1.0);
        tD = 2.0 * decayTime(std::abs(a_) + 2.0 * std::abs(b_) / (e * w0_), w0_, eps);
        tV = 2.0 * decayTime(std::abs(b_ - w0_ * a_) + 2.0 * std::abs(b_) / e, w0_, epsV);
    } else if (zeta < 1.0) {
        mode_ = Mode::Under;
        decay_ = zeta * w0_;
        wd_ = w0_ * std::sqrt(1.0 - zeta * zeta);
        a_ = d0;
        b_ = (v0 + decay_ * d0) / wd_;
        tD = decayTime(std::abs(a_) + std::abs(b_), decay_, eps);
        tV = decayTime(std::abs(-decay_ * a_ + wd_ * b_) + std::abs(-decay_ * b_ - wd_ * a_), decay_, epsV);
    } else {
        mode_ = Mode::Over;
        const double s = std::sqrt(zeta * zeta - 1.0);
        r1_ = -w0_ * (zeta - s);
        r2_ = -w0_ * (zeta + s);
        b_ = (v0 - r1_ * d0) / (r2_ - r1_);
        a_ = d0 - b_;
        tD = decayTime(std::abs(a_) + std::abs(b_), -r1_, eps);
        tV = decayTime(std::abs(a_ * r1_) + std::abs(b_ * r2_), -r1_, epsV);
    }
    // The envelope is safe but up to ~1.8x late (oscillation phase is ignored). Walk back from it in
    // 1 ms steps to the last sample still outside tolerance: a few hundred evaluations at most.
    const double bound = std::min(std::max(tD, tV), kMaxSettle);
    constexpr double kStep = 1e-3;
    double t = bound;
    while (t > 0.0 && std::abs(value(t) - to_) < eps && std::abs(velocity(t)) < epsV) t -= kStep;
    settle_ = std::min(bound, std::max(t + kStep, 0.0));
}

double Spring::value(double t) const {
    switch (mode_) {
    case Mode::Under: return to_ + std::exp(-decay_ * t) * (a_ * std::cos(wd_ * t) + b_ * std::sin(wd_ * t));
    case Mode::Critical: return to_ + (a_ + b_ * t) * std::exp(-w0_ * t);
    case Mode::Over: return to_ + a_ * std::exp(r1_ * t) + b_ * std::exp(r2_ * t);
    }
    return to_;
}

double Spring::velocity(double t) const {
    switch (mode_) {
    case Mode::Under:
        return std::exp(-decay_ * t) * ((-decay_ * a_ + wd_ * b_) * std::cos(wd_ * t) + (-decay_ * b_ - wd_ * a_) * std::sin(wd_ * t));
    case Mode::Critical: return std::exp(-w0_ * t) * (b_ - w0_ * (a_ + b_ * t));
    case Mode::Over: return a_ * r1_ * std::exp(r1_ * t) + b_ * r2_ * std::exp(r2_ * t);
    }
    return 0.0;
}

Curve Curve::timed(double durationSec, CubicBezier bez) { return Curve(Timed{std::max(durationSec, 0.0), bez}); }
Curve Curve::spring(SpringParams p) { return Curve(Spring(p)); }

double Curve::value(double t) const {
    if (auto* tm = std::get_if<Timed>(&impl_)) {
        if (tm->duration <= 0.0 || t >= tm->duration) return 1.0;
        return t <= 0.0 ? 0.0 : tm->bez(t / tm->duration);
    }
    const auto& sp = std::get<Spring>(impl_);
    if (t <= 0.0) return 0.0;
    return t >= sp.settleTime() ? 1.0 : sp.value(t);
}

double Curve::duration() const {
    if (auto* tm = std::get_if<Timed>(&impl_)) return tm->duration;
    return std::get<Spring>(impl_).settleTime();
}

// ---------- AnimatedValue ----------

void AnimatedValue::jump(double v, double velocity) {
    from_ = v;
    motion_ = Rest{velocity};
}

void AnimatedValue::springTo(double target, double now, const SpringParams& p, double eps, double delay) {
    const double cur = value(now);
    double v = velocity(now) + p.initialVelocity * (target - cur);
    if (delay > 0.0) v = 0.0;  // a delayed start launches from rest
    from_ = cur;
    start_ = now + std::max(delay, 0.0);
    motion_ = Spring(p, cur, target, v, eps);
}

void AnimatedValue::easeTo(double target, double now, double duration, CubicBezier bez, double delay) {
    from_ = value(now);
    start_ = now + std::max(delay, 0.0);
    motion_ = Timed{target, std::max(duration, 0.0), bez};
}

double AnimatedValue::value(double now) const {
    const double t = now - start_;
    if (std::holds_alternative<Rest>(motion_) || t <= 0.0) return from_;
    if (auto* sp = std::get_if<Spring>(&motion_)) return t >= sp->settleTime() ? sp->target() : sp->value(t);
    const auto& tm = std::get<Timed>(motion_);
    if (t >= tm.duration) return tm.to;
    return from_ + (tm.to - from_) * tm.bez(t / tm.duration);
}

double AnimatedValue::velocity(double now) const {
    if (auto* r = std::get_if<Rest>(&motion_)) return r->velocity;
    const double t = now - start_;
    if (t <= 0.0) return 0.0;
    if (auto* sp = std::get_if<Spring>(&motion_)) return t >= sp->settleTime() ? 0.0 : sp->velocity(t);
    const auto& tm = std::get<Timed>(motion_);
    if (t >= tm.duration) return 0.0;
    const double h = std::min(1e-4, tm.duration * 1e-3);  // central difference: Bézier has no closed-form dx/dt
    return (value(now + h) - value(now - h)) / (2.0 * h);
}

double AnimatedValue::target() const {
    if (auto* sp = std::get_if<Spring>(&motion_)) return sp->target();
    if (auto* tm = std::get_if<Timed>(&motion_)) return tm->to;
    return from_;
}

bool AnimatedValue::settled(double now) const {
    if (std::holds_alternative<Rest>(motion_)) return true;
    const double t = now - start_;
    if (auto* sp = std::get_if<Spring>(&motion_)) return t >= sp->settleTime();
    return t >= std::get<Timed>(motion_).duration;
}

// ---------- helpers ----------

double staggerDelay(std::size_t index, std::size_t count, double step, double maxTotal) {
    if (count <= 1 || step <= 0.0) return 0.0;
    const double span = std::min(step * static_cast<double>(count - 1), std::max(maxTotal, 0.0));
    return span * static_cast<double>(index) / static_cast<double>(count - 1);
}

double rubberBand(double x, double lo, double hi, double range) {
    if (range <= 0.0) return std::clamp(x, lo, hi);
    constexpr double c = 0.55;  // initial resistance: 55% of the finger movement
    auto band = [&](double over) { return range * (1.0 - 1.0 / (over * c / range + 1.0)); };
    if (x > hi) return hi + band(x - hi);
    if (x < lo) return lo - band(lo - x);
    return x;
}

}  // namespace mc::anim
