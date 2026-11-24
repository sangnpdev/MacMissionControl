#pragma once
// Draws the three traffic-light circles with Direct2D on any render target. Shared by the overview
// chrome (SceneRenderer, DComp surface) and the Edge PoC overlay (DC render target).
#include <d2d1.h>

#include "ui/traffic_lights.h"

namespace mc {

struct TrafficButtonLook {
    float hover = 0.f;    // 0..1: enlarged, glyph visible
    float pressed = 0.f;  // 0..1: darker
    bool disabled = false;  // greyed out, no hover/press/glyph (window cannot minimize / maximize)
};

// `tl` rects are in the coordinates of `rt` shifted by (-originX, -originY). `inactive` 0..1 fades the
// colors toward grey and hides glyphs (window not active).
void drawTrafficCircles(ID2D1RenderTarget* rt, ID2D1Factory* factory, const TrafficLights& tl, double originX, double originY,
                        float dpiScale, const TrafficButtonLook look[3], float inactive = 0.f);

}  // namespace mc
