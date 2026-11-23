#include "input/wintab_detector.h"

namespace mc {

namespace {
// Virtual-key codes (from WinUser.h; duplicated so this module stays free of <windows.h>).
constexpr std::uint32_t kTab = 0x09, kLWin = 0x5B, kRWin = 0x5C;
constexpr std::uint32_t kCtrl = 0x11, kLCtrl = 0xA2, kRCtrl = 0xA3;
constexpr std::uint32_t kAlt = 0x12, kLAlt = 0xA4, kRAlt = 0xA5;
}  // namespace

WinTabDetector::Action WinTabDetector::onKey(const KeyEvent& e) {
    if (e.injected && !acceptInjected_) return Action::Pass;

    switch (e.vk) {
    case kLWin: lwin_ = e.down; return Action::Pass;
    case kRWin: rwin_ = e.down; return Action::Pass;
    case kCtrl: case kLCtrl: case kRCtrl: ctrl_ = e.down; return Action::Pass;
    case kAlt: case kLAlt: case kRAlt: alt_ = e.down; return Action::Pass;
    case kTab: break;
    default: return Action::Pass;
    }

    if (!e.down) {
        // Key-up of a Tab we swallowed: swallow it too, so no app sees a lone Tab-up.
        const bool swallowed = tabSwallowed_;
        tabSwallowed_ = false;
        return swallowed ? Action::Swallow : Action::Pass;
    }
    if (tabSwallowed_) return Action::Swallow;  // auto-repeat while held: one trigger per press
    // Win+Tab and Win+Shift+Tab. Combinations with Ctrl or Alt keep their Windows meaning.
    if ((lwin_ || rwin_) && !ctrl_ && !alt_) {
        tabSwallowed_ = true;
        return Action::Trigger;
    }
    return Action::Pass;
}

}  // namespace mc
