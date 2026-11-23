#include "diagnostics/log.h"

#include <share.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace mc::log {

namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
}  // namespace

std::wstring directory() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    return std::wstring(buf) + L"\\MissionControl";
}

void init() {
    const std::wstring dir = directory();
    CreateDirectoryW(dir.c_str(), nullptr);  // fails harmlessly if it exists
    std::lock_guard lock(g_mutex);
    // Others may read the log while the app runs (diagnostics, smoke tests); only writing is denied.
    if (!g_file) g_file = _wfsopen((dir + L"\\mc.log").c_str(), L"w", _SH_DENYWR);
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void write(const char* fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof msg, fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1100];
    snprintf(line, sizeof line, "%02d:%02d:%02d.%03d [%5lu] %s\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, GetCurrentThreadId(), msg);
    OutputDebugStringA(line);
    std::lock_guard lock(g_mutex);
    if (g_file) { fputs(line, g_file); fflush(g_file); }
}

bool check(HRESULT hr, const char* what) {
    if (SUCCEEDED(hr)) return true;
    write("ERROR %s failed: hr=0x%08lX", what, static_cast<unsigned long>(hr));
    return false;
}

void lastError(const char* what) { write("ERROR %s failed: GetLastError=%lu", what, GetLastError()); }

}  // namespace mc::log
