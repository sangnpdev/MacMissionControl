#pragma once
// Pure state machine behind the Win+Tab takeover (Phase 5). Fed with every low-level keyboard event;
// tells the hook whether to pass the event on, swallow it, or swallow it and open the overview.
#include <cstdint>

namespace mc {

struct KeyEvent {
    std::uint32_t vk = 0;
    bool down = false;
    bool injected = false;  // LLKHF_INJECTED: synthetic input (ours or other tools')
};

class WinTabDetector {
public:
    enum class Action { Pass, Swallow, Trigger };  // Trigger = swallow and open/close the overview

    // acceptInjected: also react to synthetic Win+Tab (tests only; in normal use synthetic input is
    // passed through, so our own injected keys can never loop back into the detector).
    explicit WinTabDetector(bool acceptInjected = false) : acceptInjected_(acceptInjected) {}

    Action onKey(const KeyEvent& e);

private:
    bool acceptInjected_;
    bool lwin_ = false, rwin_ = false, ctrl_ = false, alt_ = false;
    bool tabSwallowed_ = false;  // the Tab key-down was swallowed: swallow its repeats and key-up too
};

}  // namespace mc
