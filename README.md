# Native Windows Mission Control

A native Windows 11 overview of open windows, inspired by how macOS Mission Control organizes
windows and spaces. C++20, Win32, DWM thumbnails, DirectComposition. No Electron, no WebView,
no Explorer changes.

**Status: 0.6.0 — Phases 0–6 done.** See `docs/benchmarks/phase-1.md` … `phase-6.md` for what
works, what was measured and the known issues.

## Build

Requirements: Visual Studio 2022 or later with the C++ x64 toolset and Windows SDK 10.0.22000+
(developed with VS 2026 / MSVC 14.50 / SDK 10.0.26100). CMake and Ninja come with Visual Studio.

```powershell
scripts\build.ps1                 # Release build + unit tests
scripts\build.ps1 -Config Debug
```

Output: `build\Release\MissionControl.exe`, `mc_testwin.exe`, `unit_tests.exe`, `integration_tests.exe`.

## Run

```powershell
build\Release\MissionControl.exe                 # resident; Win+Tab (or Ctrl+Alt+Space) toggles the overview
build\Release\MissionControl.exe --show          # also opens the overview immediately
build\Release\MissionControl.exe --no-takeover   # leave Win+Tab to Windows Task View
build\Release\MissionControl.exe --quit          # stops the running instance (Win+Tab goes back to Task View)
```

Release package: `scripts\package.ps1` → `dist\MissionControl-<version>-x64.zip`.

The app shows an icon in the notification area: click it to open the overview; right-click for
**Start with Windows** and **Quit**. Start with Windows writes one per-user value
(`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\MissionControl`) only when you tick it, and
removes it when you untick it; it also shows in Settings › Apps › Startup. From a script:
`MissionControl.exe --autostart on|off`.

App icon: `resources\app-icon.png` → `scripts\make-icon.ps1` → `resources\app.ico` (16–256 px).

In the overview: click a window (or select it with the arrow keys and press `Enter`) to switch to
it; click the background or press `Esc` to go back. Scroll down (two-finger swipe or wheel) to
scrub the overview closed; scroll sideways, click a tile in the top strip, `Ctrl+←`/`Ctrl+→` or
`1`–`9` to switch virtual desktops; `Ctrl+N` or the `+` tile creates one. Launching the exe again while it runs toggles the overview;
`MissionControl.exe --desktop next|prev|new|N` sends a desktop command to it.

Win+Tab takeover is a low-level keyboard hook owned by the app (ADR-005): no Windows setting is
changed, and if the app quits or crashes, Win+Tab opens Windows Task View again. Turn it off with
`"integration": {"takeoverWinTab": false}` in the config or `--no-takeover`. The taskbar Task View
button keeps opening Windows Task View.

Log: `%LOCALAPPDATA%\MissionControl\mc.log` (rewritten on every start).

### Configuration

Copy `resources\config.default.json` to `%LOCALAPPDATA%\MissionControl\config.json` and edit it
(springs, easings, durations, stagger, dim, hover, desktop transition, gesture, blur). Restart the
app to apply. Any error rejects the whole file; the log lists each error with its key path.

### Animation demo

```powershell
build\Release\mc_animdemo.exe                                     # uses %LOCALAPPDATA%\MissionControl\config.json if present
build\Release\mc_animdemo.exe --config resources\config.default.json
```

One panel per primitive: easing, springs, stagger, scale/hover, translation with interruption,
opacity/dim, desktop switch with parallax, gesture (drag it), shared transition. `R` reloads the
config while it runs; `Esc` quits.

The app changes no system settings, registry keys or shortcuts. Win+Tab still opens Task View.

## Test

```powershell
build\Release\unit_tests.exe
build\Release\integration_tests.exe build\Release\mc_testwin.exe   # needs a desktop session; opens test windows
build\Release\desktop_tests.exe        # CHANGES the session briefly: creates, switches, removes one desktop
scripts\desktop-smoke.ps1              # same, driving the running overview; restores the start state
scripts\ui-smoke.ps1                   # keyboard focus and scroll gesture via messages posted to the overlay only
scripts\takeover-smoke.ps1             # INJECTS Win+Tab: takeover works, and Task View returns after killing the app
scripts\stress.ps1                     # 200 rapid toggles, simulated display change/resume, 30 desktop switches
```

## Benchmark

```powershell
benchmarks\run-phase1.ps1                                  # idle 300 s + 5/10/20/30 windows x 10 cycles
benchmarks\run-phase1.ps1 -SoakCycles 200 -IdleSeconds 0   # add a soak with window churn
```

Results go to `benchmarks\results\` (`summary.csv`, per-frame `frames_*.csv`, per-sample `samples_*.csv`).
The overlay takes over the screen while the benchmark runs.

## Layout

| Path | Content |
|---|---|
| `src/animation` | Pure animation engine: Bézier, analytic spring, curves, stagger |
| `src/ui` | Pure overview layout |
| `src/window` | Window filter (pure) and Win32 enumeration/activation |
| `src/preview` | RAII DWM thumbnail |
| `src/rendering` | DComp chrome and the compositor-clock frame thread |
| `src/app` | Lifecycle, overlay window, input, CLI |
| `docs/` | Research, architecture, ADRs, benchmark reports |
