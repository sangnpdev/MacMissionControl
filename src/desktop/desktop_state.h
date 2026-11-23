#pragma once
// Pure virtual-desktop model. The platform provider fills it from the registry.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// Raw 16-byte GUID as stored in the registry (Windows GUID memory layout).
struct DesktopId {
    std::array<std::uint8_t, 16> bytes{};
    bool operator==(const DesktopId&) const = default;
    bool isNull() const;
};

struct DesktopInfo {
    DesktopId id;
    std::wstring name;  // user-set name; empty when the user never renamed it
};

struct DesktopState {
    std::vector<DesktopInfo> desktops;  // in the order the shell shows them
    int current = -1;                   // index into desktops, -1 if unknown

    int indexOf(const DesktopId& id) const;
    // Name to show: the user's name, or "Desktop N" like the shell.
    std::wstring displayName(int index) const;
};

// Parses the REG_BINARY "VirtualDesktopIDs" value: a concatenation of 16-byte GUIDs.
// Returns nullopt when the size is not a multiple of 16.
std::optional<std::vector<DesktopId>> parseDesktopIds(const std::uint8_t* data, std::size_t size);

// Relative steps to go from `from` to `to` (positive = right). Shortcut switching moves one desktop
// per step, so callers send |steps| Ctrl+Win+Arrow presses, one after each confirmed change.
int switchSteps(int from, int to);

}  // namespace mc
