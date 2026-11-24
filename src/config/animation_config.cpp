#include "config/animation_config.h"

#include <cmath>
#include <initializer_list>

#include "config/json.h"

namespace mc {

namespace {

void checkRange(std::vector<std::string>& errs, const std::string& name, double v, double lo, double hi) {
    if (!std::isfinite(v) || v < lo || v > hi)
        errs.push_back(name + " must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "], got " + std::to_string(v));
}
void checkSpring(std::vector<std::string>& errs, const std::string& name, const anim::SpringParams& s) {
    checkRange(errs, name + ".stiffness", s.stiffness, 1.0, 5000.0);
    checkRange(errs, name + ".dampingRatio", s.dampingRatio, 0.05, 5.0);
    checkRange(errs, name + ".mass", s.mass, 0.01, 100.0);
    checkRange(errs, name + ".initialVelocity", s.initialVelocity, -100.0, 100.0);
}
void checkEasing(std::vector<std::string>& errs, const std::string& name, const anim::CubicBezier& b) {
    // x control points must stay in [0,1] or the curve is not a function of time.
    checkRange(errs, name + "[0]", b.x1, 0.0, 1.0);
    checkRange(errs, name + "[1]", b.y1, -2.0, 3.0);
    checkRange(errs, name + "[2]", b.x2, 0.0, 1.0);
    checkRange(errs, name + "[3]", b.y2, -2.0, 3.0);
}

// Reads one JSON object into config fields, collecting errors with the full key path.
class Reader {
public:
    explicit Reader(std::vector<std::string>& errs) : errs_(errs) {}

    // Checks `v` is an object and only has keys from `allowed`.
    bool object(const json::Value* v, const std::string& path, std::initializer_list<const char*> allowed) {
        if (!v) return false;
        if (v->type != json::Value::Type::Object) { errs_.push_back(path + " must be an object"); return false; }
        for (auto& [k, _] : v->object) {
            bool ok = false;
            for (const char* a : allowed) ok = ok || k == a;
            if (!ok) errs_.push_back("unknown key " + join(path, k));
        }
        return true;
    }
    void number(const json::Value& obj, const std::string& path, const char* key, double& out) {
        if (const json::Value* v = obj.find(key)) {
            if (v->type == json::Value::Type::Number) out = v->number;
            else errs_.push_back(join(path, key) + " must be a number");
        }
    }
    void boolean(const json::Value& obj, const std::string& path, const char* key, bool& out) {
        if (const json::Value* v = obj.find(key)) {
            if (v->type == json::Value::Type::Bool) out = v->boolean;
            else errs_.push_back(join(path, key) + " must be true or false");
        }
    }
    void text(const json::Value& obj, const std::string& path, const char* key, std::string& out) {
        if (const json::Value* v = obj.find(key)) {
            if (v->type == json::Value::Type::String) out = v->string;
            else errs_.push_back(join(path, key) + " must be a string");
        }
    }
    void spring(const json::Value& obj, const std::string& path, const char* key, anim::SpringParams& out) {
        const json::Value* v = obj.find(key);
        const std::string p = join(path, key);
        if (!object(v, p, {"stiffness", "dampingRatio", "mass", "initialVelocity"})) return;
        number(*v, p, "stiffness", out.stiffness);
        number(*v, p, "dampingRatio", out.dampingRatio);
        number(*v, p, "mass", out.mass);
        number(*v, p, "initialVelocity", out.initialVelocity);
    }
    // A preset name or [x1, y1, x2, y2].
    void easing(const json::Value& obj, const std::string& path, const char* key, anim::CubicBezier& out) {
        const json::Value* v = obj.find(key);
        if (!v) return;
        const std::string p = join(path, key);
        if (v->type == json::Value::Type::String) {
            static const std::pair<const char*, anim::CubicBezier> presets[] = {
                {"linear", anim::easing::linear}, {"easeIn", anim::easing::easeIn}, {"easeOut", anim::easing::easeOut},
                {"easeInOut", anim::easing::easeInOut}, {"emphasized", anim::easing::emphasized}};
            for (auto& [name, bez] : presets)
                if (v->string == name) { out = bez; return; }
            errs_.push_back(p + ": unknown easing \"" + v->string + "\" (linear, easeIn, easeOut, easeInOut, emphasized, or [x1,y1,x2,y2])");
            return;
        }
        if (v->type == json::Value::Type::Array && v->array.size() == 4) {
            double c[4];
            for (int i = 0; i < 4; ++i) {
                if (v->array[i].type != json::Value::Type::Number) { errs_.push_back(p + " must contain 4 numbers"); return; }
                c[i] = v->array[i].number;
            }
            out = {c[0], c[1], c[2], c[3]};
            return;
        }
        errs_.push_back(p + " must be an easing name or [x1, y1, x2, y2]");
    }

private:
    static std::string join(const std::string& path, const std::string& key) { return path.empty() ? key : path + "." + key; }
    std::vector<std::string>& errs_;
};

}  // namespace

std::vector<std::string> validate(const AnimationConfig& c) {
    std::vector<std::string> errs;
    checkSpring(errs, "entrance.spring", c.entranceSpring);
    checkRange(errs, "entrance.staggerStep", c.staggerStep, 0.0, 0.2);
    checkRange(errs, "entrance.staggerMax", c.staggerMax, 0.0, 1.0);
    checkRange(errs, "entrance.minimizedStartScale", c.minimizedStartScale, 0.1, 1.0);
    checkSpring(errs, "exit.spring", c.exitSpring);
    checkRange(errs, "dim.opacity", c.dimOpacity, 0.0, 1.0);
    checkRange(errs, "dim.inDuration", c.dimInDuration, 0.0, 2.0);
    checkRange(errs, "dim.outDuration", c.dimOutDuration, 0.0, 2.0);
    checkEasing(errs, "dim.inEasing", c.dimInEasing);
    checkEasing(errs, "dim.outEasing", c.dimOutEasing);
    checkRange(errs, "hover.scale", c.hoverScale, 0.5, 2.0);
    checkSpring(errs, "hover.spring", c.hoverSpring);
    checkRange(errs, "trafficLights.diameter", c.trafficDiameter, 6.0, 24.0);
    checkRange(errs, "trafficLights.spacing", c.trafficSpacing, 8.0, 40.0);
    if (c.trafficSpacing < c.trafficDiameter + 2) errs.push_back("trafficLights.spacing must exceed the diameter");
    checkRange(errs, "desktopTransition.duration", c.desktopDuration, 0.0, 3.0);
    checkEasing(errs, "desktopTransition.easing", c.desktopEasing);
    checkRange(errs, "desktopTransition.parallax", c.desktopParallax, 0.0, 1.0);
    checkRange(errs, "gesture.rubberBand", c.gestureRubberBand, 0.0, 1.0);
    checkRange(errs, "gesture.projection", c.gestureProjection, 0.0, 1.0);
    checkSpring(errs, "gesture.spring", c.gestureSpring);
    checkRange(errs, "blur.intensity", c.blurIntensity, 0.0, 1.0);
    return errs;
}

std::vector<std::string> loadAnimationConfig(std::string_view jsonText, AnimationConfig& out) {
    auto parsed = json::parse(jsonText);
    if (!parsed.value) return {"config is not valid JSON: " + parsed.error};

    std::vector<std::string> errs;
    AnimationConfig c = out;
    Reader r(errs);
    const json::Value& root = *parsed.value;
    if (!r.object(&root, "", {"$schemaVersion", "entrance", "exit", "dim", "hover", "trafficLights", "globalTrafficLights", "desktopTransition", "gesture", "blur", "integration"}))
        return errs;
    if (const json::Value* ver = root.find("$schemaVersion"); ver && !(ver->type == json::Value::Type::Number && ver->number == 1.0))
        errs.push_back("$schemaVersion must be 1");

    if (auto* v = root.find("entrance"); r.object(v, "entrance", {"spring", "staggerStep", "staggerMax", "minimizedStartScale"})) {
        r.spring(*v, "entrance", "spring", c.entranceSpring);
        r.number(*v, "entrance", "staggerStep", c.staggerStep);
        r.number(*v, "entrance", "staggerMax", c.staggerMax);
        r.number(*v, "entrance", "minimizedStartScale", c.minimizedStartScale);
    }
    if (auto* v = root.find("exit"); r.object(v, "exit", {"spring"})) r.spring(*v, "exit", "spring", c.exitSpring);
    if (auto* v = root.find("dim"); r.object(v, "dim", {"opacity", "inDuration", "outDuration", "inEasing", "outEasing"})) {
        r.number(*v, "dim", "opacity", c.dimOpacity);
        r.number(*v, "dim", "inDuration", c.dimInDuration);
        r.number(*v, "dim", "outDuration", c.dimOutDuration);
        r.easing(*v, "dim", "inEasing", c.dimInEasing);
        r.easing(*v, "dim", "outEasing", c.dimOutEasing);
    }
    if (auto* v = root.find("hover"); r.object(v, "hover", {"scale", "spring"})) {
        r.number(*v, "hover", "scale", c.hoverScale);
        r.spring(*v, "hover", "spring", c.hoverSpring);
    }
    if (auto* v = root.find("trafficLights"); r.object(v, "trafficLights", {"enabled", "diameter", "spacing"})) {
        r.boolean(*v, "trafficLights", "enabled", c.trafficLights);
        r.number(*v, "trafficLights", "diameter", c.trafficDiameter);
        r.number(*v, "trafficLights", "spacing", c.trafficSpacing);
    }
    if (auto* g = root.find("globalTrafficLights"); r.object(g, "globalTrafficLights", {"experimentalOverlay"})) {
        if (auto* v = g->find("experimentalOverlay"); r.object(v, "globalTrafficLights.experimentalOverlay", {"enabled", "titleContains"})) {
            r.boolean(*v, "globalTrafficLights.experimentalOverlay", "enabled", c.edgePocEnabled);
            r.text(*v, "globalTrafficLights.experimentalOverlay", "titleContains", c.edgePocTitle);
        }
    }
    if (auto* v = root.find("desktopTransition"); r.object(v, "desktopTransition", {"duration", "easing", "parallax"})) {
        r.number(*v, "desktopTransition", "duration", c.desktopDuration);
        r.easing(*v, "desktopTransition", "easing", c.desktopEasing);
        r.number(*v, "desktopTransition", "parallax", c.desktopParallax);
    }
    if (auto* v = root.find("gesture"); r.object(v, "gesture", {"rubberBand", "projection", "spring"})) {
        r.number(*v, "gesture", "rubberBand", c.gestureRubberBand);
        r.number(*v, "gesture", "projection", c.gestureProjection);
        r.spring(*v, "gesture", "spring", c.gestureSpring);
    }
    if (auto* v = root.find("blur"); r.object(v, "blur", {"enabled", "intensity"})) {
        r.boolean(*v, "blur", "enabled", c.blurEnabled);
        r.number(*v, "blur", "intensity", c.blurIntensity);
    }
    if (auto* v = root.find("integration"); r.object(v, "integration", {"takeoverWinTab"}))
        r.boolean(*v, "integration", "takeoverWinTab", c.takeoverWinTab);

    if (errs.empty()) errs = validate(c);
    if (errs.empty()) out = c;
    return errs;
}

}  // namespace mc
