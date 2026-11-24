# ADR-009: Window tiling menu on the green traffic light

- Status: Accepted (part of the experimental app-window traffic lights, ADR-008; same switch)
- Date: 2026-11-24

## What it does

Hover the green dot of a covered window for 350 ms: a small menu opens under the strip with nine layouts. Click one and the
window goes there. Clicking the green dot itself still maximizes / restores, as before; the menu never replaces it.

| Row | Layouts |
|---|---|
| 1 | Fill (the window's own maximize), Left Half, Right Half, Top Half, Bottom Half |
| 2 | Top Left, Top Right, Bottom Left, Bottom Right |

Thirds and two-thirds are not built; they are one more row in `kFractions` (`window/layout_engine.cpp`) plus a tile in
`ui/tile_menu_layout.cpp`.

Only windows that have an overlay get the menu (ADR-008: the listed apps), and only when the window can be resized
(`WS_THICKFRAME`), is visible, not minimized and not cloaked. A fixed-size dialog keeps its greyed dot and no menu.

## Architecture

```
 EdgePoc (one per window, ADR-008)                     TilingMenu (ONE popup for the whole app)
   hover green 350 ms  --- SetTimer one-shot --->        open(anchor)   layered, WS_EX_NOACTIVATE, topmost
   mouse over dot/strip ---- anchorHover(bool) --->      closes 200 ms after the mouse left dot, strip AND menu
   focus lost / window moved / minimized / gone ---> close(immediate)
   <--- kMsgClosed (green dot goes back to where the mouse is)
                                                        pick(tile) -> close, then WindowTiler::apply (no waiting)
                                                                          verify() 150 ms later
```

| Piece | File | Notes |
|---|---|---|
| Layout math | `window/layout_engine.*` | Pure. `layoutRect` (work area -> rect, one rounding per edge, neighbours share edges exactly), `outerRect` / `scaleInsets` (invisible frame border), `fitWithin` (a window that keeps its own size), `clampInside`. |
| Menu geometry | `ui/tile_menu_layout.*` | Pure. Tiles, hit test, placement: centred under the dot, below the strip, flipped above when no room, always inside the work area. |
| Win32 side | `app/window_tiler.*` | `MonitorFromWindow` (the monitor holding most of the window), `GetMonitorInfo.rcWork` (taskbar excluded), `DWMWA_EXTENDED_FRAME_BOUNDS` vs `GetWindowRect` for the invisible border, `SetWindowPos` with `SWP_ASYNCWINDOWPOS`. |
| Menu | `app/tiling_menu.*` | State `Closed / Opening / Open / Closing`; D2D into a DIB; `mc::anim` for open (0.17 s), close (0.12 s), tile hover (0.12 s), press (0.08 s). One `MCTilingMenu` window, created on first use. |
| Dots | `app/edge_poc.*` | Unchanged geometry. New: the hover timer, `animTo()` (every dot animation goes through it), `openMenu()`, `menuClosed()`. |

State per window is the `EdgePoc` itself (`active_`, `hot_`, `pressed_`, `enabled_[]`, the animated values); the menu state is one
value, and `stateFor(hwnd)` answers `Closed` for any other window, so an active window never lights up another window's dots.

### Behaviour of the tiler

1. `canTile`, then the work area of the monitor that holds most of the window.
2. A maximized window is restored first (`WM_SYSCOMMAND SC_RESTORE` through `SendMessageTimeout`, 250 ms, abort if hung). Fill is
   `SC_MAXIMIZE` through `PostMessage`, same as the green button.
3. The invisible border is measured on the window and scaled to the DPI of the monitor it is going to.
4. `SetWindowPos` (async: a hung app never blocks us). Nothing is written into the other process.
5. 150 ms later `verify()`: if the size is wrong (a DPI change after a cross-monitor move) it applies once more; if the window
   keeps its own min / max size it is centred on its tile and kept inside the work area.

Mixed DPI works because everything is in physical pixels and the work area / DPI come from the monitor, not from a constant.

### Reduced Motion

`SPI_GETCLIENTAREAANIMATION` (Windows "Animation effects") is read each time an animation starts, so no event subscription is
needed; the user's own switch is `"animations": false` in `apps.json` or in `config.json` under
`globalTrafficLights.experimentalOverlay`. Either one turns every dot and menu animation into an immediate jump (`AnimatedValue::jump`);
actions never wait for animations in any case.

## Measured (Windows 11 26200, `benchmarks\tiling-menu.ps1`, one overlay, 16 logical CPUs)

| | Result |
|---|---|
| Idle CPU (10 s) | 0.00% |
| Mouse sweeping over the dots for 15 s (no menu opens) | 0.36% to 0.54% of the machine |
| 60 menu open / close cycles | 0.15% to 0.35% |
| Private bytes over 60 cycles after warm-up / over the next 60 | +0.4 to +0.5 MB / +0.0 MB (plateau) |
| Handles over the next 60 cycles, GDI, USER objects | +1, 0, 0 |
| Click on the green dot with the menu open -> window maximized | 9 to 32 ms (timer resolution), median 16 ms |
| Menu delay | 350 ms by design (measured 265 to 330 ms from the first pointer movement, as hover starts earlier) |

## Tested on this machine (disposable windows: `mc_testwin`, a WinForms form; no window of the user was moved)

This machine has two monitors with different DPI (1920x1080 at 96 DPI and 2880x1800 at 192 DPI, to its left).

- All nine layouts on `mc_testwin`, on the primary monitor and on the 192 DPI monitor: the visible frame ends exactly on the
  computed rect (tolerance 1 px). From a maximized window as well.
- A window straddling both monitors: it is tiled on the monitor that holds most of it.
- A window with a minimum size larger than the tile (WinForms `MinimumSize`): it keeps its size, is centred on the tile and stays
  in the work area. A fixed-size window: no menu.
- Menu: opens after the delay, stays open while the mouse crosses the gap, closes when the mouse leaves; the window losing focus
  closes it at once; closing the window with the menu open leaves no menu; quick passes over the dot do not open it; only one
  menu window exists.
- Close, minimize, maximize and restore through the dots still work with the menu open (clicks measured above).

Not tested: unplugging a monitor while a menu is open (the work area is read each time the menu opens and a tile is applied, never
cached), high-contrast themes, windows of elevated processes (never covered), Reduce Motion toggled in Windows settings (the
query is `SystemParametersInfo`; the effect was tested through the `animations` switch, which takes the same code path).

## Limits that stay

- A DPI-unaware window (old WinForms / console hosts) whose invisible border would reach a monitor with another DPI is rescaled
  wildly by Windows (measured: 974 px asked, 4792 px got). Its outer rect is cut to its own monitor, so the visible frame can start
  a few pixels inside the screen edge there. Measured with a PowerShell WinForms form.
- Windows with their own min / max size end up as large as they allow, not as the tile.
- The menu is a topmost popup of this process: a full-screen exclusive window above it hides it.
- Snap Layouts of Windows 11 stay unavailable on covered windows (ADR-008); this menu is the replacement.
