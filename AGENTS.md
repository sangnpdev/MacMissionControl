# Agent guide

Read `docs/architecture/system-design.md` and `docs/decisions/` before changing structure.

## Rules that are easy to break

- **Thread ownership.** Every `HTHUMBNAIL`, the D3D11 device and the DComp device live on the
  compositor thread (`src/rendering/compositor_thread.cpp`). The UI thread only sends commands.
  Do not call DWM thumbnail or DComp APIs from the UI thread.
- **No idle work.** The compositor thread blocks on an event while idle. Do not add timers,
  polling or a capture loop. Window changes come from `SetWinEventHook`, only while the overview
  is open.
- **One animation engine.** Animate through `mc::anim` (time-based, closed form). Do not add
  per-frame integration or a second animation system for chrome.
- **Physical pixels everywhere.** The process is Per-Monitor-V2 aware (`resources/app.manifest`).
  Do not convert to logical units in layout or thumbnail code.
- **No system changes.** No registry writes (the one exception: the per-user Run value for "Start
  with Windows", written only when the user turns it on and never touching `StartupApproved`), no
  policy changes, no Explorer patching. Win+Tab
  takeover is a low-level keyboard hook owned by the process (ADR-005); it must stay removable by
  config (`integration.takeoverWinTab`) and `--no-takeover`.
- **Keyboard hook callback** (`src/input/keyboard_hook.cpp`) must stay trivial: no I/O, no locks, no
  logging inside `proc`. Windows silently drops a hook that exceeds `LowLevelHooksTimeout`.
- **Detector ignores synthetic input** by default; keep it that way or injected bridge keys loop.
- **Desktop tests change the user's session.** `desktop_tests.exe` and `scripts/desktop-smoke.ps1`
  create one desktop and remove only that one after checking its id. Never remove a desktop you did
  not create; never inject Ctrl+Win+F4 without that check.
- **Never inject keys into the overlay from a script**: if the overlay is not foreground, the keys
  land in the user's app. Use `MissionControl.exe --desktop ...` instead.
- **Undocumented APIs** only behind a provider with a Windows build allow-list and a fallback.
  No byte-pattern scanning or hard-coded offsets.
- Retarget animations with `AnimatedValue::springTo` / `VisualMotion::springTo` from the current
  state. Never restart a card from a stored start rect: that drops velocity and causes a jolt.
- Integration tests must pump messages while waiting (`pump()`), and must wait for the product's
  own filter to accept new windows: the shell cloaks and re-cloaks new windows for a while.
- Pure modules (`animation`, `ui/layout`, `window/window_filter`, `config`) must stay free of
  `<windows.h>` and must have unit tests in `tests/unit_tests.cpp`.

## Verify before claiming done

```powershell
scripts\build.ps1                                                   # build + unit tests
build\Release\integration_tests.exe build\Release\mc_testwin.exe    # real DWM
benchmarks\run-phase1.ps1                                           # performance claims need numbers
```

Never report performance numbers that were not measured by `benchmarks/`.
