# Full mode optimization plan (2026-10-28)

Inputs: `opt-profile.md` (where Full's CPU goes), `opt-side-effects-mesh.md`, `opt-side-effects-anim.md`, `opt-side-effects-frame.md`
(what the engine's render code does besides drawing). All side-effect findings are from code reading, not yet checked in game.

## Where the time goes (savegame Risen_Save_0009, uncapped, unprofiled)

Wrapper 3.30 ms CPU / frame, Full 8.52 ms (+5.2). GPU equal (1.5 ms). Engine.dll self time is the same in every mode: the engine
does no extra work in Full. Of Full's extra (profiled +5.7 ms):

| bucket | ms | removed by |
|---|---|---|
| our composition (`constantsInto` bit-by-bit masks 1.0, per-op pool re-read 0.3-0.4, SEH-guarded reads 0.4, CRT copies, heap, texture lookups) | ~3.9 | O1 |
| our issue path: per-op whole device state save / restore (~0.6), one setter per register (~0.7) | ~1.3 | O1 (setters, delta restore), O2 (no restore) |
| engine slot code + its HAL calls, dropped draws | ~1.3 | O2 |
| tap, attribution, coverage, second qsort | 0.3 | O1 (sort once), O2 |

Cost follows composed draws (~5-6 us each vs ~1.1 us per Wrapper draw), not the kind.

## Stages

**O1 - our own costs, engine still running** (verifiable per draw with the existing compare modes: 0 mismatches must hold):
word-wise live register masks; constants written straight into the draw packet; validate an engine object once per op then plain reads;
our own persistent constant register file and per-effect value store instead of re-reading the engine's pools; heap / buffer reuse;
cached texture-of-API lookups; per-op state save / restore reduced to the delta we changed; the double sort removed (Execute's qsort
sites redirected to sort-then-snapshot); coverage counter behind a debug switch. Target: Full ~ Wrapper + ~1.5 ms.

**O2 - lean executor** (the engine's render code no longer runs): our executor replaces `Execute`, `ExecuteShadowMap`, the PSSM loop at
the existing detours, walks the engine's live sorted lists (one in-place sort with the engine's qsort / comparators), calls the
engine's cheap brackets at Execute's places (camera, fog, Begin/EndZPass, Begin/EndMaterial, sampler holders, ClearCells, module
OnEndRenderSolid) and reverts its own state at each bracket end; runs the extracted side effects per reached op in op order:
resource touches (`eCResource2::RequestData(res,1)` for every image / mesh our draws bind; lightmaps), actor palette
(`UpdateRenderMatrices`), SpeedTree queue sort + stamp reset + MaskReference 80, vegetation buffer cursor, particles (master
instance, no-live-particle simulation, inactivity timer reset, in-place depth sort, buffer / material / image creation), strip and
streak buffer creation, billboard queue reset, sky cloud scale / scroller direction / cloud colour fields, stream-3 flag, Bink
textures; ops not ready (permutation not compiled, buffers missing) run the engine's slot after a bracket pair resets its caches.
Shadow views execute at their own `ExecuteShadowMap` (before the next Request). Culling / extraction stay whole (gameplay reads the
visible stamp: camera admin, AI). Verification: `native.lean_check=N` runs the full engine path every Nth frame and compares;
behaviour checks (no texture eviction of bound images, streaming after teleports, particles keep simulating, AI visibility, focus);
same-frame switch images.

**O3 - effects and GUI lean**: replace `ExecuteEffect` / `eCSprite::Flush` fully (graph walk, RT pool, CFXGui, Game.dll draw
callbacks stay); the focus indicators' draws (Game.dll `gCFocusCrossHairIndicator2`, `gCFocusScreenPointIndicator2`) composed.

**Then own modes** (A/B against Full): retained composition (cache composed draws across frames, only dynamic constants per frame),
merged indirect draws, GPU culling, the ubershader, bindless materials.
