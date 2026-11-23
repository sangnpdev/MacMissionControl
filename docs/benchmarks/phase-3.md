# Phase 3 — Virtual Desktop Integration: Results

Measured 2026-11-23, same machine and method as before. Design: `docs/decisions/ADR-003-virtual-desktops.md`.
Raw data: `benchmarks/results/phase3/`.

## Functions and their stability

| Function | Mechanism | Stability | Verified |
|---|---|---|---|
| Enumerate desktops (order, ids, current, names) | registry read | undocumented values, unchanged since Win10; read-only | `desktop_tests`, smoke test, unit tests for parsing |
| Track desktop changes | `RegNotifyChangeKeyValue` event | public API on an undocumented key | smoke test log; idle CPU 0 |
| Switch | Ctrl+Win+Left/Right via `SendInput`, one step per confirmed change | public + documented shortcut; relative only | 250–391 ms per step |
| Create | Ctrl+Win+D | public + documented shortcut | 78 ms |
| Remove | Ctrl+Win+F4 | current desktop only | `desktop_tests` |
| Overlay across desktops | none needed (`WS_EX_TOOLWINDOW` is on every desktop) | verified behaviour | `desktop_tests` |
| Rename | — | not possible with public means | — |
| Internal COM (`IVirtualDesktopManagerInternal`) | not used | build-specific; wrong vtable calls a different Explorer method | — |

## In the overview

- `Ctrl+←/→`, `1`–`9`: switch desktops; `Ctrl+N`: create. Also `MissionControl.exe --desktop next|prev|new|N`.
- Desktop switch animation: the old desktop's cards slide out and fade; the new desktop's cards
  slide in from the other side (`desktopTransition` duration/easing). Measured 451–452 ms with the
  default 0.45 s.
- Windows that open, close, show, hide, cloak or uncloak while the overview is open are now added
  or removed (this fixes Phase 1 known issue 3).

## Tests

| Test | Result |
|---|---|
| `unit_tests` | 33/33 pass (2 new: id parsing and lookup, switch steps) |
| `integration_tests` | 4/4 pass, 3 runs |
| `desktop_tests` (live) | pass: create → switch home → tool window stays visible → switch to created → remove → home; state restored |
| `scripts/desktop-smoke.ps1` (live, drives the running app) | pass, 2 runs; registry state identical to the start afterwards |

Smoke-test log (second run):

```
desktop command 1002                       (new)
desktop: now 1 of 2
windows changed while open: 0 now, slide 1
transition 3 done after 452 ms             (old cards slid out)
windows changed while open: 2 now, slide 0 (windows opened on the new desktop)
desktop command 1                          (go to desktop 1)
desktop: now 0 of 2
windows changed while open: 3 now, slide -1
transition 3 done after 451 ms
```

## Problems found and fixed

1. **No slide on the first try.** The shell cloaks the old desktop's windows ~8 ms *before* it writes
   the new current desktop to the registry, so the window refresh ran before the app knew a switch
   was happening and cards vanished. Fix: a 100 ms trailing debounce on window events; the registry
   event lands inside it and sets the direction.
2. **Cards flickered** (1 → 0 → 1 → 2) when windows opened on the new desktop: the shell cloaks,
   uncloaks and re-cloaks new windows. The same debounce removes it.
3. Injected Ctrl+Win+Arrow released Ctrl even while the user held it, so a second Ctrl+→ would not
   work. Fix: modifiers the user already holds are neither pressed nor released.
4. **Correction of Phase 2:** "the dock hides minimized windows" was wrong (faulty PowerShell probe).
   A native probe shows normal iconic windows; minimized windows are in the overview. Docs corrected.

## Performance (regression check)

| Scenario | FPS | p99 interval (ms) | Long frames | Work mean / max (ms) | Private Bytes peak (MB) |
|---|---|---|---|---|---|
| Idle 120 s | — | — | — | — | 26.5 (WS 2.4), CPU 0 in 94/94 samples |
| 5 windows | 240.9 | 4.48 | 0 | 0.32 / 3.66 | 28.0 |
| 10 windows | 240.5 | 4.46 | 2 | 0.54 / 6.87 | 27.8 |
| 20 windows | 240.1 | 4.47 | 5 | 1.01 / 7.75 | 28.4 |
| 30 windows | 235.7 | 8.26 | 32 | 1.38 / 22.8 | 27.0 |
| Soak, 100 cycles, window churn | 215.6 | 20.8 | 12,187 (4.1 %) | 1.39 / 30.4 | 28.8; first → last 27.04 → 27.16, handles 1074 → 1068 |

The soak is not comparable with Phase 2: the overview now also *adds* the windows the churn helper
opens every 700 ms, so it relayouts continuously while open (296k frames vs 37k). Long frames during
window creation come from `DwmUpdateThumbnailProperties` stalls while DWM is busy (known since
Phase 1). No memory or handle growth.

## Known issues

1. A switch to a far desktop passes through each one in between (shortcut limitation).
2. Only the current desktop can be removed; renaming is not possible.
3. Parallax is not applied in the app's desktop transition yet: it needs a background layer, which
   arrives with the desktop strip (Phase 4). The demo shows parallax.
4. Long frames under heavy window churn (4.1 % in the soak).
5. If a future Windows build changes the registry layout, desktop features turn off (logged) rather
   than misbehave; the overview keeps working.
6. Click/`Esc`/`Enter`/keyboard shortcuts in the overview were exercised only through the CLI
   command path, not by real key presses (scripts must not inject keys into the overlay).
