#pragma once
// Per-application and per-window settings for the experimental traffic lights, and the decision "cover this
// window?" that combines them with the compatibility verdict. Pure (no <windows.h>), JSON in and out.
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "config/json.h"
#include "window/compat_engine.h"

namespace mc {

enum class CoverMode { ListedApps, AllCompatible, VerifiedOnly };

struct AppSettings {
    std::optional<bool> enabled;           // false = excluded; true = opted in (also covers Partial in VerifiedOnly mode)
    std::optional<double> heightLogical;   // override of the native strip height (logical px)
};

// Matches a window of one app by class and/or a title fragment (an HWND is not stable across runs).
struct WindowRule {
    std::wstring exe, className, titleContains;  // exe required; the others empty = any
    bool enabled = true;
    bool operator==(const WindowRule&) const = default;
};

struct RegistryData {
    CoverMode mode = CoverMode::ListedApps;
    std::optional<bool> featureEnabled;        // the user's own on/off switch (settings window / tray); unset = config decides
    std::map<std::wstring, AppSettings> apps;  // key: lower-case exe file name
    std::vector<WindowRule> windows;
};

// Priority: window rule > application setting > global default (cover when compatible).
struct Policy {
    std::optional<bool> enabled;
    std::optional<double> heightLogical;
};
Policy resolvePolicy(const RegistryData& d, const std::wstring& exe, const std::wstring& className, const std::wstring& title);

struct Decision {
    bool cover = false;
    const char* why = "";
};
// The applications the feature is made for (lower-case exe names). Apps that are not installed are simply never seen.
const std::vector<std::wstring>& listedApps();
bool isListedApp(const std::wstring& exe);

// `listed`: the window belongs to an app of listedApps() (only used by CoverMode::ListedApps).
Decision decide(const Assessment& a, const Policy& p, CoverMode mode, bool listed = false);

// What the on/off switch of one app means. `defaultCovers`: would the app be covered with no setting at all?
//   off -> enabled=false (a custom height is kept);   on -> back to the default when that already covers, else an explicit opt-in.
void setAppOn(RegistryData& d, const std::wstring& exe, bool on, bool defaultCovers);
void resetApp(RegistryData& d, const std::wstring& exe);  // forget every user setting of the app
bool isCustomApp(const RegistryData& d, const std::wstring& exe);    // explicit opt-in
bool isExcludedApp(const RegistryData& d, const std::wstring& exe);  // turned off by the user

// JSON: {"enabled": true, "mode": "listedApps"|"allCompatible"|"verifiedOnly", "apps": {"x.exe": {"enabled": false, "height": 36}}, "windows": [{...}]}
// `parseRegistry` adds to `out` (entries of the same app are replaced) and appends human-readable errors.
void parseRegistry(const json::Value& obj, const std::string& path, RegistryData& out, std::vector<std::string>& errs);
std::string toJson(const RegistryData& d);

std::wstring lowerExe(std::wstring s);
std::wstring widenUtf8(const std::string& s);
std::string narrowUtf8(const std::wstring& s);

}  // namespace mc
