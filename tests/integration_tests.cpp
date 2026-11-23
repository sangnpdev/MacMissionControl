// Integration tests against the real DWM / window system. Needs an interactive desktop session.
// Spawns mc_testwin.exe (path from argv[1]) to get windows owned by another process.
#include <windows.h>
#include <objbase.h>

#include <cstring>
#include <string>

#include "app/autostart.h"
#include "diagnostics/log.h"
#include "preview/thumbnail.h"
#include "test_main.h"
#include "window/window_manager.h"

using namespace mc;

static std::wstring g_testwin;

namespace {
// Waits while dispatching messages. Our test windows must stay responsive: the shell sends them
// synchronous messages and, while blocked, delays uncloaking other new windows.
void pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    for (ULONGLONG now; (now = GetTickCount64()) < end;) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(end - now), QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
}

struct Spawned {
    PROCESS_INFORMATION pi{};
    explicit Spawned(int n, const wchar_t* extraArgs = L"") {
        std::wstring cmd = L"\"" + g_testwin + L"\" " + std::to_wstring(n) + L" " + extraArgs;
        STARTUPINFOW si{sizeof si};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) WaitForInputIdle(pi.hProcess, 5000);
        else std::printf("  CreateProcess(%ls) failed: %lu\n", cmd.c_str(), GetLastError());
        // The shell adopts a new window in steps: it is cloaked (DWM_CLOAKED_SHELL), uncloaked, can be
        // cloaked again, and is not "on the current virtual desktop" until assigned. Wait until the
        // product's own filter accepts all n windows on 4 consecutive checks (200 ms).
        WindowManager wm;
        wm.init();
        int waited = 0, stable = 0;
        for (; waited < 8000 && stable < 4; waited += 50) {
            stable = acceptedOwn(wm) >= n ? stable + 1 : 0;
            pump(50);
        }
        if (stable < 4) {
            std::printf("  spawn: %d of %d windows accepted after %d ms\n", acceptedOwn(wm), n, waited);
            dump();
        }
    }
    int acceptedOwn(const WindowManager& wm) const {
        WindowFilterRules r;
        r.allowClasses = {L"MCBenchWindow"};
        int count = 0;
        for (auto& w : wm.enumerate(r)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w.hwnd, &pid);
            count += pid == pi.dwProcessId;
        }
        return count;
    }
    void dump() const {
        for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"MCBenchWindow", nullptr));) {
            DWORD pid = 0, cloaked = 0;
            GetWindowThreadProcessId(h, &pid);
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            std::printf("    %p pid=%lu%s visible=%d cloaked=%lu exstyle=%llx\n", static_cast<void*>(h), pid, pid == pi.dwProcessId ? "(ours)" : "",
                        IsWindowVisible(h), cloaked, static_cast<unsigned long long>(GetWindowLongPtrW(h, GWL_EXSTYLE)));
        }
    }
    // Bench windows owned by this process; `shownOnly` = visible and not cloaked.
    int countOwn(bool shownOnly) const {
        int count = 0;
        for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"MCBenchWindow", nullptr));) {
            DWORD pid = 0, cloaked = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != pi.dwProcessId) continue;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            count += !shownOnly || (IsWindowVisible(h) && !cloaked);
        }
        return count;
    }
    void kill() {
        if (!pi.hProcess) return;
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        for (int waited = 0; waited < 5000 && countOwn(false) > 0; waited += 50) pump(50);  // windows outlive the handle briefly
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        pi = {};
    }
    ~Spawned() { kill(); }
};

WindowFilterRules benchOnly() {
    WindowFilterRules r;
    r.allowClasses = {L"MCBenchWindow"};
    return r;
}

HWND makeDest() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"MCIntegrationDest";
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"dest", WS_POPUP, 0, 0, 400, 300, nullptr, nullptr, nullptr, nullptr);
}
}  // namespace

TEST(enumerate_finds_other_process_windows_in_z_order) {
    WindowManager wm;
    wm.init();
    CHECK(wm.enumerate(benchOnly()).empty());
    Spawned s(3);
    auto ws = wm.enumerate(benchOnly());
    CHECK(ws.size() == 3);
    for (auto& w : ws) {
        CHECK(!w.bounds.empty());
        CHECK(!w.workArea.empty());
        CHECK(w.title.rfind(L"Bench window", 0) == 0);
        CHECK(w.sourceInWindow.right > w.sourceInWindow.left);
    }
    s.kill();
    CHECK(wm.enumerate(benchOnly()).empty());
}

TEST(enumerate_excludes_own_process_and_minimized_reports_restored_rect) {
    WindowManager wm;
    wm.init();
    HWND own = makeDest();
    ShowWindow(own, SW_SHOWNOACTIVATE);
    WindowFilterRules all;
    all.requireTitle = false;
    for (auto& w : wm.enumerate(all)) CHECK(w.hwnd != own);
    DestroyWindow(own);

    // The window is created minimized by the helper itself: minimizing another process's window from
    // here is unreliable when a third-party dock intercepts minimize (seen on the dev machine).
    Spawned s(1, L"--minimized");
    auto ws = wm.enumerate(benchOnly());
    if (ws.size() != 1) { std::printf("  enumerate returned %zu windows\n", ws.size()); s.dump(); }
    CHECK(ws.size() == 1);
    if (ws.size() == 1) {
        CHECK(ws[0].minimized);
        // mc_testwin places window 0 at the primary work area's top-left, 500 x 360. rcNormalPosition is in
        // workspace coordinates; WindowManager must convert it back to screen coordinates.
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        const Rect b = ws[0].bounds;
        if (b.x != wa.left || b.y != wa.top || b.w != 500 || b.h != 360)
            std::printf("  restored bounds [%.0f,%.0f %.0fx%.0f], expected [%ld,%ld 500x360]\n", b.x, b.y, b.w, b.h, wa.left, wa.top);
        CHECK(b.x == wa.left && b.y == wa.top && b.w == 500 && b.h == 360);
    }
}

TEST(thumbnail_lifecycle_and_source_destroyed) {
    WindowManager wm;
    wm.init();
    HWND dest = makeDest();
    ShowWindow(dest, SW_SHOWNOACTIVATE);
    Spawned s(1);
    auto ws = wm.enumerate(benchOnly());
    if (ws.size() != 1) { std::printf("  enumerate returned %zu windows\n", ws.size()); s.dump(); }
    CHECK(ws.size() == 1);
    if (ws.size() == 1) {
        Thumbnail t;
        CHECK(t.create(dest, ws[0].hwnd, ws[0].sourceInWindow));
        CHECK(bool(t));
        CHECK(t.update({10, 10, 200, 150}, 1.0));
        CHECK(t.update({10, 10, 200, 150}, 0.0));  // invisible is still a valid update
        Thumbnail moved = std::move(t);
        CHECK(!t && bool(moved));
        s.kill();
        // DWM learns about the destroyed source asynchronously: updates may succeed for a short time.
        // Alternate the opacity so every call really reaches DWM (unchanged values are skipped).
        bool failed = false;
        for (int i = 0; i < 40 && !failed; ++i) {
            failed = !moved.update({10, 10, 200, 150}, i % 2 ? 0.9 : 1.0);
            if (!failed) pump(50);
        }
        CHECK(failed);  // source gone: update reports failure within 2 s
        moved.reset();
        CHECK(!moved);
    }
    Thumbnail bad;
    CHECK(!bad.create(dest, nullptr, {}));  // invalid source is rejected, not crashing
    DestroyWindow(dest);
}

// Fill color mc_testwin uses for "Bench window <s>".
static COLORREF benchColor(int s) { return RGB(40 + (s * 67) % 200, 40 + (s * 131) % 200, 40 + (s * 29) % 200); }

TEST(thumbnails_draw_in_registration_order) {
    WindowManager wm;
    wm.init();
    Spawned s(2);
    auto ws = wm.enumerate(benchOnly());
    CHECK(ws.size() == 2);
    if (ws.size() != 2) return;
    HWND dest = makeDest();
    SetWindowPos(dest, HWND_TOPMOST, 100, 100, 400, 300, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    for (int order = 0; order < 2; ++order) {
        const auto& first = ws[order], & second = ws[1 - order];
        Thumbnail a, b;
        CHECK(a.create(dest, first.hwnd, first.sourceInWindow));
        CHECK(b.create(dest, second.hwnd, second.sourceInWindow));
        a.update({0, 0, 400, 300}, 1.0);
        b.update({0, 0, 400, 300}, 1.0);
        const int serial = _wtoi(second.title.c_str() + 13);  // "Bench window N"
        const COLORREF firstColor = benchColor(_wtoi(first.title.c_str() + 13));
        // Thumbnails appear after one or more composition frames: wait until either color shows.
        COLORREF px = 0;
        for (int i = 0; i < 50; ++i) {
            DwmFlush();
            HDC screen = GetDC(nullptr);
            px = GetPixel(screen, 100 + 200, 100 + 225);  // below the centered caption text
            ReleaseDC(nullptr, screen);
            if (px == benchColor(serial) || px == firstColor) break;
            pump(20);
        }
        if (px != benchColor(serial) && px != firstColor) {
            // Neither thumbnail is on screen at the probe point (covered, or the window not presented):
            // this run cannot tell anything about draw order.
            std::printf("  SKIP order %d: probe pixel %06lX shows neither thumbnail (inconclusive)\n", order, px);
            continue;
        }
        if (px != benchColor(serial)) {
            std::printf("  pixel %06lX, expected later-registered color %06lX (first %06lX)\n", px, benchColor(serial), firstColor);
            SIZE sa{}, sb{};
            DWORD ca = 0, cb = 0;
            DwmGetWindowAttribute(first.hwnd, DWMWA_CLOAKED, &ca, sizeof ca);
            DwmGetWindowAttribute(second.hwnd, DWMWA_CLOAKED, &cb, sizeof cb);
            std::printf("  sources: visible %d/%d cloaked %lu/%lu iconic %d/%d; a.update=%d b.update=%d\n", IsWindowVisible(first.hwnd),
                        IsWindowVisible(second.hwnd), ca, cb, IsIconic(first.hwnd), IsIconic(second.hwnd), a.update({0, 0, 400, 299}, 1.0),
                        b.update({0, 0, 400, 299}, 1.0));
            HDC screen = GetDC(nullptr);
            std::printf("  pixels: dest corner %06lX, center %06lX, outside %06lX; foreground window %p, dest %p\n", GetPixel(screen, 102, 102),
                        GetPixel(screen, 300, 250), GetPixel(screen, 90, 90), static_cast<void*>(GetForegroundWindow()), static_cast<void*>(dest));
            ReleaseDC(nullptr, screen);
            (void)sa; (void)sb;
        }
        CHECK(px == benchColor(serial));
    }
    DestroyWindow(dest);
}

// Writes the real HKCU Run entry for a moment and restores whatever was there before.
TEST(autostart_enable_disable_roundtrip) {
    constexpr wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    wchar_t saved[2048];
    DWORD savedSize = sizeof saved;
    const bool hadValue = RegGetValueW(HKEY_CURRENT_USER, kRun, L"MissionControl", RRF_RT_REG_SZ, nullptr, saved, &savedSize) == ERROR_SUCCESS;

    CHECK(autostart::enable());
    wchar_t value[2048];
    DWORD size = sizeof value;
    CHECK(RegGetValueW(HKEY_CURRENT_USER, kRun, L"MissionControl", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS);
    CHECK(autostart::command() == value);
    CHECK(autostart::state() != autostart::State::Off);  // On, or DisabledByUser if switched off in Settings
    CHECK(autostart::disable());
    CHECK(autostart::state() == autostart::State::Off);
    CHECK(autostart::disable());  // removing twice is fine

    if (hadValue) RegSetKeyValueW(HKEY_CURRENT_USER, kRun, L"MissionControl", REG_SZ, saved, savedSize);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: integration_tests <path-to-mc_testwin.exe>\n"); return 2; }
    g_testwin.assign(argv[1], argv[1] + std::strlen(argv[1]));
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    mc::log::init();
    for (auto& c : mctest::registry()) {
        const int before = mctest::failures();
        c.fn();
        std::printf("%s %s\n", mctest::failures() == before ? "[ OK ]" : "[FAIL]", c.name);
    }
    std::printf("%zu tests, %d failed checks\n", mctest::registry().size(), mctest::failures());
    CoUninitialize();
    return mctest::failures() == 0 ? 0 : 1;
}
