# Phase 4 — Complete Mission Control UI: Results

Measured 2026-11-23/24, same machine and method as before. Design: `docs/decisions/ADR-004-overview-ui-rendering.md`.
Raw data: `benchmarks/results/phase4/` (development runs) and `benchmarks/results/final/` (final run).

## What was built and how it was verified

| Feature | Verified by |
|---|---|
| Wallpaper backdrop per monitor (dim, optional blur) replacing the dim over real windows | screenshots (both monitors), blur via `--config` |
| Desktop strip: tiles with wallpaper preview and names, current tile ring, `+` button, click to switch/create | screenshots; click path shares code with `--desktop` (smoke tested) |
| Window cards: app icon + ellipsized title under each card, per-monitor DPI | screenshot of real apps on a 96-DPI and a 150 % monitor |
| Hover: scale (`hover.scale`, `hover.spring`), ring, soft shadow | screenshot with the real pointer over a card |
| Keyboard: arrows move the focus, Enter opens it | `scripts/ui-smoke.ps1` (posted keys) + screenshot |
| Gesture: vertical scroll scrubs the exit with rubber band; release decides by velocity | `ui-smoke.ps1`: slow scroll (p 0.13, v 1.43/s) → stays; fast (p 0.80, v 8.57/s) → closes |
| Horizontal scroll switches desktops | code path shares `requestDesktop` with the smoke-tested commands; not driven by a test |
| Multi-monitor, mixed DPI | screenshot of the whole virtual screen (1920x1080 @96 DPI + 2880x1800 @150 %) |
| Configuration (hover, blur, dim, desktop transition) | config tests + screenshots |

## Performance (final run, `benchmarks/results/final/`)

| Scenario | WS peak (MB) | Private peak (MB) | GPU dedicated (MB) | FPS | p99 / max interval (ms) | Long frames | Work mean / max (ms) | Open latency (ms) |
|---|---|---|---|---|---|---|---|---|
| Idle 300 s | 18.5 | 32.9 | 19.7 | — | — | — | — | — |
| 5 windows | 56.1 | 62.9 | 27.5 | 240.3 | 4.34 / 10.4 | 1 | 0.34 / 10.4 | 3.9 mean, 4.7 max |
| 10 windows | 55.9 | 60.6 | 27.5 | 240.0 | 4.63 / 10.2 | 4 | 0.62 / 10.1 | 6.0 mean, 8.6 max |
| 20 windows | 56.1 | 62.5 | 27.5 | 238.7 | 4.73 / 13.1 | 17 | 1.11 / 13.1 | 11.3 mean, 17.4 max |
| 30 windows | 56.0 | 61.3 | 27.5 | 238.2 | 4.70 / 14.6 | 21 | 1.55 / 14.6 | 15.4 mean, 16.5 max |

Open latency excludes the first open of each bench run (159–200 ms): the bench opens immediately at
launch, before the startup wallpaper preparation finishes, so the first open decodes the wallpaper.
A resident app prepares it at startup; this cost then never hits an open.

Against the budget: active Working Set 56 MB (< 100 MB), idle CPU 0 in 234 of 235 samples.

Compared with Phase 3: Private Bytes +6–8 MB (Direct2D, DirectWrite, WIC), GPU dedicated +14.5 MB
idle (wallpaper backdrops of two monitors, half resolution with parallax overscan).

## Problems found and fixed

1. **Everything drawn under the backdrop.** `AddVisual(v, TRUE, nullptr)` puts the visual at the
   *bottom*; `FALSE` puts it on top. Found by screenshot (only the wallpaper was visible).
2. **Label leak.** A card whose thumbnail had died (window closed while the overview was open) was
   treated as "moved", so its label visual stayed in the tree forever. Soak showed handles +52 and
   GPU memory growing ~4 MB per 50 s. After the fix, handles stayed at 1115–1121 over a 47-minute soak.
3. Opening got slower (labels): labels are now created one per frame with a cached text format.
4. A suspected tile-seam "grid" on the blurred backdrop: measured 0 seams numerically; it was an
   artefact of the half-size screenshot. No change kept.
5. Scripts: PowerShell turns `$null` into `""` for string P/Invoke arguments (FindWindow found
   nothing), and a DPI-unaware PowerShell gets virtualized window rects. Both fixed in the scripts.

## Known issues

1. **GPU memory under long churn is not settled.** After the label-leak fix, a 47-minute soak still
   showed GPU dedicated rising in 4 MB steps (30 → 42.5 MB) while Private Bytes and handles were
   flat. `IDXGIDevice3::Trim()` after each close was added, but both soaks that would have measured
   it were stopped early, so its effect is **unknown**.
2. Hover is verified with the real pointer only; posted mouse messages are overridden by the real
   pointer, so there is no automated hover test.
3. The strip shows the same wallpaper preview for every desktop (no per-desktop wallpaper, no live
   window previews of other desktops).
4. Wallpaper is always drawn "Fill"; Fit/Center/Tile/Span and slideshows are approximated.
