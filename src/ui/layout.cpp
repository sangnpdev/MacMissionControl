#include "ui/layout.h"

#include <algorithm>
#include <numeric>

namespace mc {

std::vector<Rect> layoutOverview(const Rect& area, const std::vector<Rect>& windows, const LayoutParams& p) {
    const std::size_t n = windows.size();
    std::vector<Rect> out(n);
    if (n == 0) return out;

    const Rect inner{area.x + p.padding, area.y + p.padding, area.w - 2 * p.padding, area.h - 2 * p.padding};
    if (inner.empty()) return out;

    // Reading order of the original windows: rows by center y, then x.
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Rect &ra = windows[a], &rb = windows[b];
        if (std::abs(ra.cy() - rb.cy()) > 1.0) return ra.cy() < rb.cy();
        return ra.cx() < rb.cx();
    });
    auto sizeOf = [&](std::size_t i) {
        const Rect& r = windows[i];
        return Rect{0, 0, std::max(r.w, 1.0), std::max(r.h, 1.0)};
    };

    double bestScale = -1.0;
    std::size_t bestRows = 1;
    for (std::size_t rows = 1; rows <= n; ++rows) {
        // Split into `rows` rows of near-equal count; first rows get the extra items.
        double maxRowW = 0.0, sumRowH = 0.0;
        std::size_t idx = 0;
        for (std::size_t r = 0; r < rows; ++r) {
            const std::size_t cnt = n / rows + (r < n % rows ? 1 : 0);
            double rowW = 0.0, rowH = 0.0;
            for (std::size_t k = 0; k < cnt; ++k, ++idx) {
                const Rect s = sizeOf(order[idx]);
                rowW += s.w;
                rowH = std::max(rowH, s.h);
            }
            // Spacing does not scale, so subtract it from the available space.
            const double availW = inner.w - p.spacing * static_cast<double>(cnt - 1);
            if (availW <= 0) { maxRowW = 1e300; break; }
            maxRowW = std::max(maxRowW, rowW / availW);
            sumRowH += rowH;
        }
        // Spacing and labels do not scale either.
        const double availH = inner.h - p.spacing * static_cast<double>(rows - 1) - p.labelHeight * static_cast<double>(rows);
        if (availH <= 0 || maxRowW >= 1e300) continue;
        const double scale = std::min({1.0 / maxRowW, availH / sumRowH, p.maxScale});
        if (scale > bestScale) { bestScale = scale; bestRows = rows; }
    }
    if (bestScale <= 0) return out;

    // Place rows, centered horizontally per row and vertically as a block.
    struct Row { std::size_t begin, count; double h, w; };
    std::vector<Row> rows;
    double blockH = 0.0;
    std::size_t idx = 0;
    for (std::size_t r = 0; r < bestRows; ++r) {
        const std::size_t cnt = n / bestRows + (r < n % bestRows ? 1 : 0);
        Row row{idx, cnt, 0.0, p.spacing * static_cast<double>(cnt - 1)};
        for (std::size_t k = 0; k < cnt; ++k) {
            const Rect s = sizeOf(order[idx + k]);
            row.w += s.w * bestScale;
            row.h = std::max(row.h, s.h * bestScale);
        }
        idx += cnt;
        blockH += row.h;
        rows.push_back(row);
    }
    blockH += p.spacing * static_cast<double>(bestRows - 1) + p.labelHeight * static_cast<double>(bestRows);

    double y = inner.y + (inner.h - blockH) * 0.5;
    for (const Row& row : rows) {
        double x = inner.x + (inner.w - row.w) * 0.5;
        for (std::size_t k = 0; k < row.count; ++k) {
            const std::size_t i = order[row.begin + k];
            const Rect s = sizeOf(i);
            const double w = s.w * bestScale, h = s.h * bestScale;
            out[i] = {x, y + (row.h - h) * 0.5, w, h};  // vertically centered in its row
            x += w + p.spacing;
        }
        y += row.h + p.labelHeight + p.spacing;
    }
    return out;
}

StripLayout layoutStrip(const Rect& monitor, int count, double aspect, bool showPlus, const StripParams& p) {
    StripLayout out;
    const double h = std::clamp(monitor.h * p.heightFraction, p.minHeight, p.maxHeight);
    out.band = {monitor.x, monitor.y, monitor.w, h};
    if (count <= 0 || aspect <= 0) return out;
    double tileH = h - 2 * p.padding - p.nameHeight;
    double tileW = tileH * aspect;
    const double plusW = showPlus ? tileH * 0.6 : 0.0;
    const int items = count + (showPlus ? 1 : 0);
    double total = count * tileW + plusW + p.gap * (items - 1);
    const double maxW = monitor.w - 2 * p.padding;
    if (total > maxW) {  // too many desktops: shrink tiles to fit, keep the gaps
        const double k = (maxW - p.gap * (items - 1)) / (total - p.gap * (items - 1));
        tileW *= k;
        tileH *= k;
        total = maxW;
    }
    double x = monitor.cx() - total / 2;
    const double y = monitor.y + p.padding;
    for (int i = 0; i < count; ++i) {
        out.tiles.push_back({x, y, tileW, tileH});
        x += tileW + p.gap;
    }
    if (showPlus) out.plus = {x, y, tileH * 0.6, tileH};
    return out;
}

int navigate(const std::vector<Rect>& rects, int current, NavDir dir) {
    if (rects.empty()) return -1;
    if (current < 0 || current >= int(rects.size())) {
        // Nothing selected yet: start at the top-left card.
        int best = 0;
        for (int i = 1; i < int(rects.size()); ++i) {
            const Rect &a = rects[i], &b = rects[best];
            if (a.cy() < b.cy() - 1 || (std::abs(a.cy() - b.cy()) <= 1 && a.cx() < b.cx())) best = i;
        }
        return best;
    }
    const Rect& c = rects[current];
    int best = current;
    double bestScore = 1e300;
    for (int i = 0; i < int(rects.size()); ++i) {
        if (i == current) continue;
        const double dx = rects[i].cx() - c.cx(), dy = rects[i].cy() - c.cy();
        double along = 0, across = 0;
        switch (dir) {
        case NavDir::Left: along = -dx; across = dy; break;
        case NavDir::Right: along = dx; across = dy; break;
        case NavDir::Up: along = -dy; across = dx; break;
        case NavDir::Down: along = dy; across = dx; break;
        }
        if (along <= 1.0) continue;  // not in that direction
        // Prefer straight ahead: sideways distance counts double.
        const double score = along + 2.0 * std::abs(across);
        if (score < bestScore) { bestScore = score; best = i; }
    }
    return best;
}

}  // namespace mc
