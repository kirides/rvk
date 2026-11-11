// S-7c (frame-ownership.md 14.16): overlay_color.fx OverlayMovieStrips / OverlayMovieStripsAbs: the letterbox bars (post_overlay.hlsli)
#include "post_overlay.hlsli"

RvkPsOutOverlay PSMain(RvkPsInOverlay i) {
    RvkPsOutOverlay o;
    o.oC0 = rvkOverlayStrips(i);
    return o;
}
