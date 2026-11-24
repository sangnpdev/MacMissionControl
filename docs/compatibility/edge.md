# Compatibility report: Microsoft Edge (global traffic lights)

Result: **Native Fallback.** Edge keeps its Windows buttons. The app draws nothing on Edge.
Date: 2026-11-24. Edge 158.0.4370.33, Windows 11 26200.

## Verified on this machine
- Class `Chrome_WidgetWin_1`, style has `WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX`.
- `DWMWA_CAPTION_BUTTON_BOUNDS` = `[2600,0,2892,57]` at 192 DPI: the buttons are at the **top right**, 292 px wide (146 px at 96 DPI).
- Screenshot from the user (Edge, default title bar): the top-left holds the tab-actions button, then the tabs. Minimize / restore / close are at the top right. There is no free area on the left.
- Mixed DPI is real here (Edge 192 on a secondary monitor with negative x, Chrome 96).
- Maximized window rect is 8 px larger than the screen on every side; `DWMWA_EXTENDED_FRAME_BOUNDS` gives the real frame.

## Technical basis, not tested here
- Chromium draws its own caption buttons and answers `WM_NCHITTEST` (`HTMAXBUTTON`), which is what gives Snap Layout. No flag, theme or extension moves or restyles them on Windows.

## Levels
| Level | Edge |
|---|---|
| 1 Native replacement | No. Needs injection or subclassing (forbidden). |
| 2 Application-supported custom title bar | None for Edge on Windows. |
| 3 Validated overlay | No. Native buttons stay on the right (two sets), or an overlay covering them breaks Snap Layout and pretends a replacement. |
| 4 Fallback | **Yes.** |

## Not done / needs a test if revisited
UI Automation bounds (it may switch Chromium into accessibility mode and cost CPU/RAM), Snap Layout behavior under a covering overlay.

Implemented: `classifyTitleBar` (`src/window/window_classifier.*`), pure, unit-tested, always `NativeFallback`. No consumer in the app yet.

---

# Experimental: traffic lights over EVERY Edge window (option B)

Date: 2026-11-24. Off by default. Started as a one-window PoC, extended the same day to every Edge window. Code: `src/app/edge_poc.*`, drawing shared with the overview in
`src/rendering/traffic_lights_draw.*`. This judges the look, it is not a supported integration.

## Enable / disable
- Config (`%LOCALAPPDATA%\MissionControl\config.json`):
  `{"globalTrafficLights":{"experimentalOverlay":{"enabled":true}}}`
- Target = every Edge window (class `Chrome_WidgetWin_1`, `msedge.exe`, classified `Edge`, caption-button
  bounds available), whatever tab is open. Windows that exist at start are found once; windows shown
  later are found through one global `EVENT_OBJECT_SHOW` hook that only looks at the window class (and
  through the foreground event). Closed windows lose their overlay, Edge exiting removes all of them.
- Optional `"titleContains": "TEXT"`: only windows whose title contains TEXT when first seen. A test aid
  to cover one disposable window (the title is the active tab, so it is not tracked afterwards). Empty or
  absent = every Edge window.
- Tray menu: "Edge traffic lights (experimental)" toggles it at runtime (immediate stop).
- Killing the app removes the overlays (they are windows of the app process).

## What it does
A layered, no-activate window directly above the target in Z-order (not topmost), exactly over the
native caption-button strip from `DWMWA_CAPTION_BUTTON_BOUNDS`, painted with the sampled title bar color
and three circles. Tab Actions, tabs and toolbar are untouched. One overlay per Edge window; each moves with its window
through WinEvent hooks limited to the Edge process (shared by its windows), plus the global show and
foreground hooks. Events are coalesced, there is no polling. Clicks on the
strip are swallowed; only the circles act. Actions are `PostMessage`: `WM_CLOSE`, `WM_SYSCOMMAND`
`SC_MINIMIZE`, `SC_MAXIMIZE`/`SC_RESTORE`.

## Results, one window (disposable InPrivate window, scratch profile, Windows 11 26200)
| Check | Result |
|---|---|
| Overlay placement 96 DPI / 192 DPI (second monitor, negative x) | Verified: overlay rect equals the DWM caption-button rect exactly (146x30 / 292x57) |
| Maximized | Verified after a fix: Edge draws its buttons 8 px lower than the bounds say, so the strip is anchored at the visible top |
| Restore / maximize via green, minimize via yellow, close via red | Verified; minimize hides the overlay, restore shows it again |
| Hover glyphs, pressed, hover-enlarge, inactive (grey) | Verified by screenshot |
| Press red, drag off, release | No action (verified) |
| Move / resize | Follows (verified at read time; Edge updates asynchronously, so the overlay can lag one frame) |
| Native Snap Layout flyout on hover of the maximize button | **Lost**: hovering the green circle shows no flyout; hovering the native button (PoC off) does |
| Win+Z | Flyout still opens with the PoC on (2 of 2 tries; one earlier try showed none and is unexplained) |
| Close: overlay removed | Verified: overlay is gone after the target closes (needed a process-exit wait, Edge sends no DESTROY then) |
| 1000 start/stop cycles in one process | GDI objects 0, handles flat (135), private bytes flat |
| Resources (MissionControl.exe, idle 10 s) | PoC on: WS 18.2 MB, Private 38.5 MB, CPU 0.00%. PoC off: WS 16.8 MB, Private 35.5 MB. While dragging the window: ~7% of one core |

## Results, every window (2026-11-24, same machine)
| Check | Result |
|---|---|
| Windows that exist at start (3 disposable InPrivate windows, 1 real window on the second monitor) | All attached, log `started, N window(s)` |
| Window opened after the app started | Attached; circles visible. Needed two fixes, below |
| Window closed with the red circle (other Edge windows keep the process alive) | Window closes, overlay removed 137 ms later |
| Edge process exits (19 scratch processes killed) | Every overlay removed, 0 left |
| Window title unrelated to any marker (the real window, a YouTube tab) | Attached: no title needed |
| Idle cost with 5 windows, 10 s | Process CPU 0.0 ms |
| Real window under another shell's menu bar | Overlay placed correctly but hidden by that always-on-top bar, as the native buttons are; not a test of the look |

Color match (2026-11-24, user report: the patch looked different from Edge's bar): the title bar color changes
a variable time after activation (profile with a theme: active `#1D1E30`, inactive `#2B2B34`), and the single
sample taken 80 ms later kept the old color, so an inactive window showed a navy patch on a grey bar. Now
sampled four times over ~1.4 s. Measured on a real window with the theme: overlay equals the bar next to it
(`MATCH`) at 0.3 s and at 2 s after activating and after deactivating it, and on all four windows open.

Found and fixed while testing:
- A new Edge window is cloaked by the shell for about 0.3 s: the overlay stayed hidden because nothing
  re-placed it. Fix: also hook `EVENT_OBJECT_CLOAKED` / `UNCLOAKED` for the Edge process.
- Edge raises a window above the overlay 0.3-1.2 s after it appeared (measured), with no event for that
  window, so the native buttons showed for seconds. Z-order changes of top-level windows do raise
  `EVENT_OBJECT_REORDER`, but system-wide and constantly (about 20 per second while idle here). Fix: listen
  to it only for 2.5 s after an Edge window appeared or was activated, then unhook (never while idle).
- Closing a window: Chromium hides it and destroys it later with no event (the process stays alive), so the
  overlay was left hidden. Fix: while the target is hidden, look again up to 5 times, 300 ms apart.

## Limits found
- Background color is one sample next to the strip, repeated 80, 300, 740 and 1400 ms after a move or an activation change. A theme with a gradient / mica title bar can still make a visible patch.
- Z-order (fixed): Edge raises its window above the overlay with no event when the page is clicked, which made the overlay fall behind it. The overlay is now an **owned window** of the Edge window (`GWLP_HWNDPARENT` on our own window only), so the window manager keeps it above Edge and below other apps, and hides it with Edge. Verified: clicks in the page (3 times, >3 s apart), typing, new/switch/close tab, another window covering the strip (overlay stays behind it), minimize/restore, Win+Z. If setting the owner ever fails, the old Z-order tracking is the fallback.
- Close sends `WM_CLOSE`; Edge may take seconds to exit.
- A window Edge raises above the overlay long after it appeared (more than 2.5 s, with no foreground change) keeps the native buttons visible until the next move, foreground change or minimize/restore (safe failure).
- Not tested: vertical tabs, Edge themes, high contrast, installed web apps (`--app` windows), a real profile with many windows.
- Tab Actions is unchanged. The overlay stays on the right; moving the circles to the left was not attempted (separate experiment).
- Strip height (fixed): `DWMWA_CAPTION_BUTTON_BOUNDS` reports 30 px of height but Edge's native buttons are 40 logical px tall (measured with Edge's own hover highlight on Close: y 251..289 at 96 DPI, 78 px at 192 DPI, 24..63 maximized). Covering only the bounds left the lower part of the native buttons visible as a grey patch. The overlay is now `40 * dpi` tall, from the frame top; verified to contain the measured native Close rectangle restored/96, maximized/96 and restored/192.

---

# Extension to other apps (experimental, off by default)

Same overlay as above, now for every window of a verified app: Edge, Chrome, Explorer, Notepad, plus apps added in config
(`globalTrafficLights.experimentalOverlay.extraApps`, Windows-drawn frames only; `excludeApps` always wins).
Rules live in `src/window/titlebar_profile.*` (pure, unit tested). `titleContains` empty = every covered window.

## What differs per app (measured with the native Close hover highlight, one disposable window each)

| App | Who draws the buttons | `CAPTION_BUTTON_BOUNDS` | Real native button strip | Profile |
|---|---|---|---|---|
| Explorer, Notepad | Windows (DWM) | 146x30 (right) | Close 45x29 (restored, 96 DPI), 46x27.5 logical at 192 | trust the bounds |
| Edge, Chrome | the app | 146x30 (Edge reported 146x28 on one window) | 40 logical px tall (Edge: 39/40/39 at restored 96/max 96/restored 192; Chrome: same) | height 40 |
| VS Code (Electron) | the app | 0 wide: no information | not measured | not covered |
| Windows Settings and other UWP | system/XAML | 0 wide | not measured | not covered |

Maximized windows: the buttons start 8 px (96 DPI) below the window top for all four apps; the overlay uses
window top + frame width.
Windows with fewer buttons (dialogs) get a narrow strip from Windows: the overlay is widened to the three-button width and
the dots for missing minimize/maximize are greyed out and ignore clicks.

## Never covered
Windows of this app, owned windows and tool windows, child windows, fullscreen/borderless windows, windows without
`WS_CAPTION | WS_SYSMENU`, apps with no verified profile, and elevated windows (Windows refuses `WM_CLOSE` and
`WM_SYSCOMMAND` from a lower-integrity process; a token that cannot be read is treated the same).

## Results
| Check | Result |
|---|---|
| Overlay over Explorer, Notepad, Chrome (restored, 96 DPI) | Fully covers the native buttons (screenshots) |
| Explorer / Notepad / Chrome: yellow, green (maximize and restore), red | Verified on disposable windows; red closes only that window and the overlay is removed |
| Window with no minimize/maximize (Windows Forms dialog opted in through `extraApps`) | Red works; yellow and green are grey and do nothing; bounds were only 49 px wide and needed the widening above |
| VS Code, Settings (UWP) with the feature on | Not covered |
| Resources with 2 covered windows (idle 10 s) | Working Set 18.8 MB, Private 38.0 MB, CPU 0.00% (35.5 MB private with the feature off, measured earlier). Not measured with many windows |

## Limits
- Same cost as for Edge: Snap Layout hover on the maximize button is lost for every covered window (Win+Z still works).
- Explorer and Notepad title bars are Mica (wallpaper tinted): the overlay is one sampled color and can look slightly different from the surrounding bar.
- Chromium apps other than Edge and Chrome (VS Code, Discord, Slack...) are not covered until each is measured.
- Not tested: Windows Terminal, Office, Qt/GTK apps, high-contrast themes, windows on virtual desktops other than the current one.
- Overlays are measured per window only on the disposable windows above; other apps may draw non-standard frames, so new apps go through the same measurement before being added to the built-in list.
- Border (fixed): a restored window has a 1 px accent border (2 px at 192 DPI). The overlay used to paint over it at the top and right. It now stays inside it (top +1 px, right edge at the frame's right edge minus the border, bottom edge unchanged) and its top-right corner follows the inner curve of the 8 px window radius. Verified by screenshot at 96 and 192 DPI on an Explorer test window.
- Color flash on maximize/restore (fixed): the background color was re-sampled 80 ms after every geometry change, while Windows still ran the maximize animation, and took the animation's grey/black (measured on an Explorer test window: 000D15 -> 5B5C5C at ~100 ms -> back at ~345 ms; 191919 on restore). Geometry changes now keep the current color and re-sample 550 ms later, once the animation is over (measured: no intermediate color, one 000D15 -> 000C18 correction at ~570 ms). Activation changes still sample at 80 ms.
- Maximize / restore motion (fixed): the real window jumps to its final rectangle at once while Windows animates a picture of it for about 0.3 s, so the overlay (a real window) used to arrive ahead of the animation. It now follows it: when the maximized state toggles, the overlay's position is driven by `mc::anim` (`easeTo` with delay) along the measured curve. Measured by capturing a bright test window (about 30 frames/s, same frame for window and overlay): the picture starts after ~70 ms, travels in ~200 ms, ease-out; fitted with cubic-bezier(0.5, 0.2, 0, 1), rmse 2% of the travel. The overlay starts `kMoveLead` = 8 ms earlier than the fit to cancel the latency of moving a window (tuned over 8 maximize/restore runs per value: lead 20 ms gave a mean overshoot of +85 px, 8 ms gave -12 px with a mean absolute error of 36 px on a 726 px travel). The timer runs only during the transition.
- Not covered by that motion: minimize/restore from the taskbar, Win+Arrow snapping and window drags (the overlay follows those through the normal location events, without the animation curve). The curve was measured on one machine and one Windows build, with animations enabled; when "Animate windows when minimizing and maximizing" is off (`SPI_GETANIMATION`), the overlay jumps with the window instead of animating (code path not exercised on this machine, where the option is on).

