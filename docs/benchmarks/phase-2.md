# Phase 2 — Animation Engine: Results

Measured 2026-11-23, same machine and method as Phase 1 (`phase-1.md`). Raw data:
`benchmarks/results/phase2/`. Design: `docs/decisions/ADR-002-animation-engine-and-config.md`.

## What was built

| Primitive / feature | Where | Verified by |
|---|---|---|
| Cubic Bézier + presets (linear, easeIn, easeOut, easeInOut, emphasized) | `animation.h` | unit tests; demo panel 1 |
| Spring (any from/to/velocity; under-, critically, over-damped; analytic settle time) | `Spring` | unit tests vs numeric integration and brute-force settle scan; demo panel 2 |
| Retargetable value, velocity carried across interruptions | `AnimatedValue` | `animated_value_retarget_preserves_velocity`; demo panel 5 (retarget every 0.2 s) |
| Translation + uniform scale + opacity, exact aspect ratio | `VisualMotion` | `visual_motion_keeps_aspect_and_lands`; demo panels 4, 5, 9 |
| Stagger | `staggerDelay` | unit test; demo panel 3 |
| Parallax | `parallaxOffset` | unit test; demo panel 7 |
| Gesture-driven progress, velocity estimate, commit/cancel, rubber band, inertia on release | `GestureTracker`, `rubberBand` | unit tests; demo panel 8 (drag, or scripted drag) |
| Shared transition (tile → full panel and back) | `VisualMotion` | demo panel 9 |
| JSON configuration with strict validation | `config/json.*`, `loadAnimationConfig` | 6 unit tests; app run with a valid and an invalid file (log below) |
| App cards and dim moved to the new engine | `compositor_thread.cpp` | benchmark below, integration tests |
| DWM call skipped when the rounded rect and opacity did not change | `Thumbnail::update` | benchmark below |

Demo: `build\Release\mc_animdemo.exe`. Screenshots taken during the run showed all 9 panels
animating at 240 FPS (worst frame 4.4–5.0 ms over 1 s).

Config check in the app (`--config`):

```
config error: unknown key extra
config error: dim.inEasing: unknown easing "bouncy" (...)
config: ...\bad.json rejected, using defaults          -> dim reached 0.55 (default)
config: loaded ...\good.json                           -> dim reached 0.70 (from file)
```

## Tests

| Suite | Result |
|---|---|
| `unit_tests` | 31/31 pass (13 new: general spring, settle tightness, retarget velocity, timed/delay/jump, initial velocity, gesture velocity/release, rubber band, parallax, visual motion, JSON valid, JSON invalid with positions, default config file, partial override and errors) |
| `integration_tests` | 4/4 pass, 9 consecutive runs without failure after the fixes below |

## Performance (regression check against Phase 1)

| Scenario | Phase 1 work mean / max (ms) | Phase 2 work mean / max (ms) | Phase 2 FPS | Phase 2 long frames | Phase 2 exit frames per cycle |
|---|---|---|---|---|---|
| 5 windows | 0.38 / 3.14 | 0.29 / 2.31 | 241.0 | 0 | 104 (Phase 1: ~100) |
| 10 windows | 0.69 / 3.62 | 0.51 / 5.06 | 240.8 | 1 | — |
| 20 windows | 1.22 / 4.07 | 0.91 / 4.32 | 240.2 | 3 | — |
| 30 windows | 1.72 / 25.7 | 1.31 / 20.2 | 236.0 | 28 (1.0 %) | 112 (Phase 1: 99) |
| Soak, 100 cycles, churn | 1.47 / 22.4 | 1.11 / 20.2 | 230.5 | 826 | — |

| Memory / CPU | Phase 2 |
|---|---|
| Idle Working Set / Private Bytes | 2.0 MB / 26.7 MB |
| Active Working Set peak (30 windows) | 44.8 MB |
| Idle CPU | 0 in 47 of 47 samples |
| GPU dedicated / shared | 5.18 / 3.03 MB, unchanged by window count |
| Soak Private Bytes first → last | 27.29 → 27.20 MB; handles 1070 → 1072 |

Reading the numbers:

- Average frame work fell about 25 % because unchanged thumbnails are no longer sent to DWM.
- Long frames still come from `DwmUpdateThumbnailProperties` stalls (Phase 1 finding 2). Their
  count varies a lot between runs of the same scenario: an earlier Phase 2 run measured 10 long
  frames at 30 windows, this one 28. Not solved.

## Problems found and fixed during Phase 2

1. **Transitions got longer** (first Phase 2 run: exit 142 frames per cycle vs 99 in Phase 1),
   so the chosen window was returned ~0.18 s later. Cause: the envelope settle time is safe but
   up to 1.8× late, and the 0.25 px tolerance was finer than DWM's whole-pixel rounding. Fix:
   refine the envelope by a 1 ms backward scan, and use 0.5 px. Exit is back to 104–112 frames.
2. **Integration tests were flaky** (0–3 failures per run). Causes, each confirmed with diagnostics:
   - The shell adopts a new window in steps (cloaked, uncloaked, cloaked again, not yet on the
     current virtual desktop). The test now waits until the product's own filter accepts the
     windows for 200 ms.
   - The test process owned a window but did not pump messages. It now pumps while waiting.
   - After the source dies, `DwmUpdateThumbnailProperties` can still succeed briefly. The test
     now polls for up to 2 s.
   - Minimizing another process's window from the test sometimes had no effect (cause not found).
     The test now has the helper create its window minimized, and checks our workspace-to-screen
     conversion exactly. *Correction (Phase 3): this report first blamed the MyDockFinder dock for
     hiding minimized windows; a native probe disproved that, see `windows-apis.md`.*
   - The pixel-readback draw-order test sometimes saw neither thumbnail on screen. It now reports
     SKIP (inconclusive) in that case and fails only when the wrong thumbnail is on top.
3. `small` is a macro in `rpcndr.h`; renamed a demo variable.

## Known issues

1. Long frames from DWM call stalls (above).
2. ~~Dock hides minimized windows~~ — wrong, corrected in Phase 3: minimized windows are iconic and
   appear in the overview.
3. Gesture input is not wired into the app yet; there is no OS-level touchpad progress. The model
   is ready and exercised by the demo (Phase 4 wires mouse wheel / precision touchpad pan).
4. `hover`, `desktopTransition` and `blur` settings are only used by the demo until Phases 3/4.
5. Blur is configurable but not rendered (no renderer support yet).
6. Click/`Esc`/`Enter` in the overview are still not exercised (carried over from Phase 1).
7. Config changes need an app restart (the demo reloads with `R`).
