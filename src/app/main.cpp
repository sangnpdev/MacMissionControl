#include <windows.h>
#include <dbghelp.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app/app.h"
#include "app/autostart.h"
#include "app/overlay_window.h"
#include "diagnostics/log.h"

namespace {

// Unhandled exception: write a minidump next to the log so a crash can be analysed later. The
// keyboard hook dies with the process, so Win+Tab returns to Task View on its own.
LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"\\crash-%04d%02d%02d-%02d%02d%02d.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    const std::wstring path = mc::log::directory() + name;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(f);
    }
    mc::log::write("CRASH: exception 0x%08lX at %p, dump %ls", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
                   path.c_str());
    return EXCEPTION_EXECUTE_HANDLER;  // terminate
}

// Logs whether Win+Tab can be claimed with RegisterHotKey, then releases it immediately.
int probeHotkeys() {
    struct Probe { const char* name; UINT mods; UINT vk; };
    const Probe probes[] = {{"Win+Tab", MOD_WIN, VK_TAB}, {"Ctrl+Alt+Space", MOD_CONTROL | MOD_ALT, VK_SPACE}};
    int id = 100;
    for (auto& p : probes) {
        if (RegisterHotKey(nullptr, id, p.mods | MOD_NOREPEAT, p.vk)) {
            mc::log::write("probe: RegisterHotKey(%s) succeeded (released again)", p.name);
            UnregisterHotKey(nullptr, id);
        } else {
            mc::log::write("probe: RegisterHotKey(%s) failed, GetLastError=%lu", p.name, GetLastError());
        }
        ++id;
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetUnhandledExceptionFilter(&onCrash);
    mc::log::init();
    mc::log::write("Mission Control %s", MC_VERSION);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    mc::Options opt;
    bool probe = false, quit = false;
    WPARAM desktopCmd = 0;  // --desktop next|prev|new|N, forwarded to the running instance
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--show") opt.show = true;
        else if (a == L"--settings") opt.settings = true;
        else if (a == L"--probe-hotkeys") probe = true;
        else if (a == L"--no-trim") opt.noTrim = true;
        else if (a == L"--no-takeover") opt.noTakeover = true;
        else if (a == L"--hook-accept-injected") opt.hookAcceptInjected = true;
        else if (a == L"--bench-seconds" && i + 1 < argc) opt.benchSeconds = _wtoi(argv[++i]);
        else if (a == L"--crash-test") { volatile int* p = nullptr; *p = 1; }  // verifies the crash handler and hook cleanup
        else if (a == L"--autostart" && i + 1 < argc) {
            // Explicit user request only; also available from the tray menu.
            const std::wstring v = argv[++i];
            const bool ok = v == L"on" ? mc::autostart::enable() : v == L"off" ? mc::autostart::disable() : false;
            if (v != L"on" && v != L"off") mc::log::write("--autostart expects on or off");
            LocalFree(argv);
            mc::log::shutdown();
            return ok ? 0 : 1;
        }
        else if (a == L"--config" && i + 1 < argc) opt.configPath = argv[++i];
        else if (a == L"--quit") quit = true;
        else if (a == L"--desktop" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            desktopCmd = v == L"next" ? mc::App::kDesktopNext : v == L"prev" ? mc::App::kDesktopPrev : v == L"new" ? mc::App::kDesktopNew : WPARAM(_wtoi(v.c_str()));
        }
        else if (a == L"--bench" && i + 1 < argc) opt.benchCycles = _wtoi(argv[++i]);
        else if (a == L"--csv" && i + 1 < argc) opt.benchCsv = argv[++i];
        else if (a == L"--class" && i + 1 < argc) opt.benchClass = argv[++i];
        else mc::log::write("unknown argument ignored: %ls", a.c_str());
    }
    LocalFree(argv);
    if (probe) { const int rc = probeHotkeys(); mc::log::shutdown(); return rc; }
    if (opt.benchCycles > 0 && opt.benchCsv.empty()) { mc::log::write("--bench needs --csv PATH"); return 2; }

    // Single resident instance. A second launch toggles (or quits) the running one.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\MissionControl.PoC");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS && opt.benchCycles == 0) {
        if (HWND other = FindWindowW(mc::kOverlayClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);  // pass our foreground right so the overlay can take focus
            if (quit) PostMessageW(other, WM_CLOSE, 0, 0);
            else if (opt.settings) PostMessageW(other, mc::App::kMsgOpenSettings, 0, 0);
            else if (desktopCmd) PostMessageW(other, mc::App::kMsgDesktopCommand, desktopCmd, 0);
            else PostMessageW(other, WM_HOTKEY, 1, 0);
        }
        CloseHandle(mutex);
        mc::log::shutdown();
        return 0;
    }
    if (quit || desktopCmd) { if (mutex) CloseHandle(mutex); return 0; }  // nothing running to command

    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    mc::log::check(hr, "CoInitializeEx");
    int rc;
    {
        mc::App app;
        rc = app.run(inst, opt);
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    if (mutex) { ReleaseMutex(mutex); CloseHandle(mutex); }
    mc::log::write("exit rc=%d", rc);
    mc::log::shutdown();
    return rc;
}
