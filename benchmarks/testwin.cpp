// Benchmark helper: opens N plain top-level windows of class MCBenchWindow.
// Usage: mc_testwin.exe N [--churn MS] [--minimized]
//   --churn MS   every MS milliseconds, close the oldest window and open a new one (stress test).
//   --minimized  create the windows minimized (window N's restored rect is still the computed one).
// Runs until all windows are closed or the process is killed.
#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <string>

namespace {
HINSTANCE g_inst;
int g_serial = 0, g_alive = 0;
bool g_minimized = false;
std::deque<HWND> g_windows;

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        const int s = int(GetWindowLongPtrW(h, GWLP_USERDATA));
        HBRUSH b = CreateSolidBrush(RGB(40 + (s * 67) % 200, 40 + (s * 131) % 200, 40 + (s * 29) % 200));
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        const std::wstring t = L"Bench window " + std::to_wstring(s);
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, t.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (--g_alive == 0) PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void spawn() {
    const int s = g_serial++;
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = 500 + (s % 4) * 160, h = 360 + (s % 3) * 140;
    const int x = wa.left + (s * 53) % std::max(1L, wa.right - wa.left - w);
    const int y = wa.top + (s * 37) % std::max(1L, wa.bottom - wa.top - h);
    const std::wstring title = L"Bench window " + std::to_wstring(s);
    HWND hwnd = CreateWindowExW(0, L"MCBenchWindow", title.c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, g_inst, nullptr);
    if (!hwnd) return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, s);
    ShowWindow(hwnd, g_minimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
    g_windows.push_back(hwnd);
    ++g_alive;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    g_inst = inst;
    const int n = __argc > 1 ? _wtoi(__wargv[1]) : 5;
    int churn = 0;
    for (int i = 2; i < __argc; ++i) {
        const std::wstring a = __wargv[i];
        if (a == L"--churn" && i + 1 < __argc) churn = _wtoi(__wargv[++i]);
        else if (a == L"--minimized") g_minimized = true;
    }
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MCBenchWindow";
    RegisterClassExW(&wc);
    for (int i = 0; i < n; ++i) spawn();
    const UINT_PTR churnTimer = churn > 0 ? SetTimer(nullptr, 0, churn, nullptr) : 0;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Match our timer id: the system also posts thread-level WM_TIMER messages to this thread.
        if (churnTimer && msg.message == WM_TIMER && !msg.hwnd && msg.wParam == churnTimer) {
            spawn();  // spawn first so g_alive never hits 0
            if (!g_windows.empty()) { DestroyWindow(g_windows.front()); g_windows.pop_front(); }
            continue;
        }
        DispatchMessageW(&msg);
    }
    return 0;
}
