#include "app/settings_window.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>

#include "diagnostics/log.h"

namespace mc {

namespace {
constexpr wchar_t kClass[] = L"MCSettingsWindow";
enum Id { IDC_TAB = 100, IDC_LIST, IDC_ENABLE, IDC_RAD_LISTED, IDC_RAD_ALL, IDC_RAD_VER, IDC_INFO, IDC_ADD, IDC_REMOVE, IDC_RESET, IDC_REFRESH, IDC_CLOSE, IDC_DETAILS };
enum Tab { TabGeneral = 0, TabApps, TabCustom, TabExcluded };

std::wstring baseName(const std::wstring& exe) {
    std::wstring n = exe;
    if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".exe") == 0) n.resize(n.size() - 4);
    return n;
}

std::wstring fileDescription(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD h = 0;
    const DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &h);
    if (!sz) return {};
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return {};
    struct Lang { WORD lang, cp; }* lt = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&lt), &len) || len < sizeof(Lang)) return {};
    wchar_t q[64];
    swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", lt->lang, lt->cp);
    wchar_t* d = nullptr;
    if (VerQueryValueW(buf.data(), q, reinterpret_cast<LPVOID*>(&d), &len) && len > 1 && d) return d;
    return {};
}

std::wstring widenAscii(const std::string& s) { return widenUtf8(s); }

const wchar_t* sourceText(StripSource s) {
    switch (s) {
    case StripSource::Verified: return L"measured profile for this app";
    case StripSource::Combined: return L"Windows' caption-button bounds and the window's own hit-test agree";
    case StripSource::HitTest: return L"the window's own hit-test (Windows reports no caption bounds)";
    case StripSource::DwmBounds: return L"Windows' caption-button bounds only";
    default: return L"not located";
    }
}
}  // namespace

int SettingsWindow::S(int px) const { return MulDiv(px, dpi_, 96); }

void SettingsWindow::show(HINSTANCE inst, SettingsHost host) {
    host_ = std::move(host);
    inst_ = inst;
    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        refresh();
        return;
    }
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = &SettingsWindow::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);  // fails harmlessly when registered before
    const UINT dpi = GetDpiForSystem();
    hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"Mission Control - Traffic lights", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            MulDiv(860, dpi, 96), MulDiv(660, dpi, 96), nullptr, nullptr, inst, this);
    if (!hwnd_) { log::lastError("CreateWindow(settings)"); return; }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::close() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool SettingsWindow::preTranslate(MSG* msg) { return hwnd_ && IsDialogMessageW(hwnd_, msg); }

LRESULT CALLBACK SettingsWindow::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        self = static_cast<SettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->onMessage(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

void SettingsWindow::create() {
    dpi_ = int(GetDpiForWindow(hwnd_));
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, UINT(dpi_));
    font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(INT_PTR(id)), inst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return c;
    };
    tab_ = make(WC_TABCONTROLW, L"", WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, IDC_TAB);
    const wchar_t* names[] = {L"General", L"Applications", L"Custom applications", L"Excluded applications"};
    for (int i = 0; i < 4; ++i) {
        TCITEMW ti{TCIF_TEXT};
        ti.pszText = const_cast<wchar_t*>(names[i]);
        TabCtrl_InsertItem(tab_, i, &ti);
    }
    // General page
    chkEnable_ = make(L"BUTTON", L"Enable traffic lights on application windows (experimental)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_ENABLE);
    radListed_ = make(L"BUTTON", L"Listed applications (recommended): Edge, Chrome, Explorer, Notepad, Terminal, VS Code, Cursor, DBX, "
                                 L"DBeaver, Warp, PowerShell, Command Prompt, Docker Desktop, Android Studio, IntelliJ IDEA, WinRAR, Zalo, "
                                 L"Cloudflare One Client, Photos, Media Player, Visual Studio, plus applications you add yourself.",
                      BS_AUTORADIOBUTTON | BS_MULTILINE | WS_GROUP | WS_TABSTOP, IDC_RAD_LISTED);
    radAll_ = make(L"BUTTON", L"All compatible applications: cover every window whose caption buttons can be located.",
                   BS_AUTORADIOBUTTON | BS_MULTILINE | WS_TABSTOP, IDC_RAD_ALL);
    radVerified_ = make(L"BUTTON", L"Only applications with a measured profile (and applications you turned on yourself)",
                        BS_AUTORADIOBUTTON | BS_MULTILINE | WS_TABSTOP, IDC_RAD_VER);
    info_ = make(L"STATIC", L"Games and fullscreen windows, system processes, elevated windows and unsupported windows are never "
                            L"covered. Covering a window hides its native buttons; the window's own Close, Minimize and Maximize keep "
                            L"working, but the Snap Layout flyout on the maximize button is not available. Settings are saved to "
                            L"%LOCALAPPDATA%\\MissionControl\\apps.json.",
                 SS_LEFT, IDC_INFO);
    // List pages
    list_ = make(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER, IDC_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t* cols[] = {L"Application", L"Executable", L"Status"};
    const int widths[] = {230, 150, 380};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM};
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = S(widths[i]);
        c.iSubItem = i;
        ListView_InsertColumn(list_, i, &c);
    }
    const int icon = GetSystemMetricsForDpi(SM_CXSMICON, UINT(dpi_));
    images_ = ImageList_Create(icon, icon, ILC_COLOR32 | ILC_MASK, 16, 16);
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(L"x.exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof sfi, SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES)) {
        ImageList_AddIcon(images_, sfi.hIcon);  // index 0: generic application icon
        DestroyIcon(sfi.hIcon);
    }
    ListView_SetImageList(list_, images_, LVSIL_SMALL);
    details_ = make(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP | WS_BORDER, IDC_DETAILS, WS_EX_CLIENTEDGE);
    btnAdd_ = make(L"BUTTON", L"Add application...", BS_PUSHBUTTON | WS_TABSTOP, IDC_ADD);
    btnRemove_ = make(L"BUTTON", L"Remove", BS_PUSHBUTTON | WS_TABSTOP, IDC_REMOVE);
    btnReset_ = make(L"BUTTON", L"Reset to default", BS_PUSHBUTTON | WS_TABSTOP, IDC_RESET);
    btnRefresh_ = make(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, IDC_REFRESH);
    btnClose_ = make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLOSE);
}

void SettingsWindow::layout() {
    if (!tab_) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int W = rc.right, H = rc.bottom, m = S(10), bh = S(28), bw = S(150);
    const bool listTab = tabIndex_ != TabGeneral;
    const int detH = listTab ? S(150) : 0;
    const int by = H - m - bh;
    const int tabBottom = listTab ? by - m - detH - m : by - m;
    MoveWindow(tab_, m, m, W - 2 * m, tabBottom - m, TRUE);
    RECT page{m, m, W - m, tabBottom};
    TabCtrl_AdjustRect(tab_, FALSE, &page);
    InflateRect(&page, -S(6), -S(6));
    // General page
    const int gx = page.left, gw = page.right - page.left;
    MoveWindow(chkEnable_, gx, page.top, gw, S(24), TRUE);
    MoveWindow(radListed_, gx + S(18), page.top + S(40), gw - S(18), S(48), TRUE);
    MoveWindow(radAll_, gx + S(18), page.top + S(96), gw - S(18), S(24), TRUE);
    MoveWindow(radVerified_, gx + S(18), page.top + S(128), gw - S(18), S(24), TRUE);
    MoveWindow(info_, gx, page.top + S(172), gw, S(110), TRUE);
    for (HWND c : {chkEnable_, radListed_, radAll_, radVerified_, info_}) ShowWindow(c, listTab ? SW_HIDE : SW_SHOW);
    // List pages
    MoveWindow(list_, page.left, page.top, page.right - page.left, page.bottom - page.top, TRUE);
    ShowWindow(list_, listTab ? SW_SHOW : SW_HIDE);
    MoveWindow(details_, m, by - m - detH, W - 2 * m, detH, TRUE);
    ShowWindow(details_, listTab ? SW_SHOW : SW_HIDE);
    int x = m;
    for (HWND b : {btnAdd_, btnRemove_, btnReset_, btnRefresh_}) { MoveWindow(b, x, by, bw, bh, TRUE); x += bw + S(8); }
    MoveWindow(btnClose_, W - m - S(100), by, S(100), bh, TRUE);
    ShowWindow(btnAdd_, (tabIndex_ == TabCustom || tabIndex_ == TabExcluded) ? SW_SHOW : SW_HIDE);
    ShowWindow(btnRemove_, (tabIndex_ == TabCustom || tabIndex_ == TabExcluded) ? SW_SHOW : SW_HIDE);
    ShowWindow(btnReset_, (tabIndex_ == TabApps || tabIndex_ == TabCustom) ? SW_SHOW : SW_HIDE);
    ShowWindow(btnRefresh_, listTab ? SW_SHOW : SW_HIDE);
    // The status column takes what the others leave.
    const int total = page.right - page.left - GetSystemMetricsForDpi(SM_CXVSCROLL, UINT(dpi_)) - S(8);
    ListView_SetColumnWidth(list_, 2, std::max(S(120), total - ListView_GetColumnWidth(list_, 0) - ListView_GetColumnWidth(list_, 1)));
}

std::wstring SettingsWindow::displayName(const std::wstring& exe, const std::wstring& path) {
    auto it = nameCache_.find(exe);
    if (it != nameCache_.end() && (!it->second.empty() || path.empty())) return it->second.empty() ? baseName(exe) : it->second;
    std::wstring n = fileDescription(path);
    nameCache_[exe] = n;
    return n.empty() ? baseName(exe) : n;
}

int SettingsWindow::iconIndex(const std::wstring& exe, const std::wstring& path) {
    if (path.empty()) return 0;
    auto it = iconCache_.find(exe);
    if (it != iconCache_.end()) return it->second;
    int idx = 0;
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof sfi, SHGFI_ICON | SHGFI_SMALLICON)) {
        idx = ImageList_AddIcon(images_, sfi.hIcon);
        DestroyIcon(sfi.hIcon);
        if (idx < 0) idx = 0;
    }
    iconCache_[exe] = idx;
    return idx;
}

bool SettingsWindow::coversByDefault(const std::wstring& exe, const AppInfo* info) const {
    if (!info || !info->known) return false;
    Assessment a;
    a.verdict = info->verdict;
    a.source = info->source;
    return decide(a, Policy{}, host_.user().mode, isListedApp(exe)).cover;
}

bool SettingsWindow::effectiveOn(const Row& r) const {
    const RegistryData& d = host_.user();
    if (isExcludedApp(d, r.exe)) return false;
    if (isCustomApp(d, r.exe)) return true;
    return coversByDefault(r.exe, r.info);
}

std::wstring SettingsWindow::statusOf(const Row& r) const {
    const RegistryData& d = host_.user();
    std::wstring s;
    if (isExcludedApp(d, r.exe)) s = L"Turned off by you";
    else if (!r.info || !r.info->known) s = L"Not seen running yet (each window is checked when it appears)";
    else {
        switch (r.info->verdict) {
        case Verdict::Supported: s = r.info->source == StripSource::Verified ? L"Supported (measured profile)" : L"Supported"; break;
        case Verdict::Partial: s = L"Partially supported (button size not verified)"; break;
        case Verdict::Unsupported: s = L"Not supported: native buttons stay"; break;
        case Verdict::Excluded: s = L"Excluded (system process)"; break;
        }
        if (r.info->verdict == Verdict::Supported || r.info->verdict == Verdict::Partial) {
            if (isCustomApp(d, r.exe)) s += L", turned on by you";
            else if (!coversByDefault(r.exe, r.info)) s += isListedApp(r.exe) ? L", off in this mode" : L", not in the application list";
        }
    }
    if (r.info && r.info->windows > 0) s += L"   [" + std::to_wstring(r.info->windows) + L" window(s) covered]";
    return s;
}

std::wstring SettingsWindow::detailsOf(const Row& r) const {
    const RegistryData& d = host_.user();
    std::wstring t;
    const AppInfo* i = r.info;
    t += L"Executable: " + r.exe + L"\r\n";
    t += L"Path: " + ((i && !i->path.empty()) ? i->path : std::wstring(L"(not known: the application has not been seen running)")) + L"\r\n";
    if (i) {
        t += L"Process: " + std::to_wstring(i->pid) + L"   Window class: " + i->className + L"\r\n";
        t += L"Latest window: " + i->title + L"\r\n";
    }
    t += L"Compatibility: " + statusOf(r) + L"\r\n";
    if (i && i->known) {
        t += std::wstring(L"Buttons located by: ") + sourceText(i->source);
        if (i->heightLogical > 0) t += L"; native strip height " + std::to_wstring(int(i->heightLogical + 0.5)) + L" px";
        t += L"\r\nDetail: " + widenAscii(i->reason) + L"\r\n";
    }
    const auto it = d.apps.find(lowerExe(r.exe));
    t += L"Your setting: ";
    if (it == d.apps.end()) t += L"none (default behaviour)";
    else {
        if (it->second.enabled) t += *it->second.enabled ? L"turned on" : L"turned off";
        if (it->second.heightLogical) t += L"   custom strip height " + std::to_wstring(int(*it->second.heightLogical)) + L" px";
    }
    t += L"\r\nDiagnostics: one 'compat:' line per window in MissionControl.log (" + log::directory() + L").";
    return t;
}

std::wstring SettingsWindow::selectedExe() const {
    const int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (sel < 0) return {};
    LVITEMW it{LVIF_PARAM};
    it.iItem = sel;
    ListView_GetItem(list_, &it);
    return size_t(it.lParam) < rows_.size() ? rows_[size_t(it.lParam)].exe : std::wstring();
}

void SettingsWindow::populate(const std::wstring& keepExe) {
    populating_ = true;
    apps_ = host_.detected ? host_.detected() : std::vector<AppInfo>{};
    const RegistryData& d = host_.user();
    rows_.clear();
    ListView_DeleteAllItems(list_);
    auto infoOf = [&](const std::wstring& exe) -> const AppInfo* {
        for (const auto& a : apps_)
            if (a.exe == exe) return &a;
        return nullptr;
    };
    std::vector<std::wstring> exes;
    if (tabIndex_ == TabApps) {
        for (const auto& a : apps_) exes.push_back(a.exe);
        for (const auto& [exe, s] : d.apps)  // apps the user set up but that are not running now
            if (std::find(exes.begin(), exes.end(), exe) == exes.end()) exes.push_back(exe);
    } else if (tabIndex_ == TabCustom) {
        for (const auto& [exe, s] : d.apps)
            if (s.enabled && *s.enabled) exes.push_back(exe);
    } else if (tabIndex_ == TabExcluded) {
        for (const auto& [exe, s] : d.apps)
            if (s.enabled && !*s.enabled) exes.push_back(exe);
        for (const auto& a : apps_)  // system processes are always excluded: shown, not editable
            if (a.known && a.verdict == Verdict::Excluded && std::find(exes.begin(), exes.end(), a.exe) == exes.end()) exes.push_back(a.exe);
    }
    // Order: covered first, then by name.
    std::vector<Row> rows;
    for (const auto& e : exes) rows.push_back({e, infoOf(e)});
    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const bool ca = effectiveOn(a), cb = effectiveOn(b);
        if (ca != cb) return ca;
        return _wcsicmp(a.exe.c_str(), b.exe.c_str()) < 0;
    });
    rows_ = std::move(rows);
    const bool checks = tabIndex_ == TabApps || tabIndex_ == TabCustom;
    ListView_SetExtendedListViewStyleEx(list_, LVS_EX_CHECKBOXES, checks ? LVS_EX_CHECKBOXES : 0);
    int selectIndex = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& r = rows_[i];
        const std::wstring name = displayName(r.exe, r.info ? r.info->path : L"");
        LVITEMW it{LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE};
        it.iItem = int(i);
        it.pszText = const_cast<wchar_t*>(name.c_str());
        it.lParam = LPARAM(i);
        it.iImage = iconIndex(r.exe, r.info ? r.info->path : L"");
        const int row = ListView_InsertItem(list_, &it);
        ListView_SetItemText(list_, row, 1, const_cast<wchar_t*>(r.exe.c_str()));
        const std::wstring st = statusOf(r);
        ListView_SetItemText(list_, row, 2, const_cast<wchar_t*>(st.c_str()));
        if (checks) ListView_SetCheckState(list_, row, effectiveOn(r));
        if (!keepExe.empty() && r.exe == keepExe) selectIndex = row;
    }
    if (selectIndex >= 0) ListView_SetItemState(list_, selectIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    populating_ = false;
    // General page state
    Button_SetCheck(chkEnable_, host_.running && host_.running() ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(radListed_, d.mode == CoverMode::ListedApps ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(radAll_, d.mode == CoverMode::AllCompatible ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(radVerified_, d.mode == CoverMode::VerifiedOnly ? BST_CHECKED : BST_UNCHECKED);
    if (rows_.empty() && tabIndex_ != TabGeneral) {
        SetWindowTextW(details_, tabIndex_ == TabApps ? (host_.running && host_.running() ? L"No application detected yet. Applications appear here when one of their windows is seen."
                                                                                           : L"Traffic lights are off, so applications are not being detected. Turn them on in the General tab.")
                                 : tabIndex_ == TabCustom ? L"No custom applications. Add an application to cover it even when the default mode would not."
                                                          : L"No excluded applications. Add an application to keep its native buttons.");
    } else {
        onSelection();
    }
}

void SettingsWindow::onSelection() {
    if (populating_) return;
    const std::wstring exe = selectedExe();
    const Row* row = nullptr;
    for (const auto& r : rows_)
        if (r.exe == exe) row = &r;
    if (row) SetWindowTextW(details_, detailsOf(*row).c_str());
    else if (!rows_.empty()) SetWindowTextW(details_, L"Select an application to see its details.");
    EnableWindow(btnRemove_, row != nullptr);
    EnableWindow(btnReset_, row != nullptr && host_.user().apps.count(lowerExe(exe)) > 0);
}

void SettingsWindow::applyChange() {
    if (host_.changed) host_.changed();
}

void SettingsWindow::onToggled(int index, bool on) {
    if (populating_ || index < 0) return;
    LVITEMW it{LVIF_PARAM};
    it.iItem = index;
    ListView_GetItem(list_, &it);
    if (size_t(it.lParam) >= rows_.size()) return;
    const Row r = rows_[size_t(it.lParam)];
    if (on && r.info && r.info->known && !decide([&] { Assessment a; a.verdict = r.info->verdict; a.source = r.info->source; return a; }(), Policy{}, CoverMode::AllCompatible).cover &&
        !isCustomApp(host_.user(), r.exe)) {
        MessageBoxW(hwnd_, (r.exe + L" cannot be covered: " + statusOf(r) + L".").c_str(), L"Traffic lights", MB_OK | MB_ICONINFORMATION);
        populate(r.exe);
        return;
    }
    setAppOn(host_.user(), r.exe, on, coversByDefault(r.exe, r.info));
    applyChange();
    populate(r.exe);
}

void SettingsWindow::addFromBrowse() {
    wchar_t file[MAX_PATH * 2] = L"";
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Applications (*.exe)\0*.exe\0All files\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(std::size(file));
    ofn.lpstrTitle = tabIndex_ == TabExcluded ? L"Choose an application to keep its native buttons" : L"Choose an application to cover";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&ofn)) return;
    std::wstring path = file;
    const std::wstring exe = lowerExe(path.substr(path.find_last_of(L"\\/") + 1));
    if (tabIndex_ == TabCustom) {
        // Check before allowing: an app seen running with no usable buttons is refused; an app not running can only be
        // checked per window later (the engine never forces an unsupported window).
        const auto it = std::find_if(apps_.begin(), apps_.end(), [&](const AppInfo& a) { return a.exe == exe; });
        if (it != apps_.end() && it->known) {
            Assessment a;
            a.verdict = it->verdict;
            a.source = it->source;
            if (!decide(a, Policy{}, CoverMode::AllCompatible).cover) {
                MessageBoxW(hwnd_, (exe + L" is not compatible: " + widenAscii(it->reason) + L".\nIts native buttons will stay.").c_str(), L"Traffic lights",
                            MB_OK | MB_ICONWARNING);
                return;
            }
        } else if (MessageBoxW(hwnd_, (exe + L" is not running, so its windows cannot be checked now.\nEvery window is checked when it appears and only "
                                                L"windows whose buttons can be located are covered.\n\nAdd it anyway?").c_str(),
                               L"Traffic lights", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        setAppOn(host_.user(), exe, true, false);
        nameCache_[exe] = fileDescription(path);
    } else {
        setAppOn(host_.user(), exe, false, false);
    }
    applyChange();
    populate(exe);
}

void SettingsWindow::onCommand(int id) {
    switch (id) {
    case IDC_ENABLE:
        if (host_.setRunning) host_.setRunning(Button_GetCheck(chkEnable_) == BST_CHECKED);
        refresh();
        break;
    case IDC_RAD_LISTED:
    case IDC_RAD_ALL:
    case IDC_RAD_VER:
        host_.user().mode = id == IDC_RAD_VER ? CoverMode::VerifiedOnly : id == IDC_RAD_ALL ? CoverMode::AllCompatible : CoverMode::ListedApps;
        applyChange();
        break;
    case IDC_ADD: addFromBrowse(); break;
    case IDC_REMOVE:
    case IDC_RESET: {
        const std::wstring exe = selectedExe();
        if (exe.empty()) break;
        resetApp(host_.user(), exe);
        applyChange();
        populate();
        break;
    }
    case IDC_REFRESH: refresh(); break;
    case IDC_CLOSE:
    case IDCANCEL: close(); break;
    }
}

void SettingsWindow::refresh() {
    if (list_) populate(selectedExe());
}

LRESULT SettingsWindow::onMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        create();
        layout();
        populate();
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {S(640), S(520)};
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = int(HIWORD(wp));
        NONCLIENTMETRICSW ncm{sizeof ncm};
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, UINT(dpi_));
        HFONT nf = CreateFontIndirectW(&ncm.lfMessageFont);
        for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(nf), TRUE);
        if (font_) DeleteObject(font_);
        font_ = nf;
        const RECT* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED || HIWORD(wp) == 0) onCommand(LOWORD(wp));
        return 0;
    case WM_NOTIFY: {
        const NMHDR* h = reinterpret_cast<NMHDR*>(lp);
        if (h->idFrom == IDC_TAB && h->code == TCN_SELCHANGE) {
            tabIndex_ = TabCtrl_GetCurSel(tab_);
            layout();
            populate();
        } else if (h->idFrom == IDC_LIST && h->code == LVN_ITEMCHANGED) {
            const auto* n = reinterpret_cast<const NMLISTVIEW*>(lp);
            if (n->uChanged & LVIF_STATE) {
                if ((n->uNewState ^ n->uOldState) & LVIS_STATEIMAGEMASK) onToggled(n->iItem, ((n->uNewState & LVIS_STATEIMAGEMASK) >> 12) == 2);
                else if ((n->uNewState ^ n->uOldState) & LVIS_SELECTED) onSelection();
            }
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (images_) { ImageList_Destroy(images_); images_ = nullptr; }
        iconCache_.clear();
        nameCache_.clear();
        rows_.clear();
        apps_.clear();
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        hwnd_ = tab_ = list_ = details_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace mc
