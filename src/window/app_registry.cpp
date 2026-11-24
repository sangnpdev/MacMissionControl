#include "window/app_registry.h"

#include <cwctype>

namespace mc {

std::wstring lowerExe(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}

std::wstring widenUtf8(const std::string& s) {
    std::wstring out;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned cp = 0;
        int n = 0;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
        else { out.push_back(L'?'); ++i; continue; }
        if (i + n > s.size()) { out.push_back(L'?'); break; }
        for (int k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        if (cp >= 0x10000) { cp -= 0x10000; out.push_back(wchar_t(0xD800 + (cp >> 10))); out.push_back(wchar_t(0xDC00 + (cp & 0x3FF))); }
        else out.push_back(wchar_t(cp));
    }
    return out;
}

std::string narrowUtf8(const std::wstring& w) {
    std::string out;
    for (std::size_t i = 0; i < w.size(); ++i) {
        unsigned cp = w[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (unsigned(w[i + 1]) - 0xDC00);
            ++i;
        }
        if (cp < 0x80) out.push_back(char(cp));
        else if (cp < 0x800) { out.push_back(char(0xC0 | (cp >> 6))); out.push_back(char(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back(char(0xE0 | (cp >> 12))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
        else { out.push_back(char(0xF0 | (cp >> 18))); out.push_back(char(0x80 | ((cp >> 12) & 0x3F))); out.push_back(char(0x80 | ((cp >> 6) & 0x3F))); out.push_back(char(0x80 | (cp & 0x3F))); }
    }
    return out;
}

Policy resolvePolicy(const RegistryData& d, const std::wstring& exeIn, const std::wstring& className, const std::wstring& title) {
    const std::wstring exe = lowerExe(exeIn);
    Policy p;
    if (auto it = d.apps.find(exe); it != d.apps.end()) {
        p.enabled = it->second.enabled;
        p.heightLogical = it->second.heightLogical;
    }
    // Window rules override the application setting; the first matching rule wins.
    for (const WindowRule& r : d.windows) {
        if (lowerExe(r.exe) != exe) continue;
        if (!r.className.empty() && r.className != className) continue;
        if (!r.titleContains.empty() && title.find(r.titleContains) == std::wstring::npos) continue;
        p.enabled = r.enabled;
        break;
    }
    return p;
}

const std::vector<std::wstring>& listedApps() {
    // The user's list (2026-11-24). UWP apps are named by their own process, not by ApplicationFrameHost.exe.
    static const std::vector<std::wstring> apps{
        L"msedge.exe", L"chrome.exe", L"explorer.exe", L"notepad.exe", L"windowsterminal.exe",  // measured profiles
        L"code.exe", L"cursor.exe", L"dbx.exe", L"dbeaver.exe", L"warp.exe", L"powershell.exe", L"pwsh.exe", L"cmd.exe",
        L"docker desktop.exe", L"studio64.exe", L"idea64.exe", L"winrar.exe", L"zalo.exe", L"cloudflare warp.exe",
        L"photos.exe", L"microsoft.media.player.exe", L"devenv.exe"};
    return apps;
}

bool isListedApp(const std::wstring& exe) {
    const std::wstring e = lowerExe(exe);
    for (const auto& a : listedApps())
        if (a == e) return true;
    return false;
}

Decision decide(const Assessment& a, const Policy& p, CoverMode mode, bool listed) {
    if (p.enabled && !*p.enabled) return {false, "turned off for this app or window"};
    if (!a.covers()) return {false, a.reason};
    if (mode == CoverMode::ListedApps && !listed && !(p.enabled && *p.enabled)) return {false, "not in the application list"};
    if (mode == CoverMode::VerifiedOnly && !(a.verdict == Verdict::Supported && a.source == StripSource::Verified) && !(p.enabled && *p.enabled))
        return {false, "verified-only mode"};
    return {true, a.reason};
}

void setAppOn(RegistryData& d, const std::wstring& exeIn, bool on, bool defaultCovers) {
    const std::wstring exe = lowerExe(exeIn);
    if (!on) { d.apps[exe].enabled = false; return; }
    auto it = d.apps.find(exe);
    if (defaultCovers) {
        if (it == d.apps.end()) return;
        it->second.enabled.reset();
        if (!it->second.heightLogical) d.apps.erase(it);
    } else {
        d.apps[exe].enabled = true;
    }
}

void resetApp(RegistryData& d, const std::wstring& exe) { d.apps.erase(lowerExe(exe)); }

bool isCustomApp(const RegistryData& d, const std::wstring& exe) {
    const auto it = d.apps.find(lowerExe(exe));
    return it != d.apps.end() && it->second.enabled && *it->second.enabled;
}

bool isExcludedApp(const RegistryData& d, const std::wstring& exe) {
    const auto it = d.apps.find(lowerExe(exe));
    return it != d.apps.end() && it->second.enabled && !*it->second.enabled;
}

namespace {
void err(std::vector<std::string>& errs, const std::string& path, const std::string& msg) { errs.push_back(path + " " + msg); }

bool readString(const json::Value& o, const char* key, std::wstring& out, const std::string& path, std::vector<std::string>& errs) {
    const json::Value* v = o.find(key);
    if (!v) return true;
    if (v->type != json::Value::Type::String) { err(errs, path + "." + key, "must be a string"); return false; }
    out = widenUtf8(v->string);
    return true;
}
}  // namespace

void parseRegistry(const json::Value& obj, const std::string& path, RegistryData& out, std::vector<std::string>& errs) {
    if (obj.type != json::Value::Type::Object) { err(errs, path, "must be an object"); return; }
    for (auto& [k, v] : obj.object) {
        const std::string p = path + "." + k;
        if (k == "enabled") {
            if (v.type == json::Value::Type::Bool) out.featureEnabled = v.boolean;
            else err(errs, p, "must be true or false");
        } else if (k == "mode") {
            if (v.type == json::Value::Type::String && v.string == "listedApps") out.mode = CoverMode::ListedApps;
            else if (v.type == json::Value::Type::String && v.string == "allCompatible") out.mode = CoverMode::AllCompatible;
            else if (v.type == json::Value::Type::String && v.string == "verifiedOnly") out.mode = CoverMode::VerifiedOnly;
            else err(errs, p, "must be \"listedApps\", \"allCompatible\" or \"verifiedOnly\"");
        } else if (k == "apps") {
            if (v.type != json::Value::Type::Object) { err(errs, p, "must be an object"); continue; }
            for (auto& [exe, s] : v.object) {
                const std::string ep = p + "." + exe;
                if (exe.empty() || s.type != json::Value::Type::Object) { err(errs, ep, "must be an object"); continue; }
                AppSettings as;
                bool ok = true;
                for (auto& [sk, sv] : s.object) {
                    if (sk == "enabled") {
                        if (sv.type == json::Value::Type::Bool) as.enabled = sv.boolean;
                        else { err(errs, ep + ".enabled", "must be true or false"); ok = false; }
                    } else if (sk == "height") {
                        if (sv.type == json::Value::Type::Number && sv.number >= 16 && sv.number <= 80) as.heightLogical = sv.number;
                        else { err(errs, ep + ".height", "must be a number from 16 to 80"); ok = false; }
                    } else { err(errs, ep + "." + sk, "unknown key"); ok = false; }
                }
                if (ok) out.apps[lowerExe(widenUtf8(exe))] = as;
            }
        } else if (k == "windows") {
            if (v.type != json::Value::Type::Array) { err(errs, p, "must be an array"); continue; }
            for (std::size_t i = 0; i < v.array.size(); ++i) {
                const json::Value& e = v.array[i];
                const std::string ep = p + "[" + std::to_string(i) + "]";
                if (e.type != json::Value::Type::Object) { err(errs, ep, "must be an object"); continue; }
                WindowRule r;
                bool ok = readString(e, "exe", r.exe, ep, errs) && readString(e, "class", r.className, ep, errs) && readString(e, "titleContains", r.titleContains, ep, errs);
                if (const json::Value* en = e.find("enabled")) {
                    if (en->type == json::Value::Type::Bool) r.enabled = en->boolean;
                    else { err(errs, ep + ".enabled", "must be true or false"); ok = false; }
                }
                for (auto& [wk, wv] : e.object)
                    if (wk != "exe" && wk != "class" && wk != "titleContains" && wk != "enabled") { err(errs, ep + "." + wk, "unknown key"); ok = false; }
                if (r.exe.empty()) { err(errs, ep + ".exe", "is required"); ok = false; }
                if (ok) out.windows.push_back(std::move(r));
            }
        } else {
            err(errs, p, "unknown key");
        }
    }
}

namespace {
std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o.push_back(char(c));
        }
    }
    return o + "\"";
}
}  // namespace

std::string toJson(const RegistryData& d) {
    std::string o = "{\n";
    if (d.featureEnabled) o += std::string("  \"enabled\": ") + (*d.featureEnabled ? "true" : "false") + ",\n";
    o += "  \"mode\": ";
    o += d.mode == CoverMode::VerifiedOnly ? "\"verifiedOnly\"" : d.mode == CoverMode::AllCompatible ? "\"allCompatible\"" : "\"listedApps\"";
    o += ",\n  \"apps\": {";
    bool first = true;
    for (auto& [exe, s] : d.apps) {
        o += first ? "\n    " : ",\n    ";
        first = false;
        o += quote(narrowUtf8(exe)) + ": {";
        bool f2 = true;
        if (s.enabled) { o += std::string("\"enabled\": ") + (*s.enabled ? "true" : "false"); f2 = false; }
        if (s.heightLogical) { char b[32]; snprintf(b, sizeof b, "%g", *s.heightLogical); o += std::string(f2 ? "" : ", ") + "\"height\": " + b; }
        o += "}";
    }
    o += first ? "}" : "\n  }";
    o += ",\n  \"windows\": [";
    for (std::size_t i = 0; i < d.windows.size(); ++i) {
        const WindowRule& r = d.windows[i];
        o += i ? ",\n    {" : "\n    {";
        o += "\"exe\": " + quote(narrowUtf8(r.exe));
        if (!r.className.empty()) o += ", \"class\": " + quote(narrowUtf8(r.className));
        if (!r.titleContains.empty()) o += ", \"titleContains\": " + quote(narrowUtf8(r.titleContains));
        o += std::string(", \"enabled\": ") + (r.enabled ? "true" : "false") + "}";
    }
    o += d.windows.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

}  // namespace mc
