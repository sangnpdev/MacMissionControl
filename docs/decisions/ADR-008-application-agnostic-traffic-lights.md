# ADR-008: Application-agnostic traffic lights

- Status: Accepted (experimental feature, off by default)
- Date: 2026-11-24

## Context

The overlay that covers the native caption buttons (`src/app/edge_poc.*`) decided which windows to cover from a list of
exe names (Edge, Chrome, Explorer, Notepad). The goal is every ordinary window, without a list: discover windows,
decide from what the window itself reports, let the user turn apps off, and keep the native buttons when unsure.

Update: covering every compatible app proved too unpredictable (apps that draw their own buttons report nothing usable).
The default mode is now `listedApps`: only a fixed list of applications, each checked by hand, is covered. The engine and
the other modes stay; an app that is not installed is simply never seen, so it costs nothing and raises no error.

## What the code does now

```
 WinEvent hooks (show, foreground, per target process)        EnumWindows once at start
                         \                                      /
                          EdgeOverlays::consider(hwnd)   (rejected windows are not re-probed for 60 s, bounded to 256)
                                   |
                  EdgePoc::evaluate  --- cheap filters: top-level, unowned, no tool window, not our process,
                                   |      not elevated (token unreadable)
                                   |--- resolvePolicy(registry, exe, class, title)     [app_registry]   user setting first:
                                   |                                                    a window turned off is never probed
                                   |--- assess(facts, probe)                            [compat_engine]  verdict + geometry
                                   |      probe = DWM bounds + WM_NCHITTEST scan (only for apps without a measured profile)
                                   '--- decide(assessment, policy, mode)                [app_registry]   cover or keep native
                                   |
                    EdgePoc::attach  (one overlay per window: owned layered window, shared D2D factory)
```

Pure modules (in `mc_core`, unit tested): `window/compat_engine`, `window/app_registry`, `window/titlebar_profile`
(measured profiles), `window/window_classifier`. Win32 side: the probe, hooks and overlays in `edge_poc.cpp`.
The class names `EdgePoc` / `EdgeOverlays` are historical; they are the overlay and its manager for any app.

### Verdicts

| Verdict | Meaning | Covered by default |
|---|---|---|
| Supported | measured profile, or both geometry sources found, or the hit-test found the buttons | yes |
| Partial | only the DWM bounds are known (exact for Windows-drawn frames, unverified for this app) | yes (mode `allCompatible`) |
| Unsupported | no way to locate the buttons, or no caption (borderless, fullscreen: games) | no: native buttons stay |
| Excluded | system shell process, or turned off by the user | no |

Mode `listedApps` (default) covers Supported and Partial windows of the listed applications (below) plus apps the user
explicitly turned on; any other app logs "not in the application list". Mode `allCompatible` covers Supported and Partial
of every app. Mode `verifiedOnly` covers only apps with a measured profile plus apps the user explicitly turned on.

### Listed applications (`listedApps()` in `src/window/app_registry.cpp`)

| App | Exe | How the buttons are located | Checked |
|---|---|---|---|
| Edge, Chrome | `msedge.exe`, `chrome.exe` | profile, height 40 | screenshot + clicks |
| Explorer, Notepad | `explorer.exe`, `notepad.exe` | profile, DWM bounds | screenshot + clicks |
| Windows Terminal | `windowsterminal.exe` | profile, height 40 | screenshot |
| VS Code, Cursor | `code.exe`, `cursor.exe` | hit-test | screenshot |
| PowerShell, cmd (console host) | `powershell.exe`, `pwsh.exe`, `cmd.exe` | probe (DWM bounds + hit-test) | screenshot |
| DBX, DBeaver, IntelliJ, Android Studio | `dbx.exe`, `dbeaver.exe`, `idea64.exe`, `studio64.exe` | probe (DWM bounds + hit-test) | screenshot |
| WinRAR, Media Player, Visual Studio | `winrar.exe`, `microsoft.media.player.exe`, `devenv.exe` | probe (DWM bounds + hit-test) | screenshot (Visual Studio: a dialog only) |
| Zalo | `zalo.exe` | fixed strip 157 / 0, height 24 | screenshot + clicks |
| Docker Desktop | `docker desktop.exe` | fixed strip 148 / 10, height 62 | screenshot + clicks |
| Warp | `warp.exe` | fixed strip 138 / 0, height 35 | screenshot + clicks |
| Photos | `photos.exe` (UWP identity) | fixed strip 145 / 1, height 48 | screenshot + clicks |
| Cloudflare One Client | `cloudflare warp.exe` | listed, not verified: it showed no captioned window here (tray only) | no |

"Fixed strip" (`StripProfile::fixedStrip`): for apps that report neither caption bounds nor a hit-test answer, the strip
sits at a measured distance from the frame's right edge (left edge / right gap, logical px) and the given height. It
applies only to the listed window class with a caption. "Clicks" = minimize, maximize and restore through the dots on
windows launched for the test.

### Geometry sources (measured against the real buttons, Windows 11 26200, 96 DPI)

| App | `DWMWA_CAPTION_BUTTON_BOUNDS` | `WM_NCHITTEST` scan | Real Close button | Engine result |
|---|---|---|---|---|
| Explorer | 146x30, right | no `HT*` code | 45x29 | verified profile (trust the bounds) |
| Notepad | 146x30 right | 48x30 | 45x29 | verified profile |
| `mc_testwin` (DefWindowProc) | 146x30 | 36x21 (classic metrics) | DWM-drawn | Supported, bounds win (union) |
| Edge, Chrome | 146x30 **wrong** | 45x40 / 48x39 right | 40 tall | verified profile (40) |
| VS Code | none | 45x36 | not measured | Supported by hit-test |
| UWP (Calculator, Settings) | none | 45x33 | 46x32 | Supported by hit-test |
| Windows Terminal | 146x30 **wrong** | no `HT*` code | 46x40 | verified profile (40) |
| Windows Forms dialog | 49 wide (Close only) | - | - | Partial, widened to 146 |
| Zalo, Docker Desktop, Warp, Photos | none | no `HT*` code | measured by hover / glyph spacing | verified fixed strip |

No single source is right for every app: the hit-test is wrong for DefWindowProc windows, the DWM bounds are wrong for
apps that draw their own buttons, and Windows Terminal answers neither. So: measured profiles first, then the union of
both sources, then one source alone. The union never leaves native pixels showing; the cost of a wrong profile or a
Partial verdict is a few native pixels or a slightly larger patch, never a broken window.

## Probe and safety

- The probe sends `WM_NCHITTEST` (a query the system itself sends on every mouse move) with `SendMessageTimeout`, 60 ms,
  `SMTO_ABORTIFHUNG`; about 100 messages once per window, again after a zoom or DPI change. Never to windows the user
  turned off, never to elevated windows, our own, or system shell processes. No injection, no hook in the other process, no
  window style or procedure change; the overlay is a window of this process that is owned by the target.
- Actions go to the exact window with `PostMessage`: `WM_CLOSE`, `WM_SYSCOMMAND` (`SC_MINIMIZE`, `SC_MAXIMIZE`, `SC_RESTORE`).
- Windows that can not be owned (UWP frames) fall back to Z-order tracking; the system-wide `EVENT_OBJECT_REORDER` hook is
  armed only while such an overlay exists, for 2.5 s after a window appears or is activated.

## Configuration

`config.json` (read at start), `globalTrafficLights.experimentalOverlay`:

```json
{ "enabled": true, "titleContains": "", "mode": "listedApps",
  "apps": { "code.exe": { "enabled": false }, "slack.exe": { "enabled": true, "height": 40 } },
  "windows": [ { "exe": "app.exe", "class": "Dialog", "titleContains": "Secret", "enabled": false } ],
  "extraApps": [], "excludeApps": [] }
```

Priority: window rule (first match) > application setting > default (cover when compatible). `enabled: false` excludes;
`enabled: true` opts in (also for Partial apps in `verifiedOnly` mode) but never forces an Unsupported window.
`height` overrides the strip height (logical px, 16 to 80). The older `extraApps` / `excludeApps` still work.

`%LOCALAPPDATA%\MissionControl\apps.json` (same schema, written by the tray menu, atomic replace) wins over `config.json`.
A file with any error is ignored as a whole and the error is logged. Tray menu while the feature runs: "Turn off for
<last app>", "Turned-off apps" (click to turn back on), "Only apps with a measured profile".

`MissionControl.log` has one `compat:` line per evaluated window (exe, class, verdict, source, decision, reason): the
diagnostics for "why is this app (not) covered".

## Measured (Windows 11 26200)

| | Before | After |
|---|---|---|
| Private bytes, 40 overlays | 88.1 MB (35 overlays; ~1.5 MB each) | 38.2 MB (40 overlays; 36 MB with none) |
| Threads after window churn | 36 to 50 | 25 |
| Memory over 5 cycles of 30 windows opened and closed | flat | flat (37.5 to 37.9 MB), handles 1123 to 1130 |
| CPU, one of 20 windows replaced every 150 ms | 1.33% | 1.17% |
| Idle CPU | 0.00% | 0.00% |

The cost was a Direct2D factory per overlay; there is now one shared factory, and the process-exit wait is per process, not
per window.

## Tested (disposable windows only)

Covered with the native buttons hidden, checked by screenshot: Edge, Chrome, Explorer, Notepad, Windows Terminal, a console
window, DefWindowProc windows, a Windows Forms dialog (Partial, two greyed dots), Calculator and Settings (UWP).
Minimize, maximize, restore and close verified on `mc_testwin`, Calculator and the dialog (and earlier on Explorer, Notepad,
Chrome, Edge). VS Code and Settings are covered/decided as logged; VS Code buttons were not measured by hover and no
button of the user's own windows was clicked. Not covered: games and fullscreen windows (no caption), elevated windows,
owned dialogs, tool windows, system shell processes.

## Limits that stay

- Snap Layout hover on the maximize button is lost on every covered window (Win+Z still works).
- Mica / gradient title bars: the overlay is one sampled color.
- An app with its own buttons whose size neither source reports needs a measured profile or a `height` override
  (Windows Terminal was such a case).
- Not tested: windows on other virtual desktops (the overlay hides while the window is cloaked and shows again on uncloak),
  Qt / Discord / DBeaver / .NET WPF apps (not running here; the engine classifies by behavior, not by framework, so they
  go through the same probe and will show up as `compat:` lines), high-contrast themes, multiple monitors with mixed DPI
  beyond the earlier Edge/Explorer checks.
- UWP apps are hosted by `ApplicationFrameHost.exe`, so the settings list shows them as "Application Frame Host" (one entry
  for all UWP apps) and a setting applies to all of them.
- Applying a change re-evaluates every covered window once (a probe of up to ~100 messages for windows without a measured
  profile); with very many windows this takes a moment on the UI thread.

## Settings window (`src/app/settings_window.*`)

A native Win32 window in the main process (tab control, list view, buttons, details pane; Common-Controls 6 through the
manifest, DPI-aware, keyboard navigation). Opened from the tray menu ("Traffic lights settings...") or by
`MissionControl.exe --settings` (also on a running instance). It knows nothing about hooks: it talks to the app through
`SettingsHost` callbacks (detected apps, the user's `RegistryData`, running state, "changed").

| Tab | Content |
|---|---|
| General | enable switch, mode (All compatible / Only measured profiles), what is never covered |
| Applications | every app seen since the feature started: icon, display name (file description), exe, status, covered window count, a checkbox that turns it on or off, "Reset to default" |
| Custom applications | apps the user turned on explicitly; "Add application..." (Browse EXE), Remove, Reset |
| Excluded applications | apps turned off by the user (removable) and system processes (shown, always excluded); Add, Remove |

Selecting an app shows executable path, process id, window class, latest title, compatibility and how the buttons were located,
the engine's reason, the user's setting and where the diagnostics are. Adding an EXE is checked first: an app seen running
with no usable buttons is refused; an app never seen asks for confirmation (every window is still checked when it appears and
an unsupported window is never forced). Changes are written to `apps.json` and applied at once (`EdgeOverlays::applyRules`
drops overlays that no longer qualify and looks at all windows again). The on/off switch is stored in `apps.json` as
`"enabled"` and wins over `config.json`.

Verified on the running app with disposable windows: list, details, uncheck (overlay removed at once, `apps.json` written),
Excluded + Remove (overlay back at once), Enable switch (9 overlays to 0 and back), Browse EXE for Excluded and Custom, the
confirmation for an unseen app, cancelling the file dialog. 40 open/close cycles of the window: GDI objects 95, USER objects 64,
handles 1213 and private bytes 41.3 MB all flat after the first cycle (the first one loads the common controls).
Cosmetic: on the General tab the static texts have the dialog background instead of the tab page color.

## Active / inactive color sync (2026-11-24)

The app's own title bar changes color at once on activation (measured with a 2 ms pixel probe on Edge and on a plain Windows
frame: the switch happens 7 to 20 ms after the activation, no fade). The overlay used to sample the bar 80 ms later and jump,
while the dots faded over 180 ms: three moments, two visible effects. Now `EdgePoc::followTitleBar()` samples the bar right away
and every 12 ms for about 0.3 s, and shows the color it sees without easing of its own; the dots fade in 0.12 s from the same
moment. Measured the same way after the change: the overlay changes in the same 2 ms sample as the app's bar (11 to 16 ms after the
switch), where it used to trail by 87 to 127 ms. The timer runs only after an activation change, never idle.
