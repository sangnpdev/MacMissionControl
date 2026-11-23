# Phase 1 — Proof of Concept: Results

Measured 2026-11-23 on the development machine (`docs/technical-research/environment.md`):
Ryzen 7 H 255, Radeon 780M, Windows 11 25H2 build 26200. Two monitors: 1920x1080 @ 240 Hz
(primary) and 2880x1800 @ 120 Hz. Release build.

All numbers come from `benchmarks/run-phase1.ps1`. Raw data is in `benchmarks/results/`.

## What the PoC does (verified by running it)

| Requirement | Status | Evidence |
|---|---|---|
| Launch, resident, hotkey `Ctrl+Alt+Space` | works | `--probe-hotkeys`: registration succeeds when no instance runs |
| Enumerate windows, bounds, filter | works | integration tests; log lists the windows |
| DWM thumbnails | works | screenshots; integration test `thumbnail_lifecycle_and_source_destroyed` |
| Overview layout (per monitor, aspect kept, no overlap) | works | unit tests; screenshots on 2 monitors |
| Entrance animation (spring, stagger, dim) | works | screenshots; frame CSV |
| Exit animation (back to real rects, dim out) | works | frame CSV; log |
| Click card → activate window | implemented, **not verified** | no manual or automated run yet (needs input) |
| `Esc`/`Enter`/background click → previous window | implemented, **not verified** | same as above; the exit path itself is exercised by every bench cycle |
| Window closed while open → relayout | works | soak with window churn: 41,907 relayout frames, no errors other than expected logs |
| Clean shutdown | works | thumbnails released before overlay destruction; no `DwmUnregisterThumbnail` errors in the log |

## Tests

| Suite | Result |
|---|---|
| `unit_tests` (Bézier, spring vs numeric integration, curves, stagger, layout 1–100 windows, filter, config validation) | 18/18 pass |
| `integration_tests` (enumeration across processes, minimized restored rect, thumbnail lifecycle and source destruction, thumbnail draw order by pixel readback) | 4/4 pass |

## Performance

Each window scenario: 10 open/close cycles. Process sampled every ~250 ms. "Peak" is the highest
sample. Frame data covers every frame of every transition.

### Memory, CPU, GPU

| Scenario | Working Set peak (MB) | Working Set idle after (MB) | Private Bytes peak (MB) | CPU avg (%) | CPU max (%) | GPU dedicated (MB) | GPU shared (MB) | Handles |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Idle 300 s | 3.6 | 3.6 | 26.2 | 0.000 | 0.000 | 5.18 | 3.03 | 1017 |
| 5 windows | 44.5 | 3.2 | 28.3 | 0.39 | 2.7 | 5.18 | 3.03 | 1070 |
| 10 windows | 44.2 | 3.3 | 27.8 | 0.42 | 3.5 | 5.18 | 3.03 | 1072 |
| 20 windows | 44.3 | 3.3 | 26.6 | 0.43 | 3.0 | 5.18 | 3.03 | 1072 |
| 30 windows | 44.3 | 3.5 | 26.9 | 0.53 | 2.3 | 5.18 | 3.03 | 1072 |
| Soak: 200 cycles, 20 windows, 1 window replaced every 700 ms | 44.5 | 3.4 | 27.3 | 0.36 | 5.0 | 5.18 | 3.03 | 1067 |

CPU % is a share of all 16 logical processors. Idle CPU was 0 in all 236 samples.

GPU memory does not change with the number of windows. DWM composes thumbnails from the
source windows' own surfaces, so our process allocates no texture per preview.

### Frames (240 Hz compositor clock, budget 4.167 ms)

| Scenario | Frames | FPS mean | Interval p95 / p99 / max (ms) | Long frames (> 6.25 ms) | Work mean / max (ms) | Open latency mean / max (ms) |
|---|---:|---:|---|---:|---|---|
| 5 windows | 2538 | 240.8 | 4.34 / 4.48 / 4.79 | 0 | 0.38 / 3.14 | 9.2 / 52.8 |
| 10 windows | 2598 | 240.6 | 4.28 / 4.41 / 4.67 | 0 | 0.69 / 3.62 | 12.6 / 57.4 |
| 20 windows | 2575 | 240.3 | 4.27 / 4.35 / 6.85 | 1 | 1.22 / 4.07 | 19.3 / 73.3 |
| 30 windows | 2531 | 238.1 | 4.26 / 4.36 / 29.2 | 11 (0.4 %) | 1.72 / 25.7 | 23.4 / 54.5 |
| Soak (churn) | 74401 | 225.1 | 4.31 / 16.7 / 25.0 | 2128 (2.9 %) | 1.47 / 22.4 | 14.1 / 56.9 |

- *Work* = CPU time on the compositor thread to evaluate the animation and apply it (one
  `DwmUpdateThumbnailProperties` per card plus the DComp commit).
- *Open latency* = from the UI thread's open command to the first entrance frame being applied,
  including enumeration hand-off and thumbnail registration.

### Resource budget check

| Budget | Target | Measured | Verdict |
|---|---|---|---|
| Idle Working Set | 15–30 MB | 3.6 MB (with trim); 35.0 MB without trim | met with trim (see below) |
| Idle Private Bytes | — | 26.2 MB | — |
| Active Working Set | < 100 MB | 44.6 MB peak at 30 windows | met |
| Idle CPU | ~0 % | 0.000 % | met |
| Animation | ≥ 60 FPS | 238–241 FPS mean at 5–30 windows | met |
| Memory leak | none | soak: Private Bytes 27.18 → 27.20 MB over 200 cycles, handles 1065–1071 | none detected in 7 min |

## Findings and decisions

### 1. Idle Working Set exceeded the budget: cause and fix

Without changes, idle Working Set was 35.0 MB. Most of it is the D3D11 device and the GPU driver
DLLs loaded for the DComp chrome. Private Bytes (26.2 MB) is the actual committed memory.

Options considered:

- Create the D3D/DComp device on every open and release it after: device creation measured at
  about 70–80 ms (log: thread start → "chrome ready"). That delay would be added to every open.
  Rejected.
- Trim the working set when idle (`SetProcessWorkingSetSize(-1, -1)` after each exit and after
  startup). Pages move to the standby list and fault back on the next open. **Chosen.**

A/B (`benchmarks/results/no-trim/`):

| | Idle WS | Private Bytes | Open latency mean (5 / 30 windows) | Open latency max (5 / 30) |
|---|---:|---:|---|---|
| no trim | 35.0 MB | 26.3 MB | 8.5 / 21.2 ms | 44.8 / 42.9 ms |
| trim | 3.6 MB | 26.2 MB | 9.2 / 23.4 ms | 52.8 / 54.5 ms |

Trimming costs about 1–2 ms on average and up to ~10–30 ms in the worst case for the first frame.
It does not reduce Private Bytes. `--no-trim` turns it off.

### 2. Long frames come from DWM calls, not from our math

Long frames do not happen on the first frame (thumbnail registration). They follow frames where
the work time was over 4 ms in the middle of an animation: 9 of 11 at 30 windows, 2125 of 2128
in the soak. Our own computation is microseconds. The time is spent in
`DwmUpdateThumbnailProperties`, a synchronous call into DWM, which sometimes stalls when DWM is
busy. The soak makes this worse on purpose: the test process creates a new window every 700 ms.

Phase 2/6 options, to measure before choosing: skip the update when the rounded rect and opacity
did not change; spread updates of settled cards; check whether `DwmFlush`-aligned updates reduce
stalls.

### 3. Measurement mistakes found and corrected

- The first full run (`benchmarks/results/run1-contaminated/`) is **invalid**. The test-window
  helper treated every thread-level `WM_TIMER` as its own churn timer. The system posts such
  timers to its thread while the overlay is open, so the helper closed and reopened windows,
  and the 20/30-window runs measured relayouts. Fixed in `benchmarks/testwin.cpp`, and the run
  was repeated.
- In that run, Private Bytes also grew in the soak because the bench mode kept all frame samples
  in memory (127,540 × 48 bytes ≈ 6.1 MB). Samples are now streamed to disk, and the repeated
  soak shows no growth.

### 4. Bugs found by running the PoC

- A relayout that arrived during the entrance replaced the dim animation, so the dim stayed at 0,
  and the "entrance done" notification was lost. Fixed: the dim has its own clock, and an
  interrupted entrance still reports completion.
- Thumbnails were released after the overlay window was destroyed, so every
  `DwmUnregisterThumbnail` failed with `E_INVALIDARG`. Fixed: the compositor stops in `WM_DESTROY`.

## Known issues

1. Real windows stay visible under the dim; they are not hidden while the overview is open.
2. A minimized window that is chosen is restored after the exit animation. The system restore
   animation then plays on top of the finished transition.
3. Windows opened while the overview is open are not added (closed ones are removed).
4. New windows are excluded for a moment after they appear, because the shell cloaks them
   (`DWM_CLOAKED_SHELL`).
5. Occasional long frames from `DwmUpdateThumbnailProperties` stalls (finding 2).
6. In one early manual run an exit started about 0.8 s after opening without any input from the
   test. The log at the time did not record why. Exit reasons are now logged. It did not recur
   in the later manual runs; the ~500 bench cycles cannot rule it out, because a stray key there
   would only shorten a cycle. Suspected cause: a key event delivered to the overlay, which holds
   keyboard focus.
7. Click, `Esc` and `Enter` paths have not been exercised yet.
8. Not tested yet: sleep/resume, monitor hot-plug, DPI change while running, a 30-minute soak
   (only a 7-minute soak was run), 240 Hz-only configuration.
9. The overview has no titles, icons, hover or keyboard navigation yet (Phase 4).

## How to reproduce

```powershell
scripts\build.ps1
build\Release\integration_tests.exe build\Release\mc_testwin.exe
benchmarks\run-phase1.ps1 -SoakCycles 200
benchmarks\run-phase1.ps1 -Counts 5,30 -IdleSeconds 120 -McArgs '--no-trim' -OutDir benchmarks\results\no-trim
```
