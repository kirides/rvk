// S-7c (frame-ownership.md 14.16): post_dof.fx DoF (the back-buffer draw) on our own pixel shader (reads the ToneMap image; post_dof.hlsli)
#include "post_dof.hlsli"

RvkPsOutPost PSMain(RvkPsInDof i) { return rvkDof(i); }
