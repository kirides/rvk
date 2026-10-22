# Savegame thumbnails (Load / Save pages)

User report (2026-10-22): in Vulkan mode the thumbnails of the Load / Save pages showed the scene's colours scrambled into a
fine noisy pattern. A savegame made in Vulkan mode showed a correct thumbnail in D3D9 mode, so the capture was fine and the
display was wrong. Root cause: `eCGfxImageData::OnRead` fills the thumbnail texture through a **`D3DLOCK_READONLY`** lock, and
`d3d9::Texture::lock` did not upload anything written under a READONLY lock. The image was sampled without ever being written,
so its contents were undefined (in the user's run, stale memory; in the reproduction, black / empty). Fixed in
`d3d9::Texture::lock` (D3D9 managed-pool semantics, section 3).

[C] = confirmed (decompiled + disassembled, or seen in the game), [I] = inferred. Addresses are RVAs (Engine.dll unless
marked Game.dll).

## 1. The path

### Capture (when saving) [C]

* `gCSaveGameManager::EnsureThumbnail` (Game.dll `0x99b760`) creates the manager's thumbnail image once:
  `eSGfxLayersDesc` 128 x 128, depth 0, 1 level, usage 0, `A8R8G8B8` (0x15), pool 2 = **SYSTEMMEM**, name
  `"gCSaveGameManager::EnsureThumbnail"`.
* `gCSaveGameManager::UpdateThumbnail` (Game.dll `0x99c820`, from `QuickSave` and `gCHUDFileManager::UpdateScreenshot`)
  creates a temporary 128 x 128 `A8R8G8B8` image with usage 1 = RENDERTARGET, pool DEFAULT, then:
  * `eCGfxAssistantAdmin::CopyCurrentRTToImage(rt, filter 2 = LINEAR, NULL, NULL)` -> HAL Texture slot 11
    (`StretchRect(RT0 -> rt)`, the whole frame squeezed into the square; ours: `HalTexture::CopyCurrentRTToImage`, a blit).
  * `eCGfxImageData::GetRenderTargetData(rt, face 6, thumbnail, level 0)` -> Texture slot 15 (`GetRenderTargetData` into the
    SYSTEMMEM image; ours: `Device::readRenderTarget`, a read-back into the CPU copy).
* `gCSaveGame::Create` (Game.dll `0x99b020`) -> `gCSaveGame::CopyThumbnail` (Game.dll `0x99af10`): serialise the image into a
  `bCMemoryStream` (`OnWrite`, vtable `+0x30`), create a new image (vtable `+0x58` Create) and read it back (`OnRead`, vtable
  `+0x28`); the copy is kept at `gCSaveGame +0x38`, its stream size at `+0x30`. The savegame stores that image as a G3IMG
  record (section 2) [I: the writer of the save file was not dumped; the record format is `OnWrite`'s].

### Display (Load / Save pages) [C]

* Each listed savegame's thumbnail is a new image read with `eCGfxImageData::OnRead` `0x753fb0` (from the save file, and
  again through `CopyThumbnail` / `gCHUDFileManager::RefreshSaveGameImage` (Game.dll `0x87b450`) ->
  `eCGuiBitmapProxy2::SetImageData`).
* `OnRead` builds the `eSGfxLayersDesc` from the record's header but sets the pool itself:
  `pool = (this+0x58 != 1)`, i.e. **MANAGED** for these images (the log of the fixed build shows `128x128 A8R8G8B8 texture
  (pool 1)` four times when the Load page with four savegames opens). Then `CreateLayers` `0x751fc0` -> HAL
  `CreateTexture`.
* Then for every level from the smallest up: `LockImageLayer(level, 0x10, &pitch, &bits)` (Texture slot 2; cube images:
  `LockCubeMapLayer(level, face, 0x10, ...)`), the rows are read from the stream straight into `bits + row * pitch`,
  `UnlockImageLayer(level)`. Disassembly at `0x7541fb..0x754232`: `mov r8d, 0x10` (texture) / `mov r9d, 0x10` (cube) right
  before `call [vtable+0x10]`; **`0x10` = `D3DLOCK_READONLY`**, although the lock is used to write.
* `eCDX9Texture::LockImageLayer` `0x74c9e0` passes the flags unchanged to `IDirect3DTexture9::LockRect` (vtable `+0x98`).

## 2. The G3IMG record (`OnWrite` `0x7547e0` / `OnRead` `0x753fb0`) [C]

* `bCObjectRefBase` part, then the magic `"G3IMG"` (5 bytes), then a 0x30-byte header: `+0` u32 version (2; `OnRead` accepts
  1 and 2), then the first 0x2c bytes of the image's `eSGfxLayersDesc` (`eCGfxImageData +0x18`): `+4` width, `+8` height,
  `+0xc` depth (written 0), `+0x10` bool cubeMap, `+0x14` levels, `+0x18` usage, `+0x1c` D3DFORMAT, `+0x20` pool (the
  source's: 2 for the thumbnail; ignored by `OnRead`), `+0x24` byte size, `+0x28` u64 0.
* Data: per face (1 or 6), levels from `levels - 1` down to 0, each level row by row **without the pitch** (width x bytes
  per pixel; 3 for R8G8B8, 4 for the 32-bit formats incl. `A8R8G8B8`, 2 for 16-bit, 1 for 8-bit, 8 / 16 for 64 / 128-bit),
  DXT1 as `max(1, w/4) * max(1, h/4) * 8` bytes, DXT2-5 x 16. `OnWrite` reads each level under
  `LockImageLayer(level, 0x10 READONLY)`. It refuses volumes, non-power-of-two sizes and formats outside its list.
* The thumbnail record is therefore 128 x 128 x 4 = 64 KiB of BGRA rows. Nothing renderer specific is stored: a savegame from
  either renderer reads the same in both.

## 3. Why D3D9 shows it, and the fix

* D3D9 managed (and system-memory) textures are **dirty as a whole from creation** until their first upload; a lock adds a
  dirty rect unless it is `D3DLOCK_READONLY` / `NO_DIRTY_UPDATE`, but it never removes the initial whole-level dirty state.
  So the texels `OnRead` writes under its READONLY lock reach the GPU copy at the first use.
* `d3d9::Texture::lock` marked a level dirty only for non-READONLY locks, so the fresh image was never uploaded and was sampled
  with undefined contents (transitioned from `VK_IMAGE_LAYOUT_UNDEFINED`).
* Fix (`src/d3d9/resources.cpp`, rule `textureLockDirty` in `src/d3d9/identity.*`): a READONLY lock of a level whose image
  holds no data yet (never uploaded, and the texture never a blit destination) marks the **whole level** dirty; once the level
  was uploaded (or the texture was written by `Device::stretchRect`), READONLY adds nothing as before. The blit flag
  (`Texture::gpuWritten_`) keeps a READONLY lock from uploading a stale CPU copy over data a blit put into the image.
* Tests: `rvk_tests` `identity_texture_lock_dirty` (the rule), `rvk_d3d9test` "read-only lock of a fresh texture" (sampled
  result, and that a READONLY lock after the upload still changes nothing).

## 4. Verification (2026-10-22, AMD RX 7900 XTX)

Scenario `mods/rvk_savethumbs` (new game, `risen.world.auto_save(<save>)`, then `risen.gui.open(15)` Menu_Load and
`open(16)` Menu_Save with `risen.debug.screenshot`), in the comparison copy `B:\DevDrive\risen-test-vk-cmp` (instance 3;
savegames go to the copy's `localappdata\Local\Risen\SaveGames`, not through the data junctions):

```powershell
.\scripts\run.ps1 -NoRenderer -GameDir B:\DevDrive\risen-test-vk-cmp -Instance 3 -Mods rvk_savethumbs -Scenario rvk_savethumbs -WaitForResult -Param save=rvk_d3d9
.\scripts\run.ps1 -Mode vulkan -Validation -GameDir B:\DevDrive\risen-test-vk-cmp -Instance 3 -Mods rvk_savethumbs -Scenario rvk_savethumbs -WaitForResult -Param save=rvk_vk2
```

* D3D9: thumbnails shown. Vulkan before the fix: every thumbnail empty (black), for the D3D9-made and the Vulkan-made save.
* Vulkan after the fix: all four thumbnails shown on both pages (made by D3D9, by the old and the fixed Vulkan build, and an
  older autosave), matching the D3D9 run; `rvk.log` logs the four READONLY locks of fresh 128 x 128 MANAGED textures;
  validation clean. Reference comparison of the GUI spots (`gui_inventory`, `gui_character`, `menu_esc`) against
  `out\refcompare\20261022-093252`: PASS.
