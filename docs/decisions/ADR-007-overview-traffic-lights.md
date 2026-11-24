# ADR-007: Traffic lights on overview cards

- Status: Accepted
- Date: 2026-11-24

## Context

The request was macOS traffic lights for all windows. Windows has no supported way to hide or
replace another process's caption buttons without injecting code or subclassing its window
procedure (both ruled out). A top-left overlay on a third-party window would duplicate the native
buttons or cover tabs and icons. The `DWMWA_CAPTION_BUTTON_BOUNDS` attribute only reports geometry.
A global overlay was therefore not built.

## Decision

Show close / minimize / zoom circles under the hovered (or keyboard-selected) overview card, inside
our own overlay. Config: `trafficLights.enabled|diameter|spacing`.

- `src/ui/traffic_lights.*`: pure geometry and hit test (12 px circles, 20 px pitch, 20x20 hit
  targets, DPI scaled, no buttons on cards too narrow for three).
- `SceneRenderer::setTrafficLights`: one small DComp surface, re-rasterized only when the hovered
  card or button changes. DWM thumbnails draw above DComp, so the row sits in the label row below
  the card, never over the thumbnail. The title fades while it would collide with the circles.
- Actions go to the card's exact HWND: `WM_CLOSE`; `WM_SYSCOMMAND` `SC_MINIMIZE` (card removed, others
  keep their slots through the stable layout); `SC_MAXIMIZE` / `SC_RESTORE` then the overview closes on that window.
- Hover keeps the card focused while the mouse is in the button zone.

## Limits

- Not tested by clicking (would close real windows): verified by unit tests, and by screenshot of hover + glyph states.
- Elevated windows ignore `WM_SYSCOMMAND`/`WM_CLOSE` from a non-elevated process (UIPI).
- Zoom closes the overview toward the window's old rect; the window then jumps to its maximized size.
- No RAM/CPU benchmark was run.
