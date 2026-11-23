# ADR-001: Technology Stack

- Status: Accepted (Phase 0)
- Date: 2026-11-23

## Context

The app must show live previews of many windows, animate them smoothly at the display refresh
rate, stay under 30 MB idle and 100 MB active, and use about 0% CPU when idle. The toolchain
found on the machine is MSVC 14.50, CMake 4.2, Ninja, and Windows SDK 10.0.26100. Only the
Windows App SDK runtime is installed, not its development package.

## Decision

| Concern | Choice | Rejected alternatives |
|---|---|---|
| Language / build | C++20, MSVC, CMake + Ninja | C++23 (no feature needed yet) |
| Previews | DWM Thumbnail API | WGC (one or more textures per window, breaks the GPU budget); `DwmpCreateSharedThumbnailVisual` (undocumented) |
| Animation | One in-house, pure, time-based engine (Bézier + analytic spring), evaluated per compositor frame on a dedicated thread, only during transitions | Composition animations for everything: they cannot move DWM thumbnails, so a second engine would still be needed and the two would drift |
| Frame pacing | `DCompositionWaitForCompositorClock` (Windows 11), fallback `DwmFlush` | timers (`SetTimer`, multimedia timer): not vsync-aligned, wake up while idle |
| Chrome (dim, later highlight/labels) | DirectComposition + D3D11 | `Windows.UI.Composition` (deferred to Phase 4 if effects are needed); WinAppSDK `Microsoft.UI.Composition` (needs NuGet + bootstrapper, no gain) |
| Virtual desktops | Phase 3: registry read + `SendInput` shortcuts as the always-available provider; internal COM only behind a build allow-list | byte-pattern scanning, hard-coded offsets (forbidden) |
| Win+Tab | Phase 5: `WH_KEYBOARD_LL` hook, opt-in, on its own thread | registry/policy changes, Explorer patching |
| Tests | Self-contained minimal test runner (`tests/test_main.h`) under CTest | GoogleTest: adds a network fetch or vendored dependency for ~30 assertions today. Revisit if the suite grows |
| Config | `AnimationConfig` struct + validation now; JSON loading in Phase 2 (library choice recorded then) | — |

## Consequences

- A CPU frame loop exists, which the brief prefers to avoid. It is justified because DWM
  thumbnails have no public GPU-animatable form. Cost: one `DwmUpdateThumbnailProperties` per
  card per frame, during transitions only (about 0.3–0.5 s). Measured in Phase 1.
- The engine is frame-rate independent (closed-form time functions). Dropped frames do not
  slow the animation.
- Idle cost is near zero: the thread is blocked, the overlay is hidden, no thumbnails exist.
- Effects that need GPU shaders (blur) are not available in DComp without our own D3D work.
  Phase 4 decides between `DWMWA_SYSTEMBACKDROP_TYPE` on the overlay and `Windows.UI.Composition`.
