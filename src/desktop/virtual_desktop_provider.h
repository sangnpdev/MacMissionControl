#pragma once
// Virtual desktops through public mechanisms only (see docs/decisions/ADR-003-virtual-desktops.md):
//   read      : HKCU\...\Explorer\VirtualDesktops registry values (undocumented, read-only, stable since Win10)
//   notify    : RegNotifyChangeKeyValue on that key (event-driven, no polling)
//   switch    : SendInput of the documented shortcuts Ctrl+Win+Left/Right (relative, one desktop per press)
//   create    : Ctrl+Win+D (also switches to the new desktop)
//   remove    : Ctrl+Win+F4 (current desktop only; its windows move to the adjacent desktop)
// The overlay needs no desktop handling: WS_EX_TOOLWINDOW windows show on every desktop (verified).
// No internal shell COM interfaces, no byte patterns, no registry writes.
#include <windows.h>

#include <optional>

#include "desktop/desktop_state.h"

namespace mc {

class VirtualDesktopProvider {
public:
    VirtualDesktopProvider() = default;
    VirtualDesktopProvider(const VirtualDesktopProvider&) = delete;
    VirtualDesktopProvider& operator=(const VirtualDesktopProvider&) = delete;
    ~VirtualDesktopProvider();

    // Returns false (logged) if desktops cannot be read; the rest of the app keeps working without them.
    bool init();

    std::optional<DesktopState> read() const;

    // Auto-reset event, signaled when the shell writes desktop state. Call rearm() after handling it.
    HANDLE changeEvent() const { return event_; }
    void rearm();

    bool step(int direction) const;  // -1 left, +1 right
    bool create() const;
    bool removeCurrent() const;

private:
    HKEY key_ = nullptr;      // ...\Explorer\VirtualDesktops (or \Explorer if that key does not exist yet)
    bool hasDesktopsKey_ = false;
    HANDLE event_ = nullptr;
};

}  // namespace mc
