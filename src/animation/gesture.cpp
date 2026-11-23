#include "animation/gesture.h"

#include <algorithm>

#include "animation/animation.h"

namespace mc::anim {

namespace {
constexpr double kWindow = 0.1;  // seconds of history used for the velocity estimate
}

void GestureTracker::begin(double t, double rawProgress) {
    samples_.clear();
    update(t, rawProgress);
}

void GestureTracker::update(double t, double rawProgress) {
    raw_ = rawProgress;
    samples_.push_back({t, rawProgress});
    while (samples_.size() > 2 && samples_.front().t < t - kWindow) samples_.pop_front();
}

double GestureTracker::progress() const { return rubberBand(raw_, 0.0, 1.0, range_); }

double GestureTracker::velocity() const {
    const std::size_t n = samples_.size();
    if (n < 2) return 0.0;
    double mt = 0, mp = 0;
    for (auto& s : samples_) { mt += s.t; mp += s.p; }
    mt /= double(n);
    mp /= double(n);
    double num = 0, den = 0;
    for (auto& s : samples_) {
        num += (s.t - mt) * (s.p - mp);
        den += (s.t - mt) * (s.t - mt);
    }
    return den > 1e-12 ? num / den : 0.0;
}

GestureTracker::Release GestureTracker::release(double projection) const {
    const double v = velocity();
    const double projected = raw_ + v * projection;
    return {progress(), projected >= 0.5 ? 1.0 : 0.0, v};
}

}  // namespace mc::anim
