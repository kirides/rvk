// S-7c (frame-ownership.md 14.16): overlay_color.fx OverlayRadialColor (post_overlay.hlsli)
#include "post_overlay.hlsli"

RvkPsOutOverlay PSMain(RvkPsInOverlay i) {
    RvkPsOutOverlay o;
    o.oC0 = rvkOverlayRadialColor(i);
    return o;
}
