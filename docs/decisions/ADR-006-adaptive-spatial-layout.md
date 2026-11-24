# ADR-006: Adaptive Spatial Overview Layout

- Status: Accepted
- Date: 2026-11-24

## Context

The overview used justified rows: pick the row count with the best uniform scale, center each row.
Every card had the same scale, so the result looked like a grid, and any change in the window set
recomputed everything, so cards jumped.

## Decision

`layoutOverview` (`src/ui/layout.cpp`, pure, no `<windows.h>`) is now a binary space partition:

1. Sort the windows along one axis by their real position, cut after index k. Both axes and every k
   are tried. The cut position is bisected so both sides can reach the same scale (the scale of a
   side is estimated with the old justified-rows packer). The best (axis, k) maximises that scale.
2. Recurse on both sides. Leaves are disjoint cells, so overlap is impossible by construction.
3. Fit each card into its own cell (aspect kept, inset by half a gap, label reserved, centered).
   Scale is capped at `maxScale` and at 1.5x the median card, so sizes differ but stay comparable.

Stability: `previous` (one rect per window, empty = new) makes cards that already had a slot keep
it. New windows go into free space found MaxRects-style (split and prune), picked by best short side
fit. A full relayout happens only if the new card would be below 80% of the current scale, a window
changed shape, the area changed, or closed windows would let cards grow by more than 25%.
`App::plan` feeds it `lastSlots_` (screen coordinates, cleared on open).

Existing animation is unchanged: `relayout` retargets every card with `springTo` from its current
state, so moved cards glide and unchanged cards do not move.

## Alternatives

| Option | Why not |
|---|---|
| MaxRects / skyline as the main packer | Packs densely, but needs a fixed scale up front and ignores where windows really are. Used only for incremental insert. |
| Force-directed relaxation | Iterative, not deterministic across inputs, no overlap guarantee. |
| Treemap with area weights | Gave thin strips for small windows. The bisected cut fixed that. |

## Cost

Measured with a scratch harness (Release, one thread): 10 windows 0.16 ms, 30 windows 2.9 ms,
100 windows about 35 ms. Runs only on open and on window-set change, never per frame.

## Limits

- The row estimate is optimistic for groups of several windows, so fill is about 45-60%, below a
  tight packing. That is the price of comfortable spacing and similar card sizes.
- A cell too small for gap and label falls back to 20% of itself (gap shrinks). Only seen far above 100 windows.
