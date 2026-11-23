# ADR-002: Retargetable Animation Values and JSON Configuration

- Status: Accepted (Phase 2)
- Date: 2026-11-23

## Context

Phase 1 animated each card with one progress curve from a fixed start rect to a fixed end rect.
When a transition was interrupted (open, then close at once; a relayout during the entrance), the
new curve started with zero velocity. That produces a visible jolt. The spring's settle time was
found by sampling 10,000 points per spring, which is too slow when every card is retargeted at once.

The brief also requires configurable animation (duration, easing, stiffness, damping, initial
velocity, scale, opacity, stagger, parallax, dim, blur) loaded from JSON and validated.

## Decision

1. **Spring in absolute units with analytic settle time.** `Spring(params, from, to, v0, eps)`
   solves the damped oscillator in closed form. The settle time comes from the decay envelope
   (`|A|+|B|` for under-damped, a half-rate bound for critical, the slow root for over-damped). It
   is never early and is within 1.8× of a brute-force scan (unit test
   `spring_settle_time_envelope_is_tight`).
2. **`AnimatedValue`**: a scalar that can be retargeted at any time. A spring retarget launches
   with the current velocity, so position and velocity stay continuous. Timed Bézier motion is also
   supported but does not carry velocity.
3. **`VisualMotion`**: center x, center y, uniform scale and opacity, each an `AnimatedValue`.
   One scale channel (not separate width and height) keeps the aspect ratio exact during every
   transition, including interrupted ones.
4. **Gesture model**: `GestureTracker` maps input directly to progress, estimates velocity by
   least squares over the last 100 ms, rubber-bands past the ends, and decides commit or cancel from
   a short velocity projection. On release, the value jumps to the finger position with the finger
   velocity and springs to the target.
5. **JSON**: a small in-house reader (`src/config/json.*`, ~250 lines, RFC 8259 grammar, UTF-8,
   surrogate pairs, depth limit, line:column errors). The loader is strict: unknown keys and wrong
   types are errors, missing keys keep defaults, and a file with any error is rejected as a whole.

## Alternatives considered

| Option | Why not |
|---|---|
| nlohmann/json, RapidJSON, Boost.JSON | Would be vendored or fetched for one small config file. The in-house reader is fully unit-tested, and a library can replace it behind the same `loadAnimationConfig` if needs grow (for example writing settings from a UI). |
| Per-frame numerical integration (semi-implicit Euler) | Depends on frame rate and accumulates error. Closed form gives the same curve at 60, 120 and 240 Hz. |
| Composition (DComp/WinRT) spring animations | Still cannot drive DWM thumbnails (ADR-001). |
| Width and height as separate springs | Aspect ratio drifts when the channels start with different velocities. |

## Consequences

- An interrupted transition keeps position and velocity continuous (unit test
  `animated_value_retarget_preserves_velocity`).
- Retargeting is cheap: one closed-form setup per channel, no sampling.
- Config errors never crash the app and never apply half a file; each error is logged with its key path.
- `hover`, `desktopTransition` and `blur` settings are validated and used by the demo now; the app
  uses them when the matching UI exists (Phase 3/4).
