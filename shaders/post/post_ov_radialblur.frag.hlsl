// S-7c (frame-ownership.md 14.16): overlay_color.fx OverlayRadialBlur (OverlayHit; post_overlay.hlsli)
#include "post_overlay.hlsli"

RvkPsOutOverlay PSMain(RvkPsInOverlay i) {
    RvkPsOutOverlay o;
    o.oC0 = rvkOverlayRadialBlur(i);
    return o;
}
