# Reversed infinite depth (debug.reverse_z, default on) [C = verified in the game]

The main view's hardware depth buffer holds **near / view z** (1 at the near plane, 0 at infinity and for the cleared sky), float
(D32 on AMD), instead of the engine's D3D value z_d = f / (f - n) (1 - n / viewZ).

## Mapping
* The engine's vertex output is remapped by a linear function of its clip coordinates, so interpolation stays correct:
  `z' = w - k z`, `k = 1 - n / f` of the main camera (FrameBlock `depthParams.xy`). Then z' / w = n / viewZ exactly, whatever f is.
  Geometry beyond the engine's far plane is no longer clipped (z' >= 0 reaches far past f).
* Per draw: `DrawBlock::posFix.zw = (-k - 1, 1)` (identity (0, 0)); `z' = z (1 + zw.x) + zw.y w`. The translator (`shader/glsl.cpp`),
  `ge_default.vert`, `water_ocean.vert`, `shadow_mask.vert`, `static_mesh.vert` (reads the FrameBlock flag instead) apply it; a pixel
  shader's `oDepth` becomes `d (1 + zw.x) + zw.y`. `kTranslatorVersion` 3.
* **Per depth buffer, set by its clear**: `Device::clear` marks the buffer reversed when the clear happens in a main view
  (`Texture::reverseZ_`); every draw into a marked buffer gets the remap, flipped compare (LESS <-> GREATER, LESS_OR_EQUAL <->
  GREATER_OR_EQUAL; EQUAL unchanged) and flipped depth bias. The engine's clear to 1.0 (far) becomes 0.0 (infinity), other values
  `1 - k z`. Shadow cascades / sky cube / other views keep the engine's convention.
* The engine's far-plane trick (sky, shadow mask quad at z = 1) lands on `n / f`, so "sky" tests keep working through
  `rvkHwDepthToD3D`.

## Consumers
`FrameBlock::frameInfo.y` bit `RVK_FRAME_REVERSE_Z` (8). `rvk_common.glsl`: `rvkHwDepthToD3D(f, d)` (back to the engine's 0..1,
sky = 1) and `rvkHwDepthToViewZ`; used by GTAO / filter, screen probes, rt.debug depth capture, FSR prep (FSR still gets D3D depth:
flags unchanged), the material variants (`rvkMvD3dZ`). The native opaque pass reads `Device::reverseZ()` when its pipelines are built
(GREATER_OR_EQUAL). The linear depth map (R32F) is untouched.

## Verified
Savegame-free `water-compare` of jungle / pub_interior / sea_horizon / monastery_hall, reverse_z on vs off with the same settings:
images equal to the eye, sync validation clean, testdemo PASS. Known: the sea's horizon edge shows tiny dashes (the sea plane is no
longer far-clipped).

## Native passes
The native pre-pass / colour pass write `clip.z = near` (the exact constant: `w - k z` is that constant mathematically, but cancels to a value that differs by rounding
between shader programs) and the pre-pass a further 1e-5 farther; see native-renderer.md 14.
