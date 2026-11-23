#include "desktop/desktop_state.h"

#include <algorithm>
#include <cstring>

namespace mc {

bool DesktopId::isNull() const {
    return std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b == 0; });
}

int DesktopState::indexOf(const DesktopId& id) const {
    for (std::size_t i = 0; i < desktops.size(); ++i)
        if (desktops[i].id == id) return int(i);
    return -1;
}

std::wstring DesktopState::displayName(int index) const {
    if (index < 0 || index >= int(desktops.size())) return {};
    if (!desktops[index].name.empty()) return desktops[index].name;
    return L"Desktop " + std::to_wstring(index + 1);
}

std::optional<std::vector<DesktopId>> parseDesktopIds(const std::uint8_t* data, std::size_t size) {
    if (size % 16 != 0 || (size > 0 && !data)) return std::nullopt;
    std::vector<DesktopId> ids(size / 16);
    for (std::size_t i = 0; i < ids.size(); ++i) std::memcpy(ids[i].bytes.data(), data + i * 16, 16);
    return ids;
}

int switchSteps(int from, int to) { return (from < 0 || to < 0) ? 0 : to - from; }

}  // namespace mc
