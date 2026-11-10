// S-7c (frame-ownership.md 14.16): post_dof.fx DoFDown on our own pixel shader (reads the ToneMap image; post_dof.hlsli)
#include "post_dof.hlsli"

RvkPsOutPost PSMain(RvkPsInDofDown i) { return rvkDofDown(i); }
