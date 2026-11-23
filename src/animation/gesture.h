#pragma once
// Gesture-driven progress: input positions map straight to progress; on release the tracker
// decides where to settle and with what velocity, so a spring can continue the motion.
#include <deque>

namespace mc::anim {

class GestureTracker {
public:
    struct Release {
        double from;      // displayed progress at release
        double target;    // 0 or 1
        double velocity;  // progress units per second at release
    };

    // `range`: how far past [0,1] the rubber band may stretch, in progress units.
    explicit GestureTracker(double range = 0.15) : range_(range) {}

    void begin(double t, double rawProgress);
    void update(double t, double rawProgress);  // raw = finger position mapped to progress, unclamped

    double rawProgress() const { return raw_; }
    double progress() const;   // rubber-banded raw progress
    double velocity() const;   // least-squares slope of raw progress over the last 100 ms
    // Commit if the position projected `projection` seconds ahead passes the midpoint.
    Release release(double projection = 0.15) const;

private:
    struct Sample { double t, p; };
    std::deque<Sample> samples_;
    double raw_ = 0.0, range_;
};

}  // namespace mc::anim
