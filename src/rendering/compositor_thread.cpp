#include "rendering/compositor_thread.h"

#include <dcomp.h>
#include <dwmapi.h>
#include <objbase.h>

#include <algorithm>

#include "diagnostics/log.h"

namespace mc {

namespace {
constexpr int kLabelsPerFrame = 1;            // spread DirectWrite work over the first frames of a transition
constexpr double kPositionEps = 0.5;          // px
constexpr double kAlphaEps = 1.0 / 512.0;
}  // namespace

void CompositorThread::start(HWND overlay, int width, int height, const AnimationConfig& cfg, HWND notify) {
    stop();
    overlay_ = overlay;
    notify_ = notify;
    width_ = width;
    height_ = height;
    cfg_ = cfg;
    QueryPerformanceFrequency(&qpcFreq_);
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) { log::lastError("CreateEvent(compositor wake)"); return; }
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void CompositorThread::stop() {
    if (!thread_.joinable()) return;
    running_ = false;
    SetEvent(wake_);
    thread_.join();
    CloseHandle(wake_);
    wake_ = nullptr;
}

void CompositorThread::post(Command c) {
    c.postedAt = nowSec();
    { std::lock_guard l(mutex_); queue_.push_back(std::move(c)); }
    SetEvent(wake_);
}

void CompositorThread::prepare(std::vector<BackdropSpec> backdrops) {
    Command c{Cmd::Prepare};
    c.backdrops = std::move(backdrops);
    post(std::move(c));
}
void CompositorThread::enter(std::vector<CardTarget> cards, std::vector<BackdropSpec> backdrops, StripContent strip) {
    Command c{Cmd::Enter};
    c.cards = std::move(cards);
    c.backdrops = std::move(backdrops);
    c.strip = std::move(strip);
    post(std::move(c));
}
void CompositorThread::exit(HWND selected, bool fromGesture) {
    Command c{Cmd::Exit};
    c.hwnd = selected;
    c.flag = fromGesture;
    post(std::move(c));
}
void CompositorThread::relayout(std::vector<CardTarget> cards, int slideDir) {
    Command c{Cmd::Relayout};
    c.cards = std::move(cards);
    c.slideDir = slideDir;
    post(std::move(c));
}
void CompositorThread::setStrip(StripContent strip) {
    Command c{Cmd::Strip};
    c.strip = std::move(strip);
    post(std::move(c));
}
void CompositorThread::focus(HWND card) {
    Command c{Cmd::Focus};
    c.hwnd = card;
    post(std::move(c));
}
void CompositorThread::scrub(double p, double velocity) {
    Command c{Cmd::Scrub};
    c.p = p;
    c.v = velocity;
    post(std::move(c));
}
void CompositorThread::cancelScrub() { post(Command{Cmd::CancelScrub}); }

std::vector<FrameSample> CompositorThread::takeSamples() {
    std::lock_guard l(mutex_);
    return std::exchange(samples_, {});
}

double CompositorThread::nowSec() const {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(qpcFreq_.QuadPart);
}

void CompositorThread::run() {
    log::write("compositor thread started");
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC
    if (!scene_.init(overlay_, width_, height_)) log::write("scene renderer unavailable: overview runs without backdrop, labels and strip");
    PostMessageW(notify_, kMsgReady, 0, 0);

    // Windows 11 API; resolved at runtime so the binary still loads where it is missing.
    using WaitClockFn = DWORD(WINAPI*)(UINT, const HANDLE*, DWORD);
    auto waitClock = reinterpret_cast<WaitClockFn>(GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "DCompositionWaitForCompositorClock"));
    if (!waitClock) log::write("DCompositionWaitForCompositorClock unavailable, pacing with DwmFlush");

    while (running_) {
        if (kind_ == TransitionKind::None) {
            WaitForSingleObject(wake_, INFINITE);  // idle: no CPU until a command arrives
        } else if (waitClock) {
            if (waitClock(1, &wake_, INFINITE) == WAIT_FAILED) log::lastError("DCompositionWaitForCompositorClock");
        } else {
            DwmFlush();
        }
        if (!running_) break;

        std::deque<Command> cmds;
        { std::lock_guard l(mutex_); cmds.swap(queue_); }
        for (auto& c : cmds) handle(c, nowSec());
        if (kind_ != TransitionKind::None) applyFrame(nowSec());
    }

    for (auto& c : cards_) dropCard(c);
    cards_.clear();  // unregisters thumbnails on the owning thread
    scene_.reset();
    if (SUCCEEDED(co)) CoUninitialize();
    log::write("compositor thread stopped");
}

void CompositorThread::dropCard(Card& c) {
    scene_.destroyLabel(c.label);
    c.thumb.reset();
}

Rect CompositorThread::restRect(const Card& c) const {
    return c.target.hwnd == focus_ && focus_ ? scaled(c.target.overview, cfg_.hoverScale) : c.target.overview;
}

void CompositorThread::retargetFocus(double now) {
    for (auto& c : cards_)
        if (!c.leaving) c.motion.springTo(restRect(c), 1.0, now, cfg_.hoverSpring);
}

void CompositorThread::handle(Command& cmd, double now) {
    const BackdropStyle style{cfg_.dimOpacity, cfg_.blurEnabled, cfg_.blurIntensity, cfg_.desktopParallax};
    TransitionKind next = kind_ == TransitionKind::None ? TransitionKind::Interaction : kind_;

    switch (cmd.cmd) {
    case Cmd::Prepare:
        scene_.setBackdrops(cmd.backdrops, style);
        scene_.commit();
        return;  // no animation

    case Cmd::Strip:
        strip_ = cmd.strip;
        scene_.setStrip(strip_);
        stripRingX_.springTo(scene_.stripTileX(strip_.current), now, cfg_.hoverSpring, kPositionEps);
        break;

    case Cmd::Focus: {
        focus_ = cmd.hwnd;
        auto it = std::find_if(cards_.begin(), cards_.end(), [&](const Card& c) { return c.target.hwnd == focus_ && !c.leaving; });
        if (it == cards_.end()) focus_ = nullptr;
        else scene_.setFocusChrome(it->target.overview, it->target.dpiScale);
        focusAlpha_.springTo(focus_ ? 1.0 : 0.0, now, cfg_.hoverSpring, kAlphaEps);
        retargetFocus(now);
        break;
    }

    case Cmd::Scrub:
        for (auto& c : cards_) {
            if (c.leaving) continue;
            const bool toReal = !c.target.minimized;
            const Rect end = toReal ? c.target.real : scaled(c.target.overview, cfg_.minimizedStartScale);
            c.motion.scrub(c.target.overview, end, cmd.p, cmd.v, toReal ? 1.0 : std::clamp(1.0 - cmd.p, 0.0, 1.0));
        }
        presence_.jump(std::clamp(1.0 - cmd.p, 0.0, 1.0), -cmd.v);
        focusAlpha_.jump(0.0);
        focus_ = nullptr;
        break;

    case Cmd::CancelScrub:
        for (auto& c : cards_)
            if (!c.leaving) c.motion.springTo(restRect(c), 1.0, now, cfg_.gestureSpring);
        presence_.springTo(1.0, now, cfg_.gestureSpring, kAlphaEps);
        break;

    case Cmd::Exit: {
        selected_ = cmd.hwnd;
        focus_ = nullptr;
        const anim::SpringParams& spring = cmd.flag ? cfg_.gestureSpring : cfg_.exitSpring;
        for (auto& c : cards_)
            if (c.leaving) dropCard(c);
        std::erase_if(cards_, [](const Card& c) { return c.leaving; });  // other desktop's cards: gone at once
        for (auto& c : cards_) {
            const bool toReal = c.target.hwnd == selected_ || !c.target.minimized;
            if (toReal) c.motion.springTo(c.target.real, 1.0, now, spring);
            else c.motion.springTo(scaled(c.motion.rect(now), cfg_.minimizedStartScale), 0.0, now, spring);
        }
        // DWM draws thumbnails in registration order: re-register the selected one so it lands on top.
        for (auto& c : cards_) {
            if (c.target.hwnd != selected_ || &c == &cards_.back()) continue;
            Thumbnail fresh;
            if (fresh.create(overlay_, c.target.hwnd, c.target.sourceInWindow)) {
                fresh.update(c.motion.rect(now), c.motion.opacity(now));
                c.thumb = std::move(fresh);  // old handle released after the new one is visible
            }
        }
        presence_.easeTo(0.0, now, cfg_.dimOutDuration, cfg_.dimOutEasing);
        focusAlpha_.easeTo(0.0, now, cfg_.dimOutDuration, cfg_.dimOutEasing);
        enterPending_ = false;
        next = TransitionKind::Exit;
        break;
    }

    case Cmd::Enter:
    case Cmd::Relayout: {
        const bool entering = cmd.cmd == Cmd::Enter;
        if (entering) {
            scene_.setBackdrops(cmd.backdrops, style);
            strip_ = cmd.strip;
            scene_.setStrip(strip_);
            stripRingX_.jump(scene_.stripTileX(strip_.current));
            focus_ = nullptr;
            focusAlpha_.jump(0.0);
            deskQ_.jump(1.0);
            deskDir_ = 0;
        }
        const int dir = entering ? 0 : cmd.slideDir;
        if (dir != 0) {
            deskDir_ = dir;
            deskQ_.jump(0.0);
            deskQ_.easeTo(1.0, now, cfg_.desktopDuration, cfg_.desktopEasing);
        }
        auto shifted = [](Rect r, double dx) { r.x += dx; return r; };
        std::vector<Card> fresh(cmd.cards.size());
        std::vector<bool> moved(cards_.size(), false);  // explicit: a card whose thumbnail died must still be dropped
        for (std::size_t i = 0; i < cmd.cards.size(); ++i) {
            Card& c = fresh[i];
            c.target = cmd.cards[i];
            auto old = std::find_if(cards_.begin(), cards_.end(), [&](const Card& o) { return o.target.hwnd == c.target.hwnd; });
            if (old != cards_.end() && old->thumb) {
                moved[std::size_t(old - cards_.begin())] = true;
                c.thumb = std::move(old->thumb);
                c.motion = old->motion;  // continue from the current position and velocity
                if (std::abs(old->target.overview.w - c.target.overview.w) < 1.0) c.label = std::move(old->label);
                else scene_.destroyLabel(old->label);  // slot width changed: re-rasterize lazily
                c.motion.springTo(restRect(c), 1.0, now, entering ? cfg_.entranceSpring : cfg_.exitSpring);
            } else if (dir != 0) {
                // Arrives with the new desktop: slides in from the side it is coming from.
                c.motion = anim::VisualMotion(c.target.real.w, c.target.real.h);
                c.motion.jump(shifted(c.target.overview, dir * c.target.monitor.w), 1.0);
                c.motion.easeTo(c.target.overview, 1.0, now, cfg_.desktopDuration, cfg_.desktopEasing);
            } else {
                c.motion = anim::VisualMotion(c.target.real.w, c.target.real.h);
                if (entering && !c.target.minimized) c.motion.jump(c.target.real, 1.0);
                else c.motion.jump(scaled(c.target.overview, cfg_.minimizedStartScale), 0.0);
                const double delay = entering ? anim::staggerDelay(i, cmd.cards.size(), cfg_.staggerStep, cfg_.staggerMax) : 0.0;
                c.motion.springTo(c.target.overview, 1.0, now, entering ? cfg_.entranceSpring : cfg_.exitSpring, delay);
            }
        }
        for (std::size_t k = 0; k < cards_.size(); ++k) {
            Card& o = cards_[k];
            if (moved[k]) continue;
            if (dir != 0 && o.thumb) {
                // Cards of the desktop being left slide out the other way and fade, then are dropped.
                if (!o.leaving) {
                    o.leaving = true;
                    o.motion.easeTo(shifted(o.motion.rect(now), -dir * o.target.monitor.w), 0.0, now, cfg_.desktopDuration, cfg_.desktopEasing);
                }
                fresh.push_back(std::move(o));
            } else {
                dropCard(o);
            }
        }
        cards_ = std::move(fresh);
        if (focus_ && std::none_of(cards_.begin(), cards_.end(), [&](const Card& c) { return c.target.hwnd == focus_ && !c.leaving; })) {
            focus_ = nullptr;
            focusAlpha_.springTo(0.0, now, cfg_.hoverSpring, kAlphaEps);
        }
        // Register new thumbnails bottom-most first so the top window is drawn last (on top).
        for (auto it = cards_.rbegin(); it != cards_.rend(); ++it) {
            if (!it->thumb && !it->thumb.create(overlay_, it->target.hwnd, it->target.sourceInWindow))
                log::write("card skipped: no thumbnail for %p", static_cast<void*>(it->target.hwnd));
        }
        if (entering) {
            presence_.easeTo(1.0, now, cfg_.dimInDuration, cfg_.dimInEasing);
            enterPending_ = true;
            next = TransitionKind::Enter;
        } else if (next != TransitionKind::Enter && next != TransitionKind::Exit) {
            next = TransitionKind::Relayout;
        }
        break;
    }
    }

    if (kind_ == TransitionKind::None || next == TransitionKind::Enter || next == TransitionKind::Exit || next == TransitionKind::Relayout) {
        start_ = now;
        posted_ = cmd.postedAt;
        lastFrame_ = -1.0;
    }
    kind_ = next;
}

void CompositorThread::applyFrame(double now) {
    const double t = now - start_;
    const double presence = std::clamp(presence_.value(now), 0.0, 1.0);
    bool done = presence_.settled(now) && focusAlpha_.settled(now) && deskQ_.settled(now) && stripRingX_.settled(now);

    int labelBudget = kLabelsPerFrame;
    for (auto& c : cards_) {
        if (!c.leaving && !c.label.visual && labelBudget > 0 && c.target.labelHeight > 0) {
            const float gap = std::round(6.f * c.target.dpiScale);
            c.label = scene_.createLabel(c.target.title, c.target.icon, float(c.target.overview.w), c.target.labelHeight - gap, c.target.dpiScale);
            --labelBudget;
        }
        if (!c.leaving && !c.label.visual && c.target.labelHeight > 0) done = false;  // still owed a label

        const Rect r = c.motion.rect(now);
        const double opacity = c.motion.opacity(now);
        if (c.thumb && !c.thumb.update(r, opacity)) {
            log::write("thumbnail update failed for %p, releasing", static_cast<void*>(c.target.hwnd));
            c.thumb.reset();
        }
        scene_.placeLabel(c.label, r, opacity * presence, std::round(6.f * c.target.dpiScale));
        if (c.target.hwnd == focus_ && !c.leaving) {
            lastFocusRect_ = r;
            scene_.placeFocus(r, focusAlpha_.value(now) * presence * opacity);
        }
        done = done && c.motion.settled(now);
    }
    if (!focus_ && !lastFocusRect_.empty()) scene_.placeFocus(lastFocusRect_, focusAlpha_.value(now) * presence);  // fade out in place
    for (auto& c : cards_)
        if (c.leaving && c.motion.settled(now)) dropCard(c);
    std::erase_if(cards_, [&](const Card& c) { return c.leaving && !c.thumb && !c.label.visual; });

    if (deskQ_.settled(now)) deskDir_ = 0;
    scene_.placeBackdrop(presence, deskQ_.value(now), deskDir_);
    scene_.placeStrip(presence, stripRingX_.value(now));
    if (scene_.ready() && !scene_.commit()) {
        log::write("scene device lost; rebuilding");
        for (auto& c : cards_) c.label = {};  // visuals died with the device
        if (!scene_.init(overlay_, width_, height_)) log::write("scene rebuild failed: continuing without chrome");
    }
    const double work = nowSec() - now;

    {
        std::lock_guard l(mutex_);
        const bool first = lastFrame_ < 0;
        samples_.push_back({int(kind_), int(cards_.size()), t * 1000.0, first ? 0.0 : (now - lastFrame_) * 1000.0, work * 1000.0,
                            first ? (now + work - posted_) * 1000.0 : 0.0});
    }
    lastFrame_ = now;

    if (!done) return;
    if (kind_ != TransitionKind::Interaction)
        log::write("transition %d done after %.0f ms, presence=%.2f, cards=%zu", int(kind_), t * 1000.0, presence, cards_.size());
    const TransitionKind finished = kind_;
    kind_ = TransitionKind::None;
    if (finished == TransitionKind::Exit) {
        for (auto& c : cards_) dropCard(c);
        cards_.clear();
        scene_.commit();
        scene_.trim();
        PostMessageW(notify_, kMsgExitDone, reinterpret_cast<WPARAM>(selected_), 0);
    } else if (enterPending_) {  // Enter, or a Relayout/interaction that interrupted it
        enterPending_ = false;
        PostMessageW(notify_, kMsgEnterDone, 0, 0);
    }
}

}  // namespace mc
