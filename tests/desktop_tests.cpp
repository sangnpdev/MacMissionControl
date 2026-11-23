// Live virtual-desktop tests. They CHANGE the user's session for a few seconds: they create one
// desktop, switch between it and the original, and remove only the desktop they created.
// Opt-in: run `desktop_tests.exe` from an interactive session (not part of the default ctest run).
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>

#include "desktop/virtual_desktop_provider.h"
#include "diagnostics/log.h"
#include "test_main.h"

using namespace mc;

namespace {

void pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    for (ULONGLONG now; (now = GetTickCount64()) < end;) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, DWORD(end - now), QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
}

// Waits (event-driven) until `pred(state)` holds. Returns elapsed ms, or -1 on timeout.
template <class Pred>
int waitFor(VirtualDesktopProvider& p, Pred pred, DWORD timeoutMs = 4000) {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (auto s = p.read(); s && pred(*s)) return int(GetTickCount64() - start);
        const ULONGLONG now = GetTickCount64();
        if (now - start >= timeoutMs) return -1;
        HANDLE ev = p.changeEvent();
        if (MsgWaitForMultipleObjects(1, &ev, FALSE, DWORD(timeoutMs - (now - start)), QS_ALLINPUT) == WAIT_OBJECT_0) p.rearm();
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
    }
}

// Switches one press at a time, waiting for each change to land.
bool switchTo(VirtualDesktopProvider& p, const DesktopId& target) {
    for (int guard = 0; guard < 20; ++guard) {
        auto s = p.read();
        if (!s) return false;
        const int to = s->indexOf(target);
        const int steps = switchSteps(s->current, to);
        if (to < 0) return false;
        if (steps == 0) return true;
        const DesktopId before = s->desktops[s->current].id;
        if (!p.step(steps > 0 ? 1 : -1)) return false;
        const int ms = waitFor(p, [&](const DesktopState& st) { return st.current >= 0 && !(st.desktops[st.current].id == before); });
        std::printf("  step %+d took %d ms\n", steps > 0 ? 1 : -1, ms);
        if (ms < 0) return false;
        pump(250);  // let the shell finish its own switch animation before the next press
    }
    return false;
}

HWND makeToolWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = L"MCDesktopTestWindow";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"", WS_POPUP, 0, 0, 50, 50, nullptr, nullptr, nullptr, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

DWORD cloakedOf(HWND h) {
    DWORD c = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof c);
    return c;
}

}  // namespace

TEST(desktop_create_switch_move_remove) {
    VirtualDesktopProvider p;
    CHECK(p.init());
    auto original = p.read();
    CHECK(original.has_value());
    if (!original) return;
    const std::size_t count = original->desktops.size();
    std::printf("  start: %zu desktop(s), current %d\n", count, original->current);
    CHECK(original->current >= 0);
    if (original->current < 0) return;
    const DesktopId home = original->desktops[original->current].id;

    // Create: the shell adds a desktop and switches to it.
    CHECK(p.create());
    DesktopId created{};
    const int ms = waitFor(p, [&](const DesktopState& s) {
        if (s.desktops.size() != count + 1 || s.current < 0) return false;
        created = s.desktops[s.current].id;
        return original->indexOf(created) < 0;
    });
    std::printf("  create -> new current desktop after %d ms\n", ms);
    CHECK(ms >= 0);
    if (ms < 0) return;
    pump(400);

    // An own tool window (like the overlay) on the new desktop.
    HWND tool = makeToolWindow();
    pump(300);
    std::printf("  tool window on new desktop: cloaked=%lu\n", cloakedOf(tool));

    // Switch back home. A WS_EX_TOOLWINDOW window is shown on every desktop, so the overview overlay
    // (also a tool window) stays visible while the overview switches desktops.
    CHECK(switchTo(p, home));
    pump(300);
    std::printf("  after switching home: tool window cloaked=%lu\n", cloakedOf(tool));
    CHECK(cloakedOf(tool) == 0);
    DestroyWindow(tool);

    // Remove only the desktop this test created: switch to it, verify identity, Ctrl+Win+F4.
    CHECK(switchTo(p, created));
    auto s = p.read();
    const bool onCreated = s && s->current >= 0 && s->desktops[s->current].id == created;
    CHECK(onCreated);
    if (onCreated) {
        CHECK(p.removeCurrent());
        const int rms = waitFor(p, [&](const DesktopState& st) { return st.desktops.size() == count && st.indexOf(created) < 0; });
        std::printf("  remove -> desktop gone after %d ms\n", rms);
        CHECK(rms >= 0);
        pump(400);
    } else {
        std::printf("  NOT removing: current desktop is not the one this test created\n");
    }
    CHECK(switchTo(p, home));
    auto end = p.read();
    CHECK(end && end->desktops.size() == count && end->current >= 0 && end->desktops[end->current].id == home);
}

int main() {
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
