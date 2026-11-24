#pragma once
// Settings window of the experimental traffic lights: a plain Win32 window (tab control, list view, buttons) in the
// main process. Four tabs: General, Applications (everything detected, with its compatibility), Custom, Excluded;
// a details pane shows what is known about the selected application. All data comes in and goes out through the host
// callbacks, so the window knows nothing about hooks or overlays.
#include <windows.h>
#include <commctrl.h>  // HIMAGELIST

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "app/edge_poc.h"
#include "window/app_registry.h"

namespace mc {

struct SettingsHost {
    std::function<std::vector<AppInfo>()> detected;  // apps seen since the feature started
    std::function<RegistryData&()> user;             // the user's own settings (edited in place)
    std::function<bool()> running;                   // is the feature on?
    std::function<void(bool)> setRunning;            // switch it (the host persists it)
    std::function<void()> changed;                   // `user()` was modified: save it and apply it
};

class SettingsWindow {
public:
    ~SettingsWindow() { close(); }
    void show(HINSTANCE inst, SettingsHost host);  // opens the window, or brings the open one to the front
    void close();
    bool isOpen() const { return hwnd_ != nullptr; }
    bool preTranslate(MSG* msg);  // keyboard navigation (Tab, Esc); true when the message was consumed
    void refresh();               // re-read the host's data (the window also does it when it is shown or a tab changes)

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT onMessage(UINT msg, WPARAM wp, LPARAM lp);
    void create();
    void layout();
    void populate(const std::wstring& keepExe = L"");
    void onSelection();
    void onToggled(int row, bool on);
    void onCommand(int id);
    void addFromBrowse();
    void applyChange();
    int S(int px) const;

    struct Row {
        std::wstring exe;
        const AppInfo* info = nullptr;  // into apps_; null when the app was never seen
    };
    std::wstring selectedExe() const;
    std::wstring statusOf(const Row& r) const;
    std::wstring detailsOf(const Row& r) const;
    bool coversByDefault(const std::wstring& exe, const AppInfo* info) const;
    bool effectiveOn(const Row& r) const;
    std::wstring displayName(const std::wstring& exe, const std::wstring& path);
    int iconIndex(const std::wstring& exe, const std::wstring& path);

    HWND hwnd_ = nullptr, tab_ = nullptr, list_ = nullptr, details_ = nullptr, info_ = nullptr;
    HWND chkEnable_ = nullptr, radListed_ = nullptr, radAll_ = nullptr, radVerified_ = nullptr, general_ = nullptr;
    HWND btnAdd_ = nullptr, btnRemove_ = nullptr, btnReset_ = nullptr, btnRefresh_ = nullptr, btnClose_ = nullptr;
    HFONT font_ = nullptr;
    HIMAGELIST images_ = nullptr;
    HINSTANCE inst_ = nullptr;
    SettingsHost host_;
    std::vector<AppInfo> apps_;
    std::vector<Row> rows_;
    std::map<std::wstring, int> iconCache_;
    std::map<std::wstring, std::wstring> nameCache_;
    int dpi_ = 96;
    int tabIndex_ = 0;
    bool populating_ = false;
};

}  // namespace mc
