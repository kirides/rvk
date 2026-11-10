// S-7c (frame-ownership.md 14.16): post_dof.fx DoFDown fused with ToneMap: its 16 colour texels tone-mapped from the HDR scene here (post_dof.hlsli)
#define RVK_POST_FUSED 1
#include "post_dof.hlsli"

RvkPsOutPost PSMain(RvkPsInDofDown i) { return rvkDofDown(i); }
