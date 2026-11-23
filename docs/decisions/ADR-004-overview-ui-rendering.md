# ADR-004: Overview UI Rendering

- Status: Accepted (Phase 4)
- Date: 2026-11-23

## Context

The overview needs a backdrop, window titles and icons, hover/selection highlight with shadow, a
desktop strip and gesture-driven transitions. DWM draws every window thumbnail *above* all of the
overlay's own content (`windows-apis.md` §1.1), so nothing we draw can sit on top of a thumbnail.

## Decision

- **Direct2D into DirectComposition surfaces** (`SceneRenderer`): `DCompositionCreateDevice2` with a
  D2D device; `BeginDraw` returns an `ID2D1DeviceContext`. Static content is rasterized once when it
  changes; every frame only sets offsets, transforms and opacity.
- **Layer order** (bottom to top): backdrop, focus chrome, labels, strip; DWM thumbnails on top.
  Everything we draw is placed *outside* the thumbnail rect (labels under the card, ring and shadow
  around it, strip in a band the layout reserves).
- **Backdrop = the monitor's wallpaper** from `IDesktopWallpaper` (public), drawn "Fill", dimmed by
  `dim.opacity`, optionally blurred (D2D Gaussian blur, `blur.intensity`). It hides the real windows
  behind the overview (Phase 1 known issue) and fades in/out with the transition. Rendered at half
  resolution (it is dimmed/blurred background) with mirrored overscan on both sides so the
  desktop-switch parallax never shows an edge. Built at startup (`prepare`) and on wallpaper
  change, so opening does not decode images.
- **Parallax**: during a desktop switch the backdrop crossfades between two visuals sharing one
  surface; the old one drifts by `-dir * width * parallax`, the new one comes from `+dir`. Cards move
  a full width, the backdrop a fraction of it.
- **Labels**: one small surface per card (icon + ellipsized title, per-monitor DPI), created lazily,
  at most 4 per frame, so opening is not delayed by DirectWrite.
- **Focus chrome**: only the focused card gets a ring + soft shadow (one surface, re-rasterized when
  the focused size changes); the card scales by `hover.scale` with `hover.spring`.
- **Desktop strip**: one surface (tiles with the primary wallpaper preview, names, "+" button) plus a
  separate ring visual that springs to the current tile.
- **Gestures**: vertical scroll scrubs the exit (progress = scroll distance / 600 units, with rubber
  band); 140 ms without input = release; `GestureTracker` velocity + projection decide close vs. stay;
  the spring continues with the finger velocity. Horizontal scroll switches one desktop per swipe.

## Rejected

| Option | Why |
|---|---|
| Shadows under every card | One surface per card (or a stretched shared one with distorted blur); hover shadow only is enough and cheap. |
| Full-resolution backdrop | 4x the GPU memory for a dimmed background. |
| Drawing labels on top of the thumbnail (macOS puts the icon over the window) | Impossible: DWM thumbnails are above our content. |
| `Windows.UI.Composition` backdrop brush | Needs the WinRT compositor next to DComp; the wallpaper backdrop gives the same look. |

## Consequences

- GPU memory now includes the backdrop surfaces even when idle (measured in `phase-4.md`).
- `IDesktopWallpaper` gives the wallpaper file; slideshows, Spotlight and positions other than
  "Fill" are approximated (solid background color if the file cannot be decoded).
- The strip shows the same wallpaper preview on every tile: per-desktop wallpapers and live previews
  of other desktops' windows are not shown.
