#pragma once
// Start with Windows: HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MissionControl (per user, no
// admin). Written only when the user turns it on (tray menu or --autostart on); never automatically.
// The user can also switch it off in Settings > Apps > Startup / Task Manager > Startup apps; that
// choice is stored by Windows in ...\Explorer\StartupApproved\Run, which we only read.
#include <cstddef>
#include <cstdint>
#include <string>

namespace mc::autostart {

enum class State {
    Off,             // no Run entry for this exe (or one pointing to another copy)
    On,              // Run entry for this exe, allowed by Windows
    DisabledByUser,  // Run entry for this exe, switched off in Settings / Task Manager
};

State state();
bool enable();   // writes the Run entry for this exe
bool disable();  // removes the Run entry
std::wstring command();  // the Run value for this exe: "\"<path>\""

// StartupApproved value: first byte even = enabled, odd = disabled (undocumented, read-only use).
// A missing or short value counts as enabled, which is what Windows does.
inline bool startupApproved(const std::uint8_t* data, std::size_t size) { return size == 0 || !data || (data[0] & 1) == 0; }

}  // namespace mc::autostart
