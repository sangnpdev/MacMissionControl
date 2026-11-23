#pragma once
#include <windows.h>

#include <string>

namespace mc::log {

// Opens %LOCALAPPDATA%\MissionControl\mc.log (truncated per run). Safe to call write() before init().
void init();
void shutdown();
void write(const char* fmt, ...);
// Logs a failed HRESULT with context and returns `false` when it failed, `true` otherwise.
bool check(HRESULT hr, const char* what);
// Logs GetLastError() with context.
void lastError(const char* what);
std::wstring directory();

}  // namespace mc::log
