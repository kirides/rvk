# Text rendering: engine path, our implementation, the "character screen text breaks" bug

User report: in Vulkan mode the text in the character screen (key C) "breaks after a short while"; with D3D9 it is fine.
All addresses are Engine.dll RVAs. **[C]** confirmed (decompiled / our code read), **[I]** inferred.
Dumps: `B:\DevDrive\risen-ghidra\rvk_fonts_dump.txt`, `rvk_fonts_dump2.txt`, `rvk_fonts_dump3.txt`.

**Answer (confirmed in game, fixed 2026-10-21)**: H1. Our `d3d9::Texture` threw away its CPU copy after the first upload.
The engine's font glyph atlas is a managed texture that keeps getting partial `LockRect` updates as new characters show up.
The first new glyph after the atlas was drawn once therefore went into a zero-filled copy, and the whole level was uploaded:
every glyph rasterised before was erased. Text in that font turned invisible, except for the glyphs added since. See
section 4 for the evidence and the fix.

## 1. How the engine draws GUI text [C]

Risen Enhanced does **not** use `ID3DXFont` / `eCAPIFont` / `eCAPISprite` for GUI text. The DX9 objects (`eCDX9Font`,
`eCDX9Sprite`, the debug font/sprite in the `eCDX9Device` ctor) are leftovers. The text path is the Enhanced edition's own
code: stb_truetype rasterisation, a glyph atlas texture, and a sprite batcher on a dynamic vertex buffer.

```
eCGuiDevice2::DrawTextW 0x518150 -> eCGuiSystemFont2::DrawTextW 0x540ef0 (EnsureFont 0x53de30 -> new eCFont + eCFont::Create)
  -> eCTextRenderer (0x529010 / 0x529310 / 0x529720): per character
       eCFont::CharToGlyphDescW 0x521ba0   (vtable 0x975628 +0x18): glyph cache lookup; on a miss -> PreloadGlyph
       eCFont::DrawGlyph 0x521c50          (+0x20): eCSprite::DrawQuad(sheet image, glyph rect, ...) = queue a sprite
eCGuiDevice2::EndFrame 0x517130 -> eCSprite::Flush 0x757d10 -> RenderBatches 0x757180 -> DrawBatch 0x7570b0 -> eCSpriteMesh::Draw
```

* **Font** `eCFont` (0x270 bytes): two `eCFnt` (stb_truetype; `+0x08` the requested face, `+0xf0` an Arial fallback), a
  glyph hash map at `+0x1d8`, and an `eCGlyphSheetCollection` at `+0x258` (sheet array `+0x260`, count `+0x268`).
  `eCFont::Create` 0x521780 preloads `" a-zA-Z0-9 !?.,;:_-+=#@()[]{}<>"` through `PreloadTextW` 0x521af0. Every other
  glyph (umlauts, `ß`, `/`, `%`, `'`, `"`, `&`, `*`, ...) is rasterised **lazily the first time it is drawn**, during GUI
  drawing, through `CharToGlyphDescW` -> `PreloadGlyph` 0x521220.
* **Atlas** `eCGlyphSheetCollection::AppendGlyph` 0x5284c0. With no sheet yet it creates one: `new eCGlyphSheet`, then
  `eCGlyphSheet::Create(256, 256)` 0x528220 -> `eCGfxImageData::CreateLayers` 0x751fc0 -> **`eCAPIDevice::CreateTexture`
  (slot 6)** with `eSGfxLayersDesc{256, 256, depth 0, levels 1, usage 0, format 21 A8R8G8B8, pool 1 MANAGED}`.
  Otherwise it always appends to **sheet 0**: there is never a second sheet (see H3).
* **Glyph upload** `eCGlyphSheet::CopyGlyphToTexture` 0x527ef0 calls **`eCAPITexture::LockRect(level 0, &lr, &cellRect,
  flags 0)` (slot 0)** directly on the sheet's API texture (`image+0x60`). It writes the glyph through the 0x527dc0 overload
  as `alpha << 24 | 0xFFFFFF` at `lr.bits + (tmAscent - originY) * pitch + max(0, originX) * 4`. D3D9 semantics: `bits`
  points at the rect's top-left. Then `eCGfxImageData::UnlockRect` 0x753f30 -> **`UnlockRect(0)` (slot 1)**. Only the cell
  rect is written; the rest of the sheet must keep its earlier glyphs.
* **Sprite batcher** `eCSprite` + `eCSpriteMesh` (at `sprite+0xb8`), capacity 0x1000 quads (`eCSprite::Begin` 0x757ff0):
  * VB: `CreateVertexData{length 0x1000*0x70, usage 0x200 DYNAMIC, pool 0, stride 0x1c}` (0x528910).
  * IB: `CreateIndexData{0x1000*12, usage 0x200, INDEX16, pool 0}`, filled **once** with a DISCARD lock (0x528a80).
  * `RenderBatches` locks **one quad per Lock**: `LockBuffer((start+count)*0x70, 0x70, start+count == 0 ? DISCARD : NOOVERWRITE)`.
    When the buffer is full it draws, zeroes `start` and `count` (8-byte store at `+0xd8`) and starts again with DISCARD.
    `End` / `EndFrame` zero them too, so every GUI frame starts with a DISCARD.
  * `DrawBatch` binds a new texture whenever the sprite texture changes:
    `eCGfxEffect::AttachImmImageData` + `Execute` on the sprite effect (HLSL source, `docs/research/fx2.md`).
    `eCSpriteMesh::Draw` 0x528650 issues `DrawIndexed(TRIANGLELIST, prims count*2, minIndex start*4, numVertices count*4,
    startIndex start*6, base 0)`.
* HAL slots involved: Device `CreateTexture`, `CreateVertexData`, `CreateIndexData`, `CreatePrimititve`, `CreateEffect`;
  Texture `LockRect`/`UnlockRect`; VertexData/IndexData `LockBuffer`/`UnlockBuffer`; Primitive `Execute`; Effect
  `SetTexture` and techniques. **Not involved**: `eCAPIFont`, `eCAPISprite`, `DrawTextA`, `LoadSurfaceFromSurface`, render-to-texture.

What is different in the character screen [I]: lots of text in several font sizes. Every `eCGuiSystemFont2` / size has its
own `eCFont` and its own sheet. The strings contain characters outside the preload set: German umlauts and `ß` in attribute
and skill names, `/` in "x/y" values, `%`, and so on. These are rasterised lazily, often in a later frame than the one where
the sheet was created and first drawn: when a tooltip, the hover text or a value appears.

## 2. Hypotheses, ranked

### H1 (confirmed, fixed): partial `LockRect` on a managed texture after its first upload erases the atlas [C code, C in game]

(Line numbers below refer to the code before the fix.)

* `Texture::upload` (`src/d3d9/resources.cpp:189-192`) frees the level's CPU copy after uploading it. This happens whenever
  `pool != SYSTEMMEM && !(usage & DYNAMIC)`. The glyph sheet (pool 1, usage 0) qualifies.
* The next `Texture::lock` (`:95`) calls `cpuLevel` (`:72-81`). That function re-creates the level **zero-filled** and returns
  a pointer into it at the cell rect (`:97-106`). The engine writes the one new glyph into it. `upload` (`:157-195`) then
  copies the **whole level** to the image: every glyph rasterised before becomes `0x00000000` (transparent).
* D3D9 keeps a managed texture's system-memory copy for its whole lifetime, so the reference renderer is unaffected.
* This matches "after a short while". The preload and the first frame's lazy glyphs all go into the CPU copy before the first
  draw, so the first frames look right. The first glyph added after the sheet's first upload wipes it. After that, each new
  glyph wipes everything added since the last upload.
* Wider impact: any non-dynamic managed or default texture that the engine locks again after first use comes back zeroed.
  Examples are `eCGuiTexture2::CreateBitmap` / `ColorFill` 0x535690 through `LockImageLayer` (slot 2) if it is re-filled,
  and runtime-updated textures in general. A `LOCK_READONLY` read-back of such a texture also returns zeros.

**Test plan**
1. Log once per texture in `Texture::lock` when it re-creates a level that was uploaded and dropped before. Needs a per-level
   `dropped` flag set at `:191`. Log size, format, pool, usage, rect and flags. Expected: 256x256 A8R8G8B8 pool 1 hits at the
   moment text breaks, with rects one glyph cell in size.
2. A/B: temporarily never drop CPU copies (comment out `:190-192`, or add a config switch). If the character-screen text
   stays intact, H1 is confirmed.
3. Screenshot scenario: a spass mod loads `testdemo`, waits for the world (~frame 450), then
   `risen.input.simulate("<character-screen action from risen.input.actions()>", true/false)`. Hover a few entries, close and
   reopen. Run with `-Screenshots '600,700,800,1000,1300'`. Compare with a `-NoRenderer` D3D9 run that uses spass's
   `risen.debug.screenshot`.
4. Offline regression in `rvk_d3d9test`: managed A8R8G8B8 256x256; lock and write rect A; sample (upload); lock and write
   rect B; sample. Pixel A must survive.

**Fix sketch**
* Keep the CPU copy for every texture that may be locked again. Drop it only for textures filled through `setLevelData`
  (file DDS via `CreateImageFromMemory`) that were never locked. One way: a `lockable_` / `lockedOnce_` flag set in `lock()`
  and in `HalDevice::CreateTexture/CreateCubeMap/CreateVolume`.
* If a dropped level is locked anyway without `LOCK_DISCARD`, restore it first: read the level back from the image into the
  CPU copy (GPU copy to a staging buffer + wait, the `readRenderTarget` approach). Never hand out a zero buffer.
* Optional: remember the union of locked rects per level and upload only that region (`VkBufferImageCopy` offset/extent)
  instead of the whole level. For a 256x256 sheet this saves 256 KiB per new glyph.

### H2 (performance, fixed; no fps effect measured): whole-buffer copy per sprite batch [C code, C impact]

* `Buffer::gpu` (`src/d3d9/resources.cpp:255-268`) copies the **whole** CPU buffer into the frame's ring copy whenever the
  buffer is dirty. The sprite VB is 0x1000 * 0x70 = 448 KiB and is locked once per quad. Every batch (a texture or blend
  change) therefore costs a 448 KiB memcpy into write-combined memory.
* A text-heavy screen interleaving several glyph sheets and GUI bitmaps can reach hundreds of batches: about 100+ MB per frame
  and a large fps drop.
* Not a visual break, and the DISCARD/NOOVERWRITE semantics are correct (see section 3), but it makes that screen slow.
* Test: `-LogFrame N` in the open character screen; count `Primitive::Execute` calls on the sprite VB and compare fps with
  the screen closed.
* Fix: track a dirty byte range in `Buffer::lock` (offset, size; size 0 = whole buffer) and copy only that range.
  On DISCARD / a new frame, copy only the range written since then (`[0, highWater)`).

### H3 (engine limitation, both renderers): only one 256x256 sheet per font [C]

`eCGlyphSheetCollection::AppendGlyph` 0x5284c0 ignores the result of `eCGlyphSheet::AppendGlyph` on sheet 0 and returns
`true` with index 0. When the sheet is full, `CalcCellRect` 0x528010 fails and nothing is copied. `PreloadGlyph` then stores
its **uninitialised** `local_28` rect for the glyph, which draws a random atlas region. This is identical under D3D9, so it is
not the regression, but it is a second source of garbled glyphs for large fonts with many distinct characters. Keep it in
mind when checking H1 fixes: garbage that also appears in D3D9 reference shots is this.

### H4 (low): effects keep raw texture pointers after release [C code, I relevance]

`HalEffect::textures_` (`src/hal/vulkan/effect.cpp:92-93, 265-268, 396`) stores `d3d9::Texture*` without a reference. When a
texture is released, `Device::processReleases` clears the device bindings (`src/d3d9/device.cpp:482ff`) but not the effect
parameters. A later pass that applies `Texture = <param>` would bind a freed object. `ID3DXEffect::SetTexture` AddRefs, so
D3D9 never has this problem.

For fonts this is unlikely: `eCGuiDevice2::EndFrame` 0x517130 and `eCSprite::End` 0x7583b0 set the sprite effect's texture to
null every GUI frame. A font's atlas is freed through `eCDeferredGuiFont2::Release` 0x539680 -> `CacheOut` 0x539350 when its
window goes away.
* Test: in debug builds, poison released `Texture` objects and assert in `Device::setTexture`.
* Fix: on release, have the device null every effect parameter that points at the texture (an effect registry), or hold
  `shared_ptr`.

### H5 (unlikely): unimplemented `eCAPITexture::LoadSurfaceFromSurface` (slot 14) / `SaveTextureToFile` (slot 9) [C log, I relevance]

`rvk.log` shows both once, around frame 1 (start-up), not when the character screen opens. Neither the font code nor the
sprite code calls them. The wrapper `eCGfxImageData::LoadSurfaceFromSurface` 0x7523c0 has no direct callers and is not
imported by other DLLs. The calls come from somewhere else (virtual / inlined).
* Test: log `_ReturnAddress()` (minus the Engine.dll base) in the stubs to identify the callers.
* Implement later as a CPU convert/scale between CPU copies (needs H1's restore path).

## 3. Checked and fine [C]

* Upload ordering (`Device::prepareTextures`, `src/d3d9/draw.cpp:91-121`):
  * a texture not yet sampled in this frame is uploaded in the frame's upload command buffer;
  * one already sampled in this frame is uploaded inline in the main command buffer, after `endRendering`.
  Earlier draws keep the old contents, later draws see the new glyph.
* Dynamic VB pattern: DISCARD sets `gpuFrame_ = 0`, so the next draw gets a fresh ring copy and draws recorded earlier keep
  theirs. NOOVERWRITE appends land in the same copy, and the app never rewrites regions in use
  (`src/d3d9/resources.cpp:233-269`). The IB (dynamic, filled once) keeps its CPU copy and is re-copied every frame: correct
  (just wasteful, see H2).
* Bindless slots are released through `Frames::retire` (`src/vk/bindless.cpp:212-221`): no slot reuse while frames in flight
  still sample it. Texture images are retired the same way (`src/d3d9/resources.cpp:59-70`).
* `eSGfxDrawCallDesc` for sprites uses base vertex 0 and `minVertexIndex = start*4`; `HalDevice::Draw` maps it correctly.
* The ring grows by 32 MiB chunks and never runs dry in a frame (`src/vk/frames.cpp:174-229`).

## 4. Confirmation and fix (2026-10-21)

**Reproduction** [C]: risen-spass scenario `rvk_charscreen` (new game, `risen.gui.open(3)` = the character screen, mouse
hovers over a grid of points, close, reopen, then the main menu; every step logs its `ui.frame` count; `shots=1` takes D3D9
reference screenshots). No action name contains "character" in `risen.input.actions()`, so the scenario opens page 3 directly.
Rendered frame = `ui.frame` + ~285..300 (the `ui.frame` counter starts when the mod is loaded).
* A diagnostic log in `Texture::lock` (a lock that hits a level whose CPU copy had been dropped) fired once, at the moment
  the hover produced the first new glyph: `256x256 A8R8G8B8 pool 1 usage 0x0 level 0 rect 62,54,67,70 flags 0x0`: the
  glyph sheet, one glyph cell, no flags, exactly as decompiled.
* Screenshot just before (frame 800) shows every label; just after (frame 850) the skill names (Sword fighting, Alchemy ...)
  and the hotbar digits are gone, the attribute texts in another font stay. Only that font's atlas was hit.
* A/B: with the fix the same frames, the tooltip (with glyphs like `"` added lazily), the reopened screen (frame 1850) and the
  main menu match the D3D9 reference shots.

**Fix** (`src/d3d9/resources.cpp`, `device.h`, `device.cpp`):
* `Texture::lock` sets `lockedOnce_`; `upload` drops a level's CPU copy only for never-locked textures (file DDS through
  `setLevelData`, non-dynamic, not SYSTEMMEM). The glyph sheets are locked before their first upload (preload set), so they
  keep the copy. Cost in game: CPU copies of all textures together 34-38 MiB (the 5 s stats line now prints it).
* Every level tracks the union of its locked rects (`markDirty`; compressed formats round to 4x4 blocks; LockBox and
  rect-less locks = whole level) and `upload` copies only that box (`VkBufferImageCopy::imageOffset`). The first upload of a
  level is always whole (the image starts undefined, the CPU copy zeroed). A new glyph now uploads ~1 KiB instead of 256 KiB.
* A lock without `LOCK_DISCARD` of a dropped level reads the level back first (`Device::readImageLevel`, the old
  `readRenderTarget` code: copy to a host buffer, submit, wait, start a new command buffer). Only on the render thread and
  for unconverted formats; otherwise a warning (once per texture) and the texels read 0, but the partial upload still keeps
  the rest of the image intact. Not seen in game (no warning in testdemo / rvk_charscreen).
* `readRenderTarget` locks its destination with `LOCK_DISCARD` (it overwrites the whole level: no read-back).
* `Buffer` (dynamic / SYSTEMMEM): `lock` records the written byte range; `gpu()` copies only that range into the frame's
  ring copy. A fresh ring copy (new frame, or after a lock) copies the whole buffer, except right after a DISCARD, where
  only the range written since is defined. NOOVERWRITE appends keep landing in the same copy; DISCARD / plain locks still
  rename (draws recorded earlier keep their copy).
* Tests: `rvk_d3d9test` "partial texture lock" (lock A, draw, lock B, draw: A survives; read-only lock sees both),
  "lock after upload" (file texture, whole-level lock writing one corner: the rest is read back), "dynamic buffer across
  frames" (data written in an earlier frame is still drawn without a new lock). The first two fail against the old code.

**Performance** [C]: character screen open, ~1560 draws per frame, vsync pacing at 120 Hz: 79.5 fps both before and after
the H2 fix. The screen is not bound by the memcpy: the time saved moves into `vkQueuePresentKHR` (avg 2.5 ms before, 5.2 ms
after). With `max_fps = 60` the game ran at only 43-47 fps with 7-11 ms average pacer sleep per frame, before and after the
fix: the pacer seems to overshoot when frames are close to the target (not investigated).
