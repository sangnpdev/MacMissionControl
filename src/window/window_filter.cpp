#include "window/window_filter.h"

#include <algorithm>

namespace mc {

bool includeWindow(const WindowFacts& f, const WindowFilterRules& rules) {
    if (!f.visible || f.cloaked || f.ownProcess || !f.onCurrentDesktop) return false;
    if (f.noActivate && !f.appWindow) return false;
    if ((f.toolWindow || f.hasOwner) && !f.appWindow) return false;
    if (f.width < rules.minWidth || f.height < rules.minHeight) return false;
    if (rules.requireTitle && f.title.empty()) return false;
    auto has = [&](const std::vector<std::wstring>& v) { return std::find(v.begin(), v.end(), f.className) != v.end(); };
    if (has(rules.denyClasses)) return false;
    if (!rules.allowClasses.empty() && !has(rules.allowClasses)) return false;
    return true;
}

}  // namespace mc
