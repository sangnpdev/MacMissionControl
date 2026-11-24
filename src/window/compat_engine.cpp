#include "window/compat_engine.h"

#include <algorithm>
#include <cwctype>

#include "window/titlebar_profile.h"

namespace mc {

namespace {
std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}
}  // namespace

bool isSystemShellProcess(const std::wstring& exeIn) {
    static const wchar_t* deny[] = {L"dwm.exe",         L"logonui.exe",         L"consent.exe",           L"lockapp.exe",
                                    L"searchhost.exe",  L"startmenuexperiencehost.exe", L"shellexperiencehost.exe",
                                    L"textinputhost.exe", L"winlogon.exe",     L"csrss.exe",             L"fontdrvhost.exe"};
    const std::wstring exe = lower(exeIn);
    return std::any_of(std::begin(deny), std::end(deny), [&](const wchar_t* d) { return exe == d; });
}

Assessment assess(const TitleBarFacts& f, const Probe& p) {
    Assessment a;
    const std::wstring exe = lower(f.exeName);
    if (exe.empty()) { a.reason = "unknown process"; return a; }
    if (isSystemShellProcess(exe)) { a.verdict = Verdict::Excluded; a.reason = "system shell process"; return a; }
    if (!f.visible || f.cloaked) { a.reason = "not visible"; return a; }
    if (!f.hasCaption) { a.reason = "no caption (borderless or fullscreen)"; return a; }

    // 1. A profile measured for this app wins over anything the probe says.
    const StripProfile prof = stripProfileFor(f, {}, {});
    if (prof.coverable) {
        a.verdict = Verdict::Supported;
        a.source = StripSource::Verified;
        a.heightLogical = prof.heightLogical;
        a.useFixed = prof.fixedStrip;
        a.fixedLeftFromRight = prof.leftFromRight;
        a.fixedRightGap = prof.rightGap;
        a.reason = prof.why;
        return a;
    }
    // 2. Generic: combine the two sources.
    if (p.dwm.ok && p.hit.ok) {
        a.verdict = Verdict::Supported;
        a.source = StripSource::Combined;
        // Taller hit-test buttons than the DWM bounds (Chromium: 40 vs 30): cover the taller of the two.
        a.heightLogical = p.hit.height > p.dwm.height * 1.1 ? p.hit.height : 0.0;
        a.reason = "DWM bounds and hit-test both found";
    } else if (p.hit.ok) {
        a.verdict = Verdict::Supported;
        a.source = StripSource::HitTest;
        a.useHit = true;
        a.hitLeftFromRight = p.hit.leftFromRight;
        a.hitHeight = p.hit.height;
        a.hitTop = p.hit.top;
        a.reason = "buttons located by hit-test (Windows reports no caption bounds)";
    } else if (p.dwm.ok && f.hasSysMenu) {
        a.verdict = Verdict::Partial;
        a.source = StripSource::DwmBounds;
        a.reason = "only DWM bounds known (exact for Windows-drawn frames, unverified for this app)";
    } else {
        a.reason = "no way to locate the caption buttons";
    }
    return a;
}

}  // namespace mc
