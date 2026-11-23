#pragma once
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "animation/animation.h"
#include "animation/visual_motion.h"
#include "config/animation_config.h"
#include "core/geometry.h"
#include "preview/thumbnail.h"
#include "rendering/scene_renderer.h"

namespace mc {

// One window card as planned by the UI thread. Rects are in overlay client coordinates.
struct CardTarget {
    HWND hwnd = nullptr;
    RECT sourceInWindow{};
    Rect real;      // where the window really is (restored rect when minimized)
    Rect overview;  // slot in the overview
    Rect monitor;   // work area of the card's monitor (desktop slides move by its width)
    bool minimized = false;
    std::wstring title;
    HICON icon = nullptr;  // owned by the window's process; may become invalid (then no icon)
    float dpiScale = 1.0f; // of the card's monitor
    float labelHeight = 0; // physical px reserved under the card
};

enum class TransitionKind : int { None = 0, Enter = 1, Exit = 2, Relayout = 3, Interaction = 4 };

struct FrameSample {
    int kind;           // TransitionKind
    int cards;
    double tMs;         // time since transition start
    double intervalMs;  // since previous frame of the same transition (0 for the first)
    double workMs;      // CPU time spent evaluating + applying this frame
    double latencyMs;   // first frame only: from the UI thread's command to this frame being applied
};

// Owns every DWM thumbnail, the D2D/DComp scene and the frame loop. Idle = blocked on an event.
class CompositorThread {
public:
    static constexpr UINT kMsgEnterDone = WM_APP + 1;  // posted to `notify`
    static constexpr UINT kMsgExitDone = WM_APP + 2;   // wParam = selected HWND (may be null)
    static constexpr UINT kMsgReady = WM_APP + 3;      // devices created, thread idle

    ~CompositorThread() { stop(); }
    void start(HWND overlay, int width, int height, const AnimationConfig& cfg, HWND notify);
    void stop();

    // Builds the wallpaper backdrops ahead of time so the first open does not decode images.
    void prepare(std::vector<BackdropSpec> backdrops);
    void enter(std::vector<CardTarget> cards, std::vector<BackdropSpec> backdrops, StripContent strip);
    // fromGesture: continue a scrub with the gesture spring instead of the exit spring.
    void exit(HWND selected, bool fromGesture = false);
    // slideDir != 0: a desktop switch. Cards that left slide out toward -slideDir and fade; new cards
    // slide in from +slideDir, the backdrop crossfades with parallax (desktopTransition config).
    void relayout(std::vector<CardTarget> cards, int slideDir = 0);
    void setStrip(StripContent strip);
    void focus(HWND card);  // hover / keyboard selection; null clears
    // Gesture: p = 0 overview, 1 windows back in place; velocity in p per second.
    void scrub(double p, double velocity);
    void cancelScrub();     // spring back to the overview

    std::vector<FrameSample> takeSamples();

private:
    enum class Cmd { Prepare, Enter, Exit, Relayout, Strip, Focus, Scrub, CancelScrub };
    struct Command {
        Cmd cmd;
        double postedAt = 0;
        std::vector<CardTarget> cards;
        std::vector<BackdropSpec> backdrops;
        StripContent strip;
        HWND hwnd = nullptr;  // Exit: selected; Focus: card
        int slideDir = 0;
        bool flag = false;    // Exit: from gesture
        double p = 0, v = 0;  // Scrub
    };
    struct Card {
        CardTarget target;
        Thumbnail thumb;
        anim::VisualMotion motion;
        SceneRenderer::Label label;
        bool leaving = false;  // sliding out after a desktop switch; dropped once settled
    };

    void post(Command c);
    void run();
    void handle(Command& cmd, double now);
    void applyFrame(double now);
    void dropCard(Card& c);
    Rect restRect(const Card& c) const;  // overview slot, enlarged when focused
    void retargetFocus(double now);
    double nowSec() const;

    HWND overlay_ = nullptr, notify_ = nullptr;
    int width_ = 0, height_ = 0;
    AnimationConfig cfg_;
    std::thread thread_;
    HANDLE wake_ = nullptr;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::deque<Command> queue_;
    std::vector<FrameSample> samples_;  // guarded by mutex_

    // Compositor-thread state.
    SceneRenderer scene_;
    std::vector<Card> cards_;
    TransitionKind kind_ = TransitionKind::None;
    bool enterPending_ = false;  // an Enter is in flight; a Relayout must not swallow its EnterDone
    HWND selected_ = nullptr, focus_ = nullptr;
    double start_ = 0, lastFrame_ = 0, posted_ = 0;
    anim::AnimatedValue presence_;    // backdrop, strip and labels: 0 hidden, 1 shown
    anim::AnimatedValue focusAlpha_;  // focus ring + shadow
    anim::AnimatedValue deskQ_{1.0};  // desktop slide progress, 0 -> 1
    int deskDir_ = 0;
    anim::AnimatedValue stripRingX_;
    StripContent strip_;
    Rect lastFocusRect_;
    LARGE_INTEGER qpcFreq_{};
};

}  // namespace mc
