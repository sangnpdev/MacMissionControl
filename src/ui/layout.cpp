#include "ui/layout.h"

#include <algorithm>
#include <numeric>
#include <optional>

namespace mc {

namespace {

using Idx = std::vector<std::size_t>;

struct Ctx {
    const std::vector<Rect>& win;
    const LayoutParams& p;
    std::vector<Rect> cells;     // leaf regions, one per window
};

Rect sizeOf(const Rect& r) { return {0, 0, std::max(r.w, 1.0), std::max(r.h, 1.0)}; }

// Reading order of the original windows: rows by center y, then x.
void readingOrder(const std::vector<Rect>& win, Idx& v) {
    std::stable_sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) {
        const Rect &ra = win[a], &rb = win[b];
        if (std::abs(ra.cy() - rb.cy()) > 1.0) return ra.cy() < rb.cy();
        return ra.cx() < rb.cx();
    });
}

// Sort along one axis by original position, ties by the other axis, then index (deterministic).
void sortAlong(const std::vector<Rect>& win, Idx& v, int axis) {
    std::stable_sort(v.begin(), v.end(), [&](std::size_t a, std::size_t b) {
        const Rect &ra = win[a], &rb = win[b];
        const double pa = axis == 0 ? ra.cx() : ra.cy(), pb = axis == 0 ? rb.cx() : rb.cy();
        if (pa != pb) return pa < pb;
        const double qa = axis == 0 ? ra.cy() : ra.cx(), qb = axis == 0 ? rb.cy() : rb.cx();
        return qa != qb ? qa < qb : a < b;
    });
}

// Estimate: the largest uniform scale at which `idx` fits a W x H region as justified rows.
// Only ranks candidate cuts; each card's final scale comes from its own cell.
double fitScale(const Ctx& c, Idx idx, double W, double H) {
    const LayoutParams& p = c.p;
    W -= p.spacing;
    H -= p.spacing;
    if (W <= 0 || H <= 0) return 0.0;
    readingOrder(c.win, idx);
    const std::size_t n = idx.size();
    double best = 0.0;
    for (std::size_t rows = 1; rows <= n; ++rows) {
        double maxRowW = 0.0, sumRowH = 0.0;
        std::size_t at = 0;
        for (std::size_t r = 0; r < rows; ++r) {
            const std::size_t cnt = n / rows + (r < n % rows ? 1 : 0);
            double rowW = 0.0, rowH = 0.0;
            for (std::size_t k = 0; k < cnt; ++k, ++at) {
                const Rect s = sizeOf(c.win[idx[at]]);
                rowW += s.w;
                rowH = std::max(rowH, s.h);
            }
            const double availW = W - p.spacing * double(cnt - 1);
            if (availW <= 0) { maxRowW = 1e300; break; }
            maxRowW = std::max(maxRowW, rowW / availW);
            sumRowH += rowH;
        }
        const double availH = H - p.spacing * double(rows - 1) - p.labelHeight * double(rows);
        if (availH <= 0 || maxRowW >= 1e300) continue;
        best = std::max(best, std::min({1.0 / maxRowW, availH / sumRowH, p.maxScale}));
    }
    return best;
}

// Recursive binary partition (guillotine cuts). Each cut sorts the windows along one axis by their
// original position, so neighbours on the real desktop stay neighbours, and gives each side a share
// of the region chosen so both can reach the same scale. Axis and cut index maximise that scale. Cells are disjoint by construction, so cards cannot overlap.
void partition(Ctx& c, Idx idx, const Rect& region) {
    if (idx.size() == 1) { c.cells[idx[0]] = region; return; }
    const std::size_t n = idx.size();
    const int iterations = n > 40 ? 5 : 10;  // 100 windows: ~35 ms instead of ~80 ms, once per open
    int bestAxis = 0;
    std::size_t bestK = 1;
    double bestF = 0.5, bestScore = -1.0, bestImbalance = 1e300;

    const int axes[2] = {region.w >= region.h ? 0 : 1, region.w >= region.h ? 1 : 0};  // longer side first
    for (int axis : axes) {
        sortAlong(c.win, idx, axis);
        for (std::size_t k = 1; k < n; ++k) {
            const Idx left(idx.begin(), idx.begin() + k), right(idx.begin() + k, idx.end());
            // Cut position f: bisect until both sides can reach the same scale (left grows with f,
            // right shrinks), so no side is squeezed into a thin strip.
            double lo = 0.05, hi = 0.95, f = 0.5, score = 0.0;
            for (int it = 0; it < iterations; ++it) {
                f = (lo + hi) / 2;
                const double sl = fitScale(c, left, axis == 0 ? region.w * f : region.w, axis == 0 ? region.h : region.h * f);
                const double sr = fitScale(c, right, axis == 0 ? region.w * (1 - f) : region.w, axis == 0 ? region.h : region.h * (1 - f));
                score = std::min(sl, sr);
                if (sl < sr) lo = f; else hi = f;
            }
            const double imbalance = std::abs(f - 0.5);
            if (score > bestScore + 1e-9 || (score > bestScore - 1e-9 && imbalance < bestImbalance)) {
                bestScore = score; bestAxis = axis; bestK = k; bestF = f; bestImbalance = imbalance;
            }
        }
    }
    sortAlong(c.win, idx, bestAxis);
    Rect a = region, b = region;
    if (bestAxis == 0) { a.w = region.w * bestF; b.x = region.x + a.w; b.w = region.w - a.w; }
    else               { a.h = region.h * bestF; b.y = region.y + a.h; b.h = region.h - a.h; }
    partition(c, Idx(idx.begin(), idx.begin() + bestK), a);
    partition(c, Idx(idx.begin() + bestK, idx.end()), b);
}

// Full layout from scratch.
std::vector<Rect> layoutFresh(const Rect& inner, const std::vector<Rect>& windows, const LayoutParams& p) {
    const std::size_t n = windows.size();
    Ctx c{windows, p, std::vector<Rect>(n)};
    Idx all(n);
    std::iota(all.begin(), all.end(), std::size_t{0});
    // Cells cover the inner area grown by half a gap; insetting each card by half a gap keeps the margin.
    partition(c, all, {inner.x - p.spacing / 2, inner.y - p.spacing / 2, inner.w + p.spacing, inner.h + p.spacing});

    std::vector<double> scale(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Rect s = sizeOf(windows[i]), &cell = c.cells[i];
        // shortcut: a cell too small for gap + label falls back to 20% of itself (gap shrinks there), upgrade if 100+ windows matter.
        const double availW = std::max(cell.w - p.spacing, cell.w * 0.2);
        const double availH = std::max(cell.h - p.spacing - p.labelHeight, cell.h * 0.2);
        scale[i] = std::min({availW / s.w, availH / s.h, p.maxScale});
    }
    // Keep sizes comparable: no card more than 1.5x the median card.
    std::vector<double> sorted = scale;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const double cap = sorted[n / 2] * 1.5;
    std::vector<Rect> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Rect s = sizeOf(windows[i]), &cell = c.cells[i];
        const double sc = std::min(scale[i], cap);
        const double w = s.w * sc, h = s.h * sc;
        out[i] = {cell.cx() - w / 2, cell.cy() - (h + p.labelHeight) / 2, w, h};
    }
    return out;
}

// Free space left by `blocked`, as maximal rectangles (MaxRects split-and-prune).
std::vector<Rect> freeRects(const Rect& inner, const std::vector<Rect>& blocked) {
    std::vector<Rect> free{inner};
    for (const Rect& o : blocked) {
        std::vector<Rect> next;
        for (const Rect& f : free) {
            if (!f.intersects(o)) { next.push_back(f); continue; }
            if (o.x > f.x) next.push_back({f.x, f.y, o.x - f.x, f.h});
            if (o.right() < f.right()) next.push_back({o.right(), f.y, f.right() - o.right(), f.h});
            if (o.y > f.y) next.push_back({f.x, f.y, f.w, o.y - f.y});
            if (o.bottom() < f.bottom()) next.push_back({f.x, o.bottom(), f.w, f.bottom() - o.bottom()});
        }
        free.clear();
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (next[i].empty()) continue;
            bool inside = false;
            for (std::size_t j = 0; j < next.size() && !inside; ++j) {
                const Rect &a = next[i], &b = next[j];
                inside = i != j && a.x >= b.x && a.y >= b.y && a.right() <= b.right() && a.bottom() <= b.bottom() &&
                         (!(a == b) || j < i);  // identical rects: keep the first
            }
            if (!inside) free.push_back(next[i]);
        }
    }
    return free;
}

// Stability: cards that already had a slot keep it; new windows go into free space. Gives up
// (nullopt) when that would look clearly worse than a fresh layout.
std::optional<std::vector<Rect>> keepPrevious(const Rect& inner, const std::vector<Rect>& windows,
                                              const std::vector<Rect>& previous, const std::vector<Rect>& fresh,
                                              const LayoutParams& p) {
    const std::size_t n = windows.size();
    Idx added;
    std::size_t kept = 0;
    double keptScale = 0.0, freshScale = 0.0;
    std::vector<Rect> blocked;
    for (std::size_t i = 0; i < n; ++i) {
        const Rect& r = previous[i];
        if (r.empty()) { added.push_back(i); continue; }
        const Rect s = sizeOf(windows[i]);
        const bool inside = r.x >= inner.x - 1e-6 && r.y >= inner.y - 1e-6 && r.right() <= inner.right() + 1e-6 &&
                            r.bottom() + p.labelHeight <= inner.bottom() + 1e-6;
        if (!inside || std::abs(r.w / r.h - s.w / s.h) > 0.02 * (s.w / s.h)) return std::nullopt;  // area or window shape changed
        ++kept;
        keptScale += r.w / s.w;
        freshScale += fresh[i].w / s.w;
        blocked.push_back({r.x - p.spacing, r.y - p.spacing, r.w + 2 * p.spacing, r.h + p.labelHeight + 2 * p.spacing});
    }
    if (kept == 0) return std::nullopt;
    keptScale /= double(kept);
    freshScale /= double(kept);
    if (freshScale > keptScale * 1.25) return std::nullopt;  // closed windows freed enough room to be worth a redistribute

    std::vector<Rect> out = previous;
    const double target = std::min(keptScale, p.maxScale);
    for (std::size_t i : added) {
        const Rect s = sizeOf(windows[i]);
        Rect best;
        double bestSc = 0.0, bestFit = 1e300;
        for (const Rect& f : freeRects(inner, blocked)) {
            const double sc = std::min({f.w / s.w, (f.h - p.labelHeight) / s.h, target});
            if (sc <= 0) continue;
            const double fit = std::min(f.w - s.w * sc, f.h - p.labelHeight - s.h * sc);  // best short side fit
            if (sc > bestSc + 1e-9 || (sc > bestSc - 1e-9 && fit < bestFit)) { bestSc = sc; bestFit = fit; best = f; }
        }
        if (bestSc < 0.8 * target) return std::nullopt;
        const double w = s.w * bestSc, h = s.h * bestSc;
        out[i] = {best.cx() - w / 2, best.cy() - (h + p.labelHeight) / 2, w, h};
        blocked.push_back({out[i].x - p.spacing, out[i].y - p.spacing, w + 2 * p.spacing, h + p.labelHeight + 2 * p.spacing});
    }
    return out;
}

}  // namespace

std::vector<Rect> layoutOverview(const Rect& area, const std::vector<Rect>& windows, const LayoutParams& p,
                                 const std::vector<Rect>& previous) {
    const std::size_t n = windows.size();
    if (n == 0) return {};
    const Rect inner{area.x + p.padding, area.y + p.padding, area.w - 2 * p.padding, area.h - 2 * p.padding};
    if (inner.empty()) return std::vector<Rect>(n);
    auto fresh = layoutFresh(inner, windows, p);
    if (previous.size() == n)
        if (auto kept = keepPrevious(inner, windows, previous, fresh, p)) return std::move(*kept);
    return fresh;
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
