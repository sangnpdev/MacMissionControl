# Phase 6 — Optimization & Release: Results

Measured 2026-11-24. Version 0.6.0.

## Done

| Item | Result |
|---|---|
| Memory profiling | Label-visual leak found and fixed (Phase 4); idle WS 18.5 MB / Private 32.9 MB; active WS 56 MB at 30 windows |
| CPU profiling | Idle 0 % in 234/235 samples over 300 s; frame work 0.34–1.55 ms mean at 5–30 windows (budget 4.17 ms at 240 Hz) |
| GPU profiling | 19.7 MB dedicated idle, 27.5 MB open, independent of window count; long-churn growth unresolved (below) |
| Leak detection | Handles flat over a 47-minute churn soak after the fix; Private Bytes flat |
| Multi-monitor / DPI | Verified by screenshot on 1920x1080 @96 DPI + 2880x1800 @150 % |
| Display change / sleep-resume | Handled: an open overview closes safely, the scene is rebuilt for the new layout, the hook is re-installed. Verified with simulated `WM_DISPLAYCHANGE` / `WM_POWERBROADCAST` (real sleep and monitor unplugging were not performed) |
| Stability | `scripts/stress.ps1`: 200 toggles at random 0–250 ms, display/resume events open and idle, 30 rapid desktop switches: process alive, 0 ERROR/CRASH log lines, handles 1106–1115, desktop state restored |
| Crash handling | Minidump + log line on unhandled exceptions (`--crash-test` verified) |
| Release build | Version resource 0.6.0, static CRT, `scripts/package.ps1` → `dist/MissionControl-0.6.0-x64.zip` (0.2 MB) |

## Final benchmark

See `phase-4.md` for the table. The 30-minute soak was **stopped at the user's request** after 33
cycles: its frame data exists (188,587 frames, 235.8 FPS mean, p99 8.34 ms, 1.51 % long frames under
window churn) but its memory samples were not written (the script writes them at the end).

## Not done / open

1. **30-minute soak memory result**: not obtained (stopped twice). The GPU 4 MB-step growth under
   heavy window churn (Phase 4 known issue 1) is therefore still open, and the effect of
   `IDXGIDevice3::Trim()` is unmeasured. To finish:
   `benchmarks\run-phase1.ps1 -Counts @() -IdleSeconds 0 -SoakSeconds 1800 -OutDir benchmarks\results\soak`
2. Real sleep/resume and physical monitor disconnect/reconnect: not performed (simulated only).
3. Device-lost recovery (GPU reset): code path exists, not triggered in a test.
4. Long frames (1–2 % under churn) from `DwmUpdateThumbnailProperties` stalls: known since Phase 1.
5. Click, Enter and arrow keys with a real keyboard/mouse: only through posted messages, plus the
   real-pointer hover screenshot. A manual pass is recommended.
