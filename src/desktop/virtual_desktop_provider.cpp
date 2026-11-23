#include "desktop/virtual_desktop_provider.h"

#include <objbase.h>

#include <cstring>
#include <string>
#include <vector>

#include "diagnostics/log.h"

namespace mc {

namespace {

constexpr wchar_t kExplorer[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer";
constexpr wchar_t kDesktops[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";

std::vector<std::uint8_t> readBinary(HKEY key, const wchar_t* subkey, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(key, subkey, value, RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS) return {};
    std::vector<std::uint8_t> data(size);
    if (RegGetValueW(key, subkey, value, RRF_RT_REG_BINARY, nullptr, data.data(), &size) != ERROR_SUCCESS) return {};
    data.resize(size);
    return data;
}

std::wstring readString(HKEY key, const std::wstring& subkey, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(key, subkey.c_str(), value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return {};
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subkey.c_str(), value, RRF_RT_REG_SZ, nullptr, s.data(), &size) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

GUID toGuid(const DesktopId& id) {
    GUID g;
    std::memcpy(&g, id.bytes.data(), sizeof g);
    return g;
}

// Presses modifiers, taps `key`, releases modifiers in reverse order. One SendInput call so the
// chord cannot interleave with other input. A modifier the user is already holding (Ctrl in the
// overview's Ctrl+Arrow) is neither pressed nor released, so their physical key state survives.
bool sendChord(std::initializer_list<WORD> requested, WORD key) {
    std::vector<WORD> modifiers;
    for (WORD m : requested) {
        const bool held = m == VK_LWIN ? (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) : GetAsyncKeyState(m) < 0;
        if (!held) modifiers.push_back(m);
    }
    auto make = [](WORD vk, bool up) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        const bool extended = vk == VK_LWIN || vk == VK_LEFT || vk == VK_RIGHT;
        in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        return in;
    };
    std::vector<INPUT> inputs;
    for (WORD m : modifiers) inputs.push_back(make(m, false));
    inputs.push_back(make(key, false));
    inputs.push_back(make(key, true));
    for (auto it = modifiers.rbegin(); it != modifiers.rend(); ++it) inputs.push_back(make(*it, true));
    const UINT sent = SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT));
    if (sent != inputs.size()) {
        // UIPI blocks injection when the foreground window belongs to a higher-integrity process.
        log::lastError("SendInput(desktop shortcut)");
        return false;
    }
    return true;
}

}  // namespace

VirtualDesktopProvider::~VirtualDesktopProvider() {
    if (key_) RegCloseKey(key_);
    if (event_) CloseHandle(event_);
}

bool VirtualDesktopProvider::init() {
    hasDesktopsKey_ = RegOpenKeyExW(HKEY_CURRENT_USER, kDesktops, 0, KEY_READ | KEY_NOTIFY, &key_) == ERROR_SUCCESS;
    // Before the user ever creates a second desktop the key may not exist: watch Explorer instead.
    if (!hasDesktopsKey_ && RegOpenKeyExW(HKEY_CURRENT_USER, kExplorer, 0, KEY_READ | KEY_NOTIFY, &key_) != ERROR_SUCCESS) {
        log::write("virtual desktops: cannot open registry key; desktop features disabled");
        key_ = nullptr;
        return false;
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) { log::lastError("CreateEvent(desktop change)"); return false; }
    rearm();
    if (auto s = read()) log::write("virtual desktops: %zu, current %d", s->desktops.size(), s->current);
    return true;
}

void VirtualDesktopProvider::rearm() {
    if (!key_ || !event_) return;
    // Re-open the specific key once it appears (first desktop created after startup).
    if (!hasDesktopsKey_) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kDesktops, 0, KEY_READ | KEY_NOTIFY, &k) == ERROR_SUCCESS) {
            RegCloseKey(key_);
            key_ = k;
            hasDesktopsKey_ = true;
        }
    }
    const LONG rc = RegNotifyChangeKeyValue(key_, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
                                            event_, TRUE);
    if (rc != ERROR_SUCCESS) log::write("RegNotifyChangeKeyValue failed: %ld", rc);
}

std::optional<DesktopState> VirtualDesktopProvider::read() const {
    if (!key_) return std::nullopt;
    DesktopState s;
    const auto raw = readBinary(HKEY_CURRENT_USER, kDesktops, L"VirtualDesktopIDs");
    auto ids = parseDesktopIds(raw.data(), raw.size());
    if (!ids) {
        log::write("virtual desktops: VirtualDesktopIDs has unexpected size %zu", raw.size());
        return std::nullopt;
    }
    if (ids->empty()) {
        // Single desktop that the shell never persisted: one entry, identity unknown.
        s.desktops.push_back({});
        s.current = 0;
        return s;
    }
    for (auto& id : *ids) {
        wchar_t guid[40];
        const GUID g = toGuid(id);
        StringFromGUID2(g, guid, 40);
        s.desktops.push_back({id, readString(HKEY_CURRENT_USER, std::wstring(kDesktops) + L"\\Desktops\\" + guid, L"Name")});
    }

    auto cur = readBinary(HKEY_CURRENT_USER, kDesktops, L"CurrentVirtualDesktop");
    if (cur.size() != 16) {
        // Windows 10 kept it per session.
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        const std::wstring sub = std::wstring(kExplorer) + L"\\SessionInfo\\" + std::to_wstring(session) + L"\\VirtualDesktops";
        cur = readBinary(HKEY_CURRENT_USER, sub.c_str(), L"CurrentVirtualDesktop");
    }
    if (cur.size() == 16) {
        DesktopId id;
        std::memcpy(id.bytes.data(), cur.data(), 16);
        s.current = s.indexOf(id);
    }
    return s;
}

bool VirtualDesktopProvider::step(int direction) const {
    return direction != 0 && sendChord({VK_CONTROL, VK_LWIN}, direction > 0 ? VK_RIGHT : VK_LEFT);
}

bool VirtualDesktopProvider::create() const { return sendChord({VK_CONTROL, VK_LWIN}, 'D'); }

bool VirtualDesktopProvider::removeCurrent() const { return sendChord({VK_CONTROL, VK_LWIN}, VK_F4); }

}  // namespace mc
