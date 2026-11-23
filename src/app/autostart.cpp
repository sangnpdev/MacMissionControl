#include "app/autostart.h"

#include <windows.h>

#include <vector>

#include "diagnostics/log.h"

namespace mc::autostart {

namespace {
constexpr wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApproved[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kName[] = L"MissionControl";

std::wstring exePath() {
    std::wstring p(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, p.data(), DWORD(p.size()));
        if (n < p.size()) { p.resize(n); return p; }
        p.resize(p.size() * 2);
    }
}
}  // namespace

std::wstring command() { return L"\"" + exePath() + L"\""; }

State state() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRun, kName, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return State::Off;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRun, kName, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) return State::Off;
    value.resize(wcsnlen(value.c_str(), value.size()));
    if (_wcsicmp(value.c_str(), command().c_str()) != 0) return State::Off;  // another copy of the app

    std::vector<std::uint8_t> approved(16);
    DWORD asize = DWORD(approved.size());
    if (RegGetValueW(HKEY_CURRENT_USER, kApproved, kName, RRF_RT_REG_BINARY, nullptr, approved.data(), &asize) != ERROR_SUCCESS) asize = 0;
    return startupApproved(approved.data(), asize) ? State::On : State::DisabledByUser;
}

bool enable() {
    const std::wstring cmd = command();
    const LSTATUS rc = RegSetKeyValueW(HKEY_CURRENT_USER, kRun, kName, REG_SZ, cmd.c_str(), DWORD((cmd.size() + 1) * sizeof(wchar_t)));
    if (rc != ERROR_SUCCESS) { log::write("autostart: cannot write Run entry (%ld)", rc); return false; }
    log::write("autostart: on (%ls)", cmd.c_str());
    return true;
}

bool disable() {
    const LSTATUS rc = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRun, kName);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) { log::write("autostart: cannot remove Run entry (%ld)", rc); return false; }
    log::write("autostart: off");
    return true;
}

}  // namespace mc::autostart
