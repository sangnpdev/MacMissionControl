#pragma once
// Minimal self-contained test runner (see ADR-001 for why not GoogleTest yet).
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace mctest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };
inline void fail(const char* file, int line, const std::string& msg) {
    std::printf("  FAIL %s:%d: %s\n", file, line, msg.c_str());
    ++failures();
}
}  // namespace mctest

#define MC_CAT2(a, b) a##b
#define MC_CAT(a, b) MC_CAT2(a, b)
#define TEST(name) \
    static void name(); \
    static mctest::Reg MC_CAT(reg_, name)(#name, name); \
    static void name()
#define CHECK(cond) do { if (!(cond)) mctest::fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (!(std::abs(_a - _b) <= (eps))) \
    mctest::fail(__FILE__, __LINE__, std::string(#a " ~ " #b ": ") + std::to_string(_a) + " vs " + std::to_string(_b)); } while (0)
