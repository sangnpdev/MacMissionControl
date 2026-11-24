#include "test_main.h"

#include <cstdio>

#include "animation/animation.h"
#include "animation/gesture.h"
#include "animation/visual_motion.h"
#include "config/animation_config.h"
#include "config/json.h"
#include "desktop/desktop_state.h"
#include "input/wintab_detector.h"
#include "app/autostart.h"
#include "ui/layout.h"
#include "window/window_filter.h"

using namespace mc;
using namespace mc::anim;

// ---------- Bézier ----------
TEST(bezier_endpoints_and_linear) {
    for (auto b : {easing::linear, easing::easeIn, easing::easeOut, easing::easeInOut, easing::emphasized}) {
        CHECK_NEAR(b(0.0), 0.0, 1e-9);
        CHECK_NEAR(b(1.0), 1.0, 1e-9);
        CHECK_NEAR(b(-1.0), 0.0, 1e-9);
        CHECK_NEAR(b(2.0), 1.0, 1e-9);
    }
    for (double x = 0; x <= 1.0; x += 0.1) CHECK_NEAR(easing::linear(x), x, 1e-6);
}

TEST(bezier_shapes_and_monotonic) {
    CHECK(easing::easeIn(0.5) < 0.5);
    CHECK(easing::easeOut(0.5) > 0.5);
    CHECK_NEAR(easing::easeInOut(0.5), 0.5, 1e-5);
    double prev = 0;
    for (int i = 1; i <= 1000; ++i) {
        const double v = easing::emphasized(i / 1000.0);
        CHECK(v >= prev - 1e-9);
        prev = v;
    }
}

TEST(bezier_known_value) {
    // CSS "ease" (0.25,0.1,0.25,1) at x=0.5 is ~0.8024 (reference value from browser engines).
    CHECK_NEAR((CubicBezier{0.25, 0.1, 0.25, 1.0})(0.5), 0.8024, 1e-3);
}

// ---------- Spring ----------
TEST(spring_starts_at_zero_with_initial_velocity) {
    for (double z : {0.3, 1.0, 2.0}) {
        Spring s({200, z, 1, 3.0});
        CHECK_NEAR(s.value(0), 0.0, 1e-9);
        CHECK_NEAR(s.velocity(0), 3.0, 1e-9);
    }
}

TEST(spring_converges_and_settles) {
    for (double z : {0.3, 0.86, 1.0, 1.5}) {
        Spring s({300, z, 1, 0});
        const double T = s.settleTime();
        CHECK(T > 0.05 && T < 10.0);
        for (double t = T; t < T + 2.0; t += 0.01) CHECK(std::abs(s.value(t) - 1.0) < 1e-3);
    }
}

TEST(spring_velocity_matches_derivative) {
    for (double z : {0.4, 1.0, 1.7}) {
        Spring s({250, z, 1.3, -1.0});
        for (double t = 0.01; t < 1.0; t += 0.05) {
            const double h = 1e-6;
            CHECK_NEAR(s.velocity(t), (s.value(t + h) - s.value(t - h)) / (2 * h), 1e-3);
        }
    }
}

TEST(spring_underdamped_overshoots_critical_does_not) {
    Spring under({300, 0.3, 1, 0}), crit({300, 1.0, 1, 0});
    double maxU = 0, maxC = 0;
    for (double t = 0; t < 3; t += 0.001) { maxU = std::max(maxU, under.value(t)); maxC = std::max(maxC, crit.value(t)); }
    CHECK(maxU > 1.1);
    CHECK(maxC <= 1.0 + 1e-9);
}

TEST(spring_matches_numeric_integration) {
    // Semi-implicit Euler with a tiny step as an independent reference.
    SpringParams p{280, 0.7, 1.0, 0.5};
    Spring s(p);
    double x = 0, v = p.initialVelocity, t = 0;
    const double dt = 1e-5, c = 2 * p.dampingRatio * std::sqrt(p.stiffness * p.mass);
    while (t < 0.5) { v += (-p.stiffness * (x - 1) - c * v) / p.mass * dt; x += v * dt; t += dt; }
    CHECK_NEAR(s.value(t), x, 2e-3);
}

// ---------- Curve / Track / stagger ----------
TEST(curve_timed_and_track_delay) {
    Track tr{0.1, Curve::timed(0.2, easing::linear)};
    CHECK_NEAR(tr.progress(0.05), 0.0, 1e-9);
    CHECK_NEAR(tr.progress(0.2), 0.5, 1e-6);
    CHECK_NEAR(tr.progress(0.35), 1.0, 1e-9);
    CHECK(!tr.done(0.29));
    CHECK(tr.done(0.3 + 1e-9));  // 0.1 + 0.2 is slightly above 0.3 in binary floating point
    CHECK_NEAR(Curve::timed(0.0, easing::linear).value(0.0), 1.0, 1e-9);  // zero duration jumps to end
}

TEST(curve_spring_clamps_after_settle) {
    Curve c = Curve::spring({300, 0.5, 1, 0});
    CHECK_NEAR(c.value(c.duration() + 0.001), 1.0, 1e-12);
    CHECK_NEAR(c.value(-1), 0.0, 1e-12);
}

TEST(stagger_delay) {
    CHECK_NEAR(staggerDelay(0, 1, 0.02, 0.1), 0.0, 1e-12);
    CHECK_NEAR(staggerDelay(3, 4, 0.02, 1.0), 0.06, 1e-12);
    CHECK_NEAR(staggerDelay(29, 30, 0.02, 0.1), 0.1, 1e-12);  // compressed to maxTotal
    CHECK_NEAR(staggerDelay(15, 31, 0.02, 0.1), 0.05, 1e-12);
}

// ---------- Layout ----------
static void checkLayout(const Rect& area, const std::vector<Rect>& in, const LayoutParams& p) {
    auto out = layoutOverview(area, in, p);
    CHECK(out.size() == in.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Rect& r = out[i];
        CHECK(!r.empty());
        CHECK(r.x >= area.x + p.padding - 1e-6 && r.right() <= area.right() - p.padding + 1e-6);
        CHECK(r.y >= area.y + p.padding - 1e-6 && r.bottom() <= area.bottom() - p.padding + 1e-6);
        CHECK_NEAR(r.w / r.h, in[i].w / in[i].h, 1e-9);           // aspect preserved
        CHECK(r.w <= in[i].w * p.maxScale + 1e-6);                 // never upscaled past cap
        for (std::size_t j = i + 1; j < out.size(); ++j) CHECK(!r.intersects(out[j]));
    }
}

TEST(layout_counts_5_10_20_30_100) {
    const Rect area{0, 0, 2880, 1800};
    for (int n : {1, 2, 5, 10, 20, 30, 100}) {
        std::vector<Rect> in;
        for (int i = 0; i < n; ++i) in.push_back({double(i * 37 % 900), double(i * 53 % 700), 600.0 + (i % 5) * 150, 400.0 + (i % 3) * 200});
        checkLayout(area, in, {});
    }
}

TEST(layout_offset_monitor_and_extreme_aspect) {
    checkLayout({-1920, 200, 1920, 1040}, {{-1900, 220, 3000, 100}, {-1000, 300, 100, 3000}, {-500, 500, 800, 600}}, {});
}

TEST(layout_single_window_centered_and_capped) {
    LayoutParams p;
    auto out = layoutOverview({0, 0, 2000, 1000}, {{0, 0, 400, 300}}, p);
    CHECK_NEAR(out[0].w, 400 * p.maxScale, 1e-6);
    CHECK_NEAR(out[0].cx(), 1000, 1e-6);
    CHECK_NEAR(out[0].cy(), 500, 1e-6);
}

TEST(layout_degenerate_inputs) {
    CHECK(layoutOverview({0, 0, 100, 100}, {}, {}).empty());
    auto out = layoutOverview({0, 0, 50, 50}, {{0, 0, 10, 10}}, {});  // padding eats the area
    CHECK(out.size() == 1 && out[0].empty());
    auto z = layoutOverview({0, 0, 1000, 1000}, {{0, 0, 0, 0}}, {});  // zero-size window does not crash
    CHECK(z.size() == 1);
}

TEST(layout_keeps_reading_order) {
    // Two windows side by side should stay left/right of each other.
    auto out = layoutOverview({0, 0, 2000, 1000}, {{1000, 0, 800, 600}, {0, 0, 800, 600}}, {});
    CHECK(out[1].x < out[0].x);
}

// ---------- Adaptive spatial layout ----------
static std::vector<Rect> mixedWindows(int n) {
    std::vector<Rect> in;
    for (int i = 0; i < n; ++i) in.push_back({double(i * 211 % 1500), double(i * 97 % 800), 500.0 + (i * 7 % 5) * 250, 300.0 + (i * 3 % 4) * 220});
    return in;
}

TEST(layout_is_deterministic) {
    const auto in = mixedWindows(11);
    CHECK(layoutOverview({0, 0, 1920, 1080}, in, {}) == layoutOverview({0, 0, 1920, 1080}, in, {}));
}

TEST(layout_sizes_differ_but_stay_comparable) {
    LayoutParams p;
    p.labelHeight = 30;
    const auto in = mixedWindows(9);
    const auto out = layoutOverview({0, 0, 1920, 1080}, in, p);
    double lo = 1e9, hi = 0, minW = 1e9, maxW = 0;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const double s = out[i].w / in[i].w;
        lo = std::min(lo, s); hi = std::max(hi, s); minW = std::min(minW, out[i].w); maxW = std::max(maxW, out[i].w);
    }
    CHECK(maxW > minW * 1.3);  // cards are not one size
    CHECK(hi < lo * 4.0);   // but no tiny/huge extremes
}

TEST(layout_stable_when_window_closes) {
    LayoutParams p;
    p.labelHeight = 30;
    const Rect area{0, 0, 1920, 1080};
    auto in = mixedWindows(12);
    const auto a = layoutOverview(area, in, p);
    in.erase(in.begin() + 5);
    std::vector<Rect> prev = a;
    prev.erase(prev.begin() + 5);
    const auto b = layoutOverview(area, in, p, prev);
    CHECK(b.size() == 11);
    for (std::size_t i = 0; i < b.size(); ++i) CHECK(b[i] == prev[i]);
}

TEST(layout_new_window_goes_to_free_space_without_moving_others) {
    LayoutParams p;
    p.labelHeight = 30;
    const Rect area{0, 0, 1920, 1080};
    auto in = mixedWindows(2);
    const auto a = layoutOverview(area, in, p);
    in.push_back({100, 100, 400, 300});
    std::vector<Rect> prev = a;
    prev.push_back({});  // new window
    const auto b = layoutOverview(area, in, p, prev);
    CHECK(b.size() == 3);
    for (std::size_t i = 0; i < 2; ++i) CHECK(b[i] == a[i]);
    CHECK(!b[2].empty());
    CHECK_NEAR(b[2].w / b[2].h, 400.0 / 300.0, 1e-9);
    for (std::size_t i = 0; i < 2; ++i) CHECK(!b[2].intersects(Rect{b[i].x, b[i].y, b[i].w, b[i].h + p.labelHeight}));
    CHECK(b[2].x >= p.padding && b[2].right() <= area.right() - p.padding);
    CHECK(b[2].y >= p.padding && b[2].bottom() + p.labelHeight <= area.bottom() - p.padding);
}

TEST(layout_full_relayout_when_no_room_or_many_closed) {
    LayoutParams p;
    const Rect area{0, 0, 1920, 1080};
    auto in = mixedWindows(20);
    const auto a = layoutOverview(area, in, p);
    in.push_back({0, 0, 1600, 1000});
    std::vector<Rect> prev = a;
    prev.push_back({});
    const auto b = layoutOverview(area, in, p, prev);  // no free slot that large: everything is re-laid out
    for (std::size_t i = 0; i < b.size(); ++i) {
        CHECK(!b[i].empty());
        for (std::size_t j = i + 1; j < b.size(); ++j) CHECK(!b[i].intersects(b[j]));
    }
    // 20 -> 1 window: the survivor grows instead of staying tiny.
    std::vector<Rect> one{in[0]}, prevOne{a[0]};
    CHECK(layoutOverview(area, one, p, prevOne)[0].w > a[0].w * 1.25);
}

TEST(layout_spacing_between_cards) {
    LayoutParams p;
    const auto out = layoutOverview({0, 0, 1920, 1080}, mixedWindows(8), p);
    for (std::size_t i = 0; i < out.size(); ++i)
        for (std::size_t j = i + 1; j < out.size(); ++j)
            CHECK(!scaled(out[i], 1.0).intersects(Rect{out[j].x - p.spacing + 1e-6, out[j].y - p.spacing + 1e-6, out[j].w + 2 * p.spacing - 2e-6, out[j].h + 2 * p.spacing - 2e-6}));
}

// ---------- Window filter ----------
static WindowFacts normal() {
    WindowFacts f;
    f.visible = true; f.width = 800; f.height = 600; f.className = L"Notepad"; f.title = L"x";
    return f;
}

TEST(filter_rules) {
    WindowFilterRules r;
    CHECK(includeWindow(normal(), r));
    auto f = normal(); f.visible = false; CHECK(!includeWindow(f, r));
    f = normal(); f.cloaked = true; CHECK(!includeWindow(f, r));
    f = normal(); f.ownProcess = true; CHECK(!includeWindow(f, r));
    f = normal(); f.onCurrentDesktop = false; CHECK(!includeWindow(f, r));
    f = normal(); f.toolWindow = true; CHECK(!includeWindow(f, r));
    f.appWindow = true; CHECK(includeWindow(f, r));
    f = normal(); f.hasOwner = true; CHECK(!includeWindow(f, r));
    f.appWindow = true; CHECK(includeWindow(f, r));
    f = normal(); f.noActivate = true; CHECK(!includeWindow(f, r));
    f = normal(); f.width = 10; CHECK(!includeWindow(f, r));
    f = normal(); f.title.clear(); CHECK(!includeWindow(f, r));
    f = normal(); f.className = L"Progman"; CHECK(!includeWindow(f, r));
    r.allowClasses = {L"MCBenchWindow"};
    CHECK(!includeWindow(normal(), r));
    f = normal(); f.className = L"MCBenchWindow"; CHECK(includeWindow(f, r));
}

// ---------- Config ----------
TEST(config_validation) {
    AnimationConfig c;
    CHECK(validate(c).empty());
    c.dimOpacity = 1.5; c.entranceSpring.stiffness = -1; c.staggerStep = std::nan("");
    CHECK(validate(c).size() == 3);
    AnimationConfig d;
    d.dimInEasing = {1.5, 0, 0.5, 1};  // x outside [0,1]
    CHECK(validate(d).size() == 1);
}

// ---------- Phase 2: general spring ----------
TEST(spring_general_from_to_velocity) {
    for (double z : {0.3, 1.0, 1.8}) {
        Spring s({250, z, 1, 0}, 40.0, -10.0, 300.0, 0.25);
        CHECK_NEAR(s.value(0), 40.0, 1e-9);
        CHECK_NEAR(s.velocity(0), 300.0, 1e-6);
        const double T = s.settleTime();
        for (double t = T; t < T + 1.0; t += 0.005) {
            CHECK(std::abs(s.value(t) + 10.0) < 0.25);
            CHECK(std::abs(s.velocity(t)) < 2.5);
        }
    }
    Spring still({300, 0.8, 1, 0}, 5.0, 5.0, 0.0, 0.1);  // already there
    CHECK_NEAR(still.settleTime(), 0.0, 1e-12);
}

TEST(spring_settle_time_envelope_is_tight) {
    // Envelope bound refined by a 1 ms backward scan: within 1 ms of a 0.1 ms brute-force scan.
    for (double z : {0.2, 0.5, 0.86, 1.0, 1.3, 2.5}) {
        for (double v0 : {0.0, 4.0, -6.0}) {
            Spring s({260, z, 1, 0}, 0.0, 1.0, v0, 1e-3);
            double last = 0;
            for (double t = 0; t < 20; t += 1e-4)
                if (std::abs(s.value(t) - 1) >= 1e-3 || std::abs(s.velocity(t)) >= 1e-2) last = t;
            CHECK(s.settleTime() >= last - 1e-3);
            CHECK(s.settleTime() <= last + 2e-3);
        }
    }
}

// ---------- AnimatedValue ----------
TEST(animated_value_retarget_preserves_velocity) {
    AnimatedValue a(0.0);
    SpringParams p{300, 0.85, 1, 0};
    a.springTo(100.0, 1.0, p, 0.25);
    const double tr = 1.08;
    const double vBefore = a.velocity(tr), xBefore = a.value(tr);
    CHECK(vBefore > 100.0);
    a.springTo(0.0, tr, p, 0.25);  // interrupt: reverse direction
    CHECK_NEAR(a.value(tr), xBefore, 1e-9);
    CHECK_NEAR(a.velocity(tr + 1e-9), vBefore, 1e-3);  // no velocity jump
    CHECK(a.value(tr + 0.01) > xBefore);                // keeps moving forward briefly before turning
    CHECK(a.settled(tr + 5.0));
    CHECK_NEAR(a.value(tr + 5.0), 0.0, 1e-12);
}

TEST(animated_value_timed_delay_and_jump) {
    AnimatedValue a(10.0);
    a.easeTo(20.0, 0.0, 0.5, easing::linear, 0.1);
    CHECK_NEAR(a.value(0.05), 10.0, 1e-12);  // delay holds the start value
    CHECK_NEAR(a.value(0.35), 15.0, 1e-6);
    CHECK_NEAR(a.velocity(0.35), 20.0, 1e-3);
    CHECK(!a.settled(0.55));
    CHECK(a.settled(0.6));
    CHECK_NEAR(a.target(), 20.0, 1e-12);

    a.jump(0.4, 3.0);  // gesture release: position and velocity
    CHECK_NEAR(a.value(9.0), 0.4, 1e-12);
    a.springTo(1.0, 9.0, {300, 0.9, 1, 0}, 1e-3);
    CHECK_NEAR(a.velocity(9.0 + 1e-9), 3.0, 1e-3);
}

TEST(animated_value_initial_velocity_param) {
    AnimatedValue a(0.0);
    a.springTo(50.0, 0.0, {300, 0.9, 1, 2.0}, 0.25);  // 2 x distance per second
    CHECK_NEAR(a.velocity(1e-9), 100.0, 1e-3);
}

// ---------- Gesture ----------
TEST(gesture_velocity_and_release) {
    GestureTracker g;
    g.begin(0.0, 0.0);
    for (int i = 1; i <= 48; ++i) g.update(i / 240.0, 2.0 * i / 240.0);  // 2 progress/s at 240 Hz
    CHECK_NEAR(g.velocity(), 2.0, 1e-6);
    auto r = g.release(0.15);
    CHECK_NEAR(r.from, 0.4, 1e-9);
    CHECK_NEAR(r.target, 1.0, 1e-12);  // 0.4 + 2 * 0.15 = 0.7 -> commit

    GestureTracker back;
    back.begin(0.0, 0.7);
    for (int i = 1; i <= 24; ++i) back.update(i / 240.0, 0.7 - 4.0 * i / 240.0);  // flick back
    CHECK(back.release(0.15).target == 0.0);                                   // 0.3 - 0.6 < 0.5

    GestureTracker slow;
    slow.begin(0.0, 0.55);
    slow.update(0.5, 0.55);  // held still
    CHECK_NEAR(slow.velocity(), 0.0, 1e-12);
    CHECK(slow.release().target == 1.0);
}

TEST(gesture_rubber_band) {
    CHECK_NEAR(rubberBand(0.5, 0, 1, 0.15), 0.5, 1e-12);
    double prev = 1.0;
    for (double x = 1.0; x < 5.0; x += 0.01) {
        const double y = rubberBand(x, 0, 1, 0.15);
        CHECK(y >= prev - 1e-12 && y < 1.15);
        prev = y;
    }
    CHECK_NEAR(rubberBand(1.0 + 1e-6, 0, 1, 0.15), 1.0 + 0.55e-6, 1e-9);  // continuous at the edge
    CHECK(rubberBand(-1.0, 0, 1, 0.15) > -0.15);
    GestureTracker g(0.2);
    g.begin(0, 3.0);
    CHECK(g.progress() < 1.2);
}

TEST(parallax_offset) {
    CHECK_NEAR(parallaxOffset(0.5, 1920, 0.25), -240.0, 1e-9);
    CHECK_NEAR(parallaxOffset(1.0, 1920, 1.0), -1920.0, 1e-9);
}

// ---------- VisualMotion (scale + translation + opacity, shared transition) ----------
TEST(visual_motion_keeps_aspect_and_lands) {
    VisualMotion m(800, 600);
    m.jump({100, 100, 800, 600}, 1.0);
    m.springTo({1000, 500, 200, 150}, 0.5, 0.0, {260, 0.6, 1, 0});
    for (double t = 0; t < 2.0; t += 0.01) {
        const Rect r = m.rect(t);
        CHECK_NEAR(r.w / r.h, 800.0 / 600.0, 1e-9);
        CHECK(m.opacity(t) >= 0.0 && m.opacity(t) <= 1.0);
    }
    CHECK(m.settled(5.0));
    const Rect end = m.rect(5.0);
    CHECK_NEAR(end.x, 1000, 1e-9);
    CHECK_NEAR(end.w, 200, 1e-9);
    CHECK_NEAR(m.opacity(5.0), 0.5, 1e-12);
}

// ---------- JSON ----------
TEST(json_parses_values) {
    auto r = json::parse("\xEF\xBB\xBF { \"a\": [1, -2.5e2, true, false, null], \"s\": \"x\\n\\u00e9\\ud83d\\ude00\", \"o\": {} }");
    CHECK(r.value.has_value());
    if (!r.value) return;
    const json::Value* a = r.value->find("a");
    CHECK(a && a->array.size() == 5 && a->array[1].number == -250.0 && a->array[4].type == json::Value::Type::Null);
    CHECK(r.value->find("s")->string == "x\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(r.value->find("o")->object.empty());
}

TEST(json_rejects_invalid_with_position) {
    for (const char* bad : {"{\"a\": 1,}", "{\"a\": 1, \"a\": 2}", "[01]", "[1.]", "\"\\x\"", "\"\\ud800\"", "{a: 1}", "[1] 2", "", "nul",
                            "\"tab\there\"", "[1e999]"}) {
        auto r = json::parse(bad);
        CHECK(!r.value.has_value());
        CHECK(r.error.rfind("line ", 0) == 0);
    }
    auto r = json::parse("{\n  \"a\": tru\n}");
    CHECK(r.error.rfind("line 2, column 8", 0) == 0);
    std::string deep(100, '[');
    CHECK(!json::parse(deep).value.has_value());
}

// ---------- Config loading ----------
static std::string readFile(const char* path) {
    std::string s;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") == 0 && f) {
        char buf[4096];
        for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
        std::fclose(f);
    }
    return s;
}

TEST(config_default_file_matches_defaults) {
    const std::string text = readFile(MC_SOURCE_DIR "/resources/config.default.json");
    CHECK(!text.empty());
    AnimationConfig c;
    CHECK(loadAnimationConfig(text, c).empty());
    const AnimationConfig d;
    CHECK(c.entranceSpring == d.entranceSpring && c.exitSpring == d.exitSpring && c.hoverSpring == d.hoverSpring && c.gestureSpring == d.gestureSpring);
    CHECK(c.dimInEasing == d.dimInEasing && c.dimOutEasing == d.dimOutEasing && c.desktopEasing == d.desktopEasing);
    CHECK(c.staggerStep == d.staggerStep && c.staggerMax == d.staggerMax && c.dimOpacity == d.dimOpacity && c.desktopParallax == d.desktopParallax);
    CHECK(c.dimInDuration == d.dimInDuration && c.dimOutDuration == d.dimOutDuration && c.hoverScale == d.hoverScale);
    CHECK(c.desktopDuration == d.desktopDuration && c.gestureRubberBand == d.gestureRubberBand && c.gestureProjection == d.gestureProjection);
    CHECK(c.minimizedStartScale == d.minimizedStartScale && c.blurEnabled == d.blurEnabled && c.blurIntensity == d.blurIntensity);
    CHECK(c.takeoverWinTab == d.takeoverWinTab);
    AnimationConfig off;
    CHECK(loadAnimationConfig(R"({"integration": {"takeoverWinTab": false}})", off).empty() && !off.takeoverWinTab);
}

TEST(config_partial_override_and_errors) {
    AnimationConfig c;
    CHECK(loadAnimationConfig(R"({"dim": {"opacity": 0.7, "inEasing": [0.3, 0, 0.1, 1]}, "exit": {"spring": {"stiffness": 500}}})", c).empty());
    CHECK(c.dimOpacity == 0.7 && c.exitSpring.stiffness == 500 && c.exitSpring.dampingRatio == 0.92);
    CHECK((c.dimInEasing == CubicBezier{0.3, 0, 0.1, 1}));

    const AnimationConfig before = c;
    auto errs = loadAnimationConfig(R"({"dim": {"opacty": 0.7}, "hover": {"scale": "big"}, "blur": {"enabled": 1}})", c);
    CHECK(errs.size() == 3);  // unknown key, wrong type, wrong type
    CHECK(!errs.empty() && errs[0].find("dim.opacty") != std::string::npos);
    CHECK(c.dimOpacity == before.dimOpacity);  // unchanged on error

    CHECK(loadAnimationConfig(R"({"dim": {"opacity": 3}})", c).size() == 1);  // out of range
    CHECK(c.dimOpacity == before.dimOpacity);
    CHECK(loadAnimationConfig(R"({"dim": {"inEasing": "bouncy"}})", c).size() == 1);
    CHECK(loadAnimationConfig("[1]", c).size() == 1);
    CHECK(loadAnimationConfig("{", c).size() == 1);
    CHECK(loadAnimationConfig(R"({"$schemaVersion": 2})", c).size() == 1);
}

// ---------- Phase 4 layout ----------
TEST(layout_reserves_label_space) {
    LayoutParams p;
    p.labelHeight = 30;
    const Rect area{0, 0, 1920, 1080};
    for (int n : {1, 4, 12, 30}) {
        std::vector<Rect> in;
        for (int i = 0; i < n; ++i) in.push_back({double(i * 41 % 900), double(i * 29 % 600), 800.0 + (i % 3) * 200, 600.0});
        auto out = layoutOverview(area, in, p);
        for (std::size_t i = 0; i < out.size(); ++i) {
            const Rect withLabel{out[i].x, out[i].y, out[i].w, out[i].h + p.labelHeight};
            CHECK(withLabel.bottom() <= area.bottom() - p.padding + 1e-6);
            for (std::size_t j = 0; j < out.size(); ++j)
                if (j != i) CHECK(!withLabel.intersects(out[j]));
        }
    }
}

TEST(strip_layout) {
    StripParams sp;
    const Rect mon{0, 0, 1920, 1080};
    auto s = layoutStrip(mon, 3, 16.0 / 9.0, true, sp);
    CHECK(s.tiles.size() == 3 && !s.plus.empty());
    CHECK(s.band.h >= sp.minHeight && s.band.h <= sp.maxHeight);
    CHECK_NEAR(s.tiles[0].w / s.tiles[0].h, 16.0 / 9.0, 1e-9);
    CHECK_NEAR((s.tiles.front().x + s.plus.right()) / 2, mon.cx(), 1e-6);  // centered
    for (auto& t : s.tiles) CHECK(t.bottom() + sp.nameHeight <= s.band.bottom() + 1e-6);
    auto many = layoutStrip(mon, 40, 16.0 / 9.0, true, sp);  // shrinks to fit
    CHECK(many.tiles.front().x >= mon.x + sp.padding - 1e-6 && many.plus.right() <= mon.right() - sp.padding + 1e-6);
    CHECK(layoutStrip(mon, 2, 16.0 / 9.0, false, sp).plus.empty());
    CHECK(layoutStrip(mon, 0, 16.0 / 9.0, true, sp).tiles.empty());
}

TEST(keyboard_navigation) {
    // 2 x 2 grid plus one card below.
    const std::vector<Rect> r = {{0, 0, 100, 100}, {200, 0, 100, 100}, {0, 200, 100, 100}, {200, 200, 100, 100}, {100, 400, 100, 100}};
    CHECK(navigate(r, -1, NavDir::Right) == 0);
    CHECK(navigate(r, 0, NavDir::Right) == 1);
    CHECK(navigate(r, 1, NavDir::Right) == 1);  // nothing further right
    CHECK(navigate(r, 0, NavDir::Down) == 2);
    CHECK(navigate(r, 3, NavDir::Left) == 2);
    CHECK(navigate(r, 3, NavDir::Down) == 4);
    CHECK(navigate(r, 4, NavDir::Up) == 2 || navigate(r, 4, NavDir::Up) == 3);
    CHECK(navigate({}, -1, NavDir::Left) == -1);
}

// ---------- Win+Tab detector (Phase 5) ----------
TEST(wintab_detector) {
    using A = WinTabDetector::Action;
    constexpr std::uint32_t Tab = 0x09, LWin = 0x5B, RWin = 0x5C, LCtrl = 0xA2, LAlt = 0xA4, Shift = 0xA0, A_ = 0x41;
    WinTabDetector d;
    CHECK(d.onKey({Tab, true}) == A::Pass);  // plain Tab
    CHECK(d.onKey({Tab, false}) == A::Pass);
    CHECK(d.onKey({LWin, true}) == A::Pass);
    CHECK(d.onKey({Tab, true}) == A::Trigger);   // Win+Tab
    CHECK(d.onKey({Tab, true}) == A::Swallow);   // auto-repeat: no second trigger
    CHECK(d.onKey({Tab, false}) == A::Swallow);  // its key-up too
    CHECK(d.onKey({Tab, true}) == A::Trigger);   // pressed again while Win still held
    CHECK(d.onKey({Tab, false}) == A::Swallow);
    CHECK(d.onKey({LWin, false}) == A::Pass);    // Win itself is never swallowed
    CHECK(d.onKey({Tab, true}) == A::Pass);
    CHECK(d.onKey({Tab, false}) == A::Pass);

    // Right Win and Win+Shift+Tab trigger; Ctrl or Alt keep Windows' meaning.
    CHECK(d.onKey({RWin, true}) == A::Pass);
    CHECK(d.onKey({Shift, true}) == A::Pass);
    CHECK(d.onKey({Tab, true}) == A::Trigger);
    CHECK(d.onKey({Tab, false}) == A::Swallow);
    CHECK(d.onKey({LCtrl, true}) == A::Pass);
    CHECK(d.onKey({Tab, true}) == A::Pass);
    CHECK(d.onKey({Tab, false}) == A::Pass);
    CHECK(d.onKey({LCtrl, false}) == A::Pass);
    CHECK(d.onKey({LAlt, true}) == A::Pass);
    CHECK(d.onKey({Tab, true}) == A::Pass);
    CHECK(d.onKey({LAlt, false}) == A::Pass);
    CHECK(d.onKey({A_, true}) == A::Pass);  // other keys untouched

    // Synthetic input is ignored by default (no loops with our own injected keys).
    WinTabDetector real;
    CHECK(real.onKey({LWin, true, true}) == A::Pass);
    CHECK(real.onKey({Tab, true, true}) == A::Pass);
    WinTabDetector test(true);
    CHECK(test.onKey({LWin, true, true}) == A::Pass);
    CHECK(test.onKey({Tab, true, true}) == A::Trigger);
}

// ---------- Autostart (StartupApproved parsing) ----------
TEST(autostart_startup_approved) {
    const std::uint8_t enabled[] = {0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::uint8_t enabled6[] = {0x06, 0, 0, 0};
    const std::uint8_t disabled[] = {0x03, 0, 0, 0, 0x10, 0x20, 0x30, 0x40, 0, 0, 0, 0};
    CHECK(autostart::startupApproved(enabled, sizeof enabled));
    CHECK(autostart::startupApproved(enabled6, sizeof enabled6));
    CHECK(!autostart::startupApproved(disabled, sizeof disabled));
    CHECK(autostart::startupApproved(nullptr, 0));  // no entry: Windows treats it as enabled
}

// ---------- Virtual desktop state ----------
TEST(desktop_ids_parse_and_lookup) {
    std::uint8_t raw[32];
    for (int i = 0; i < 32; ++i) raw[i] = std::uint8_t(i);
    auto ids = parseDesktopIds(raw, 32);
    CHECK(ids && ids->size() == 2 && (*ids)[1].bytes[0] == 16 && (*ids)[1].bytes[15] == 31);
    CHECK(parseDesktopIds(raw, 0).has_value() && parseDesktopIds(raw, 0)->empty());
    CHECK(!parseDesktopIds(raw, 17).has_value());
    CHECK(!parseDesktopIds(nullptr, 16).has_value());

    DesktopState s;
    s.desktops = {{(*ids)[0], L""}, {(*ids)[1], L"Work"}};
    s.current = 1;
    CHECK(s.indexOf((*ids)[1]) == 1);
    CHECK(s.indexOf(DesktopId{}) == -1);
    CHECK(s.displayName(0) == L"Desktop 1");
    CHECK(s.displayName(1) == L"Work");
    CHECK(s.displayName(5).empty());
    CHECK(DesktopId{}.isNull() && !(*ids)[1].isNull());
}

TEST(desktop_switch_steps) {
    CHECK(switchSteps(0, 3) == 3);
    CHECK(switchSteps(3, 1) == -2);
    CHECK(switchSteps(2, 2) == 0);
    CHECK(switchSteps(-1, 2) == 0);  // unknown current: do nothing rather than guess
}

int main() {
    for (auto& c : mctest::registry()) {
        const int before = mctest::failures();
        c.fn();
        std::printf("%s %s\n", mctest::failures() == before ? "[ OK ]" : "[FAIL]", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", mctest::registry().size(), mctest::failures());
    return mctest::failures() == 0 ? 0 : 1;
}
