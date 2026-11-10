// S-7c (frame-ownership.md 14.16): post_dof.fx DoF fused with ToneMap: the in-focus texel and the 5-tap cross tone-mapped from the HDR scene here (post_dof.hlsli)
#define RVK_POST_FUSED 1
#include "post_dof.hlsli"

RvkPsOutPost PSMain(RvkPsInDof i) { return rvkDof(i); }
