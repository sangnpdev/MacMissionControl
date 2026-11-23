# System Design

## Goal in one paragraph

A resident, single-process native app. While idle, it holds no previews, runs no timers and
uses no GPU memory beyond an empty D3D/DComp device. On trigger, it enumerates windows, plans an
overview layout, registers DWM thumbnails into one full-screen overlay window and animates them
with a frame loop paced by the DWM compositor clock. On exit, it animates back, activates the
chosen window, releases every thumbnail, and returns to idle.

## Key constraint that shapes everything

DWM thumbnails are the only zero-copy, public way to show live window content. They are not
composition visuals (see `docs/technical-research/windows-apis.md` §1.1). Moving them requires
`DwmUpdateThumbnailProperties` every frame. So:

1. There is **one** animation engine: ours. It is pure math (easing, springs, stagger),
   evaluated once per compositor frame, only while a transition runs.
2. The same engine also drives the DComp chrome (dim layer, later highlights). Thumbnails and
   chrome stay on the same curve and the same clock, with no drift between two engines.
3. The frame loop runs on a dedicated **compositor thread**. The UI thread never blocks on a
   frame and never runs the animation.

## Process layout

```
                 +-------------------- UI thread ---------------------+
 hotkey / input  |  App  ->  WindowManager (enumerate, filter, bounds) |
 WinEvent hooks  |       ->  LayoutEngine (pure)                      |
                 |       ->  OverlayWindow (HWND, hit-testing, keys)  |
                 +------------------------|---------------------------+
                                          | commands (mutex + event)
                                          v              ^ PostMessage(done)
                 +---------------- compositor thread ------------------+
                 |  FrameDriver: wait DCompositionWaitForCompositorClock|
                 |    -> Timeline (animation engine, pure)              |
                 |    -> PreviewEngine (DWM thumbnails: own lifecycle)  |
                 |    -> Chrome (D3D11 + DComp: dim visual)             |
                 +------------------------------------------------------+
```

Ownership rules:

- The compositor thread exclusively owns every `HTHUMBNAIL`, the D3D11 device and the DComp
  device. They are created, updated and released on that thread only.
- The UI thread owns all `HWND`s and the window list. It sends immutable `Scene` snapshots
  (target HWND, real rect, overview rect, source rect) to the compositor.
- The compositor reports `TransitionFinished` back via `PostMessage`. The UI thread then
  activates the chosen window and hides the overlay.

## Idle behaviour

- The compositor thread waits on an event (`WaitForSingleObject`, infinite). CPU use is 0.
- The overlay window is hidden. No thumbnails are registered.
- No polling anywhere. Hotkey is `RegisterHotKey`. Window changes come from `SetWinEventHook`,
  only while the overlay is visible.

## Overlay window

- One borderless, topmost, `WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW` popup that covers the
  virtual screen. Coordinates are physical pixels (Per-Monitor-V2).
- Background is a DComp visual, a 1x1 black surface scaled to the overlay, with animated
  opacity (the dim). Transparent areas show the real desktop.
- Thumbnails are drawn by DWM above the DComp content.
- `WS_EX_TOOLWINDOW` keeps the overlay out of Alt+Tab and out of our own enumeration.

## Transitions

| Transition | Thumbnails | Dim |
|---|---|---|
| Entrance | real rect → overview rect; spring; stagger by Z-order | 0 → `dimOpacity`, ease-out |
| Exit | overview rect → real rect (selected window last in Z-order, so on top) | → 0, ease-in-out |
| Window closed while open | removed; the rest relayout with the same spring | unchanged |

Minimized windows have no on-screen real rect. They enter with opacity 0 → 1 at their overview
slot, scaled from 0.85. The selected minimized window exits to its restored rect
(`rcNormalPosition`, converted from workspace to screen coordinates).

## Failure handling

- Any HRESULT failure is logged with `diagnostics::Log` (file + `OutputDebugString`). Nothing is
  swallowed silently.
- Thumbnail registration failure for one window: that card is skipped and logged. The overview
  still opens.
- D3D/DComp device creation failure: the overview runs without dim (thumbnails still work).
- Device loss (`DCompositionWaitForCompositorClock` error, D3D removed): rebuild the chrome
  on the next open.
- Shutdown order: stop the compositor thread (releases thumbnails and devices) → destroy the
  overlay window → unregister the hotkey → uninitialize COM.

## Planned extensions (not in Phase 1)

- Virtual desktop strip (`VirtualDesktopProvider`, Phase 3).
- Hover/selection chrome, titles and icons as DComp visuals (Phase 4).
- Win+Tab takeover through a low-level keyboard hook on its own thread (Phase 5).
- JSON configuration (Phase 2). The `AnimationConfig` struct and its validation exist from
  Phase 1, so loading is the only missing step.
