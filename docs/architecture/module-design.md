# Module Design

Pure modules have no Win32 dependency and are unit-tested. Platform modules wrap Win32/DWM/DComp.

| Module | Dir | Kind | Responsibility |
|---|---|---|---|
| Geometry | `src/core/geometry.h` | pure | `Rect` (double), lerp, aspect-fit |
| Animation | `src/animation/` | pure | Bézier, easing presets, analytic spring, `AnimatedValue`, `VisualMotion`, gesture tracker, stagger, parallax, rubber band |
| Layout | `src/ui/layout.*` | pure | Overview layout per monitor: aspect-preserving, non-overlapping rows |
| Window filter | `src/window/window_filter.*` | pure | Decide whether a window appears, from a `WindowFacts` struct |
| Config | `src/config/*` | pure | JSON reader, `AnimationConfig` defaults, strict loader, validation |
| Demo | `demo/animdemo.cpp` | platform | One panel per animation primitive (Direct2D), live config reload |
| Win+Tab detector | `src/input/wintab_detector.*` | pure | Key-event state machine: pass / swallow / trigger (ADR-005) |
| Keyboard hook | `src/input/keyboard_hook.*` | platform | `WH_KEYBOARD_LL` thread, bridge chord Ctrl+Alt+F24, re-install |
| Desktop state | `src/desktop/desktop_state.*` | pure | Desktop ids/names/current, registry blob parsing, switch steps |
| Desktop provider | `src/desktop/virtual_desktop_provider.*` | platform | Registry read + change event, shortcut switch/create/remove (ADR-003) |
| WindowManager | `src/window/window_manager.*` | platform | Enumerate, collect `WindowFacts`, bounds, monitor, activation |
| Preview | `src/preview/thumbnail.*` | platform | RAII `HTHUMBNAIL`, property updates |
| Rendering | `src/rendering/scene_renderer.*` | platform | D2D-in-DComp scene: wallpaper backdrop, labels, focus chrome, desktop strip (ADR-004) |
| Compositor | `src/rendering/compositor_thread.*` | platform | Frame loop, scene ownership, timeline evaluation, frame stats |
| Overlay | `src/app/overlay_window.*` | platform | Creates the overlay HWND; virtual-screen rect |
| App | `src/app/app.*`, `src/app/main.cpp` | platform | Lifecycle, state machine, hotkey, hit-testing, keyboard, mouse, CLI flags, bench mode |
| Diagnostics | `src/diagnostics/log.*` | platform | Log to file and debugger |

## Animation engine (pure)

See ADR-002 for the reasoning.

```cpp
struct CubicBezier { double x1, y1, x2, y2; double operator()(double x) const; };
struct SpringParams { double stiffness, dampingRatio, mass, initialVelocity; };
class Spring {            // closed form, any from/to/v0; settle time from the decay envelope
    Spring(SpringParams, double from, double to, double v0, double eps);
    double value(double t) const;  double velocity(double t) const;  double settleTime() const;
};
class AnimatedValue {     // retargetable scalar on the compositor clock (absolute seconds)
    void jump(double v, double velocity = 0);                       // gesture: follow the finger
    void springTo(double target, double now, const SpringParams&, double eps, double delay = 0);  // keeps velocity
    void easeTo(double target, double now, double duration, CubicBezier, double delay = 0);
    double value(double now) const;  double velocity(double now) const;  bool settled(double now) const;
};
class VisualMotion {      // translation + uniform scale + opacity; aspect ratio exact
    void jump(const Rect&, double opacity);
    void springTo(const Rect&, double opacity, double now, const SpringParams&, double delay = 0);
    Rect rect(double now) const;  double opacity(double now) const;  bool settled(double now) const;
};
class GestureTracker {    // input -> progress, velocity (least squares, 100 ms), commit/cancel
    void begin(double t, double raw);  void update(double t, double raw);
    double progress() const;  double velocity() const;  Release release(double projection) const;
};
double staggerDelay(size_t index, size_t count, double step, double maxTotal);
double parallaxOffset(double progress, double distance, double depth);
double rubberBand(double x, double lo, double hi, double range);
// Curve / Track (0 -> 1 progress) remain for simple one-shot use.
```

Everything is a function of elapsed time, so the motion is identical at 60, 120 and 240 Hz and a
dropped frame does not slow the animation.

Card animation in the app: each card owns a `VisualMotion`. Entrance, exit and relayout all call
`springTo` from the current state, so any interruption keeps position and velocity continuous.
The dim is an `AnimatedValue` with a timed Bézier.

Gesture-driven transitions: while the finger moves, `jump(progress, velocity)`; on release,
`springTo(target)` continues with the finger's velocity. The app gets gesture input in Phase 4;
the demo exercises the model now.

## Configuration

`%LOCALAPPDATA%\MissionControl\config.json` (or `--config PATH`). Schema and defaults:
`resources/config.default.json`. All keys are optional. Unknown keys, wrong types and out-of-range
values are errors with their key path; a file with any error is rejected and the defaults are used.

## Layout (pure)

Input: work-area rect, spacing, list of window sizes (with original centers). Output: one
target rect per window.

Algorithm: sort windows by original center (y, then x). For each row count `r` in `1..n`, split
the list into `r` rows of near-equal count. Compute the uniform scale at which every row fits
the width and all rows fit the height. Cap the scale at `maxScale`. Keep the `r` with the
largest scale, then center each row and the block. Aspect ratio is preserved exactly. Cards never
overlap (spacing ≥ 0). O(n²), with n ≤ ~100.

## Window filter (pure)

`WindowFacts { visible, cloaked, toolWindow, appWindow, noActivate, hasOwner, ownProcess,
onCurrentDesktop, width, height, className, title }` → `bool include(const WindowFacts&, const
WindowFilterRules&)`. `WindowFilterRules` carries the minimum size and a class-name deny list
(defaults include `Progman`, `WorkerW`, `Shell_TrayWnd`, `Shell_SecondaryTrayWnd`) plus an
optional class allow list (used by benchmarks to isolate test windows).

## Overlay input (Phase 1)

| Input | Action |
|---|---|
| Hotkey `Ctrl+Alt+Space` | toggle overview |
| Click on card | exit to that window and activate it |
| Click on background / `Esc` | exit to the previously active window |
| `Enter` | exit to the previously active window |
| `Ctrl+←` / `Ctrl+→` | previous / next virtual desktop (Phase 3) |
| `1`–`9` | go to desktop N, one step at a time (Phase 3) |
| `Ctrl+N` | create a desktop and switch to it (Phase 3) |
| Mouse over a card | focus it: ring, shadow, `hover.scale` (Phase 4) |
| `←` `↑` `→` `↓` | move the focus to the nearest card in that direction (Phase 4) |
| `Enter` | open the focused card, or go back to the previous window if none (Phase 4) |
| Click a strip tile / `+` | switch to that desktop / create one (Phase 4) |
| Vertical scroll (two-finger swipe, wheel) | scrub the exit; release decides close vs. stay (Phase 4) |
| Horizontal scroll | previous / next desktop, one per swipe (Phase 4) |

## Live window and desktop tracking (Phase 3)

While the overview is open, WinEvent hooks on destroy/show/hide and cloak/uncloak (top-level
windows only) restart a 100 ms timer; when it fires, the window list is enumerated again and, if the
set changed, the compositor relayouts. The debounce matters: a desktop switch cloaks every window
~8 ms before the registry names the new desktop, and a new window flaps cloaked/uncloaked while the
shell adopts it. A registry change event (in the main loop, `MsgWaitForMultipleObjectsEx`) records
the switch direction, so the following relayout slides: cards of the old desktop move out and fade,
the new desktop's cards move in, using `desktopTransition.duration/easing`. Parallax needs a
background layer and arrives with the desktop strip (Phase 4).

## CLI

| Flag | Meaning |
|---|---|
| (none) | resident; hotkey toggles the overview |
| `--show` | open the overview once at startup |
| `--bench N --csv PATH [--class NAME]` | run N open/close cycles, write per-frame CSV, exit |
| `--probe-hotkeys` | try registering Win+Tab, log the result, unregister, exit |
| `--config PATH` | load animation config from PATH instead of `%LOCALAPPDATA%\MissionControl\config.json` |
| `--no-trim` | keep the working set when idle (diagnostics) |
| `--desktop next\|prev\|new\|N` | send a desktop command to the running instance |
| `--no-takeover` | do not take over Win+Tab (config: `integration.takeoverWinTab`) |
| `--bench-seconds S` | stop a benchmark run after S seconds (soak) |
| `--hook-accept-injected` | tests only: synthetic Win+Tab triggers the hook |
| `--crash-test` | crashes on purpose to verify the crash dump handler |
