# Occlusion and event queries: engine use and the Vulkan design

Status: research (2026-10-21). Evidence tags: **[C]** read in the binary / decompile / trace, **[I]** inferred.
RVAs are Engine.dll RVAs (image base `0x180000000`). Dumps: `B:\DevDrive\risen-ghidra\rvk_occlusion_dump{,2,3}.txt`;
static scans: `%TEMP%\rvk_occ\scan_occ.py`, `scan_devirt.py` (capstone over `B:\DevDrive\risen-test-vk\bin\Engine.dll`).

## TL;DR

* **The running game never issues occlusion queries.** The only Engine.dll code that calls the HAL query slots is the
  **offline lightmap/radiosity baker** `eCLightmapPreprocessor::RenderRadiositySample` (`0x580180`), reached only from
  `eCStaticLightPreprocessor::ComputeLightmapLighting` / `FlushResults` (static-lighting compilation, an editor feature). No
  lens flare, sun, corona, portal/zone, vegetation or frustum code uses them. **[C]** (static scan + caller chain), confirmed
  by the trace run (`bin/rvk/trace-stats.txt`: zero calls to slots 162..166). **[C]**
* "Always visible" is therefore **neither a performance loss nor a visual bug** in the game. In the baker it only makes the
  iterative radiosity loop run its full 21 iterations instead of stopping at convergence (slower bake, same result [I]).
  Returning "0 pixels" instead would be **wrong** (the loop would stop after one pass): keep "visible" as the fallback.
* The **event query** is not a HAL slot: `eCDX9Device` creates it internally (`+0xe58`) and spins on it in `BeginScene` as a
  frame-latency limiter (engine-gfx.md). Our `vk::Frames` timeline (2 in flight) replaces it; nothing to emulate.
* Real occlusion queries are cheap to add (design below, ~150 lines) but **low priority**: implement only for completeness
  of the HAL / the editor baker. PLAN.md's "occlusion queries report visible" limitation can be downgraded to "unused".

## 1. The HAL slots and the DX9 implementation [C]

`eCAPIDevice` slots (`src/hal/api_generated.h:497-501`), vtable offset = slot * 8:

| slot | offset | eCDX9Device | DX9 behaviour (`dx9_layer.txt:9514-9724`) |
|---|---|---|---|
| 162 `CreateOcclusionQuery(void*& h)` | `0x510` | `0x746730` | `CreateQuery(D3DQUERYTYPE_OCCLUSION=9)`; stored in `bTObjArray` `+0x70` (data `+0x78`, count `+0x80`), free-index list `+0x88` (data `+0x90`, count `+0x98`). Handle = **1-based index**, free slots reused; fails (h = 0, returns 0) at `0xffff` live queries. Returns 1 on success. |
| 163 `DestroyOcclusionQuery(h)` | `0x518` | `0x746900` | `Release`, null the entry, push index to the free list. Returns 1 for any non-null h. |
| 164 `BeginOcclusionTest(h)` | `0x520` | `0x7469b0` | `Issue(D3DISSUE_BEGIN=2)`; 1 on success, 0 for a bad handle / failure. |
| 165 `EndOcclusionTest(h)` | `0x528` | `0x746a40` | `Issue(D3DISSUE_END=1)`. |
| 166 `GetOcclusionTestInfo(h, ulong& pixels, bool wait)` | `0x530` | `0x746ad0` | `GetData(&pixels, 4, 0)` (**no `D3DGETDATA_FLUSH`**); if `wait`, spin while it returns `S_FALSE`. Returns 1 only on `S_OK`; `S_FALSE` -> 0 with `pixels` untouched. Out-of-range handle -> `pixels = 0xffffffff`, returns 0. |

`eCGfxAssistantAdmin::{Create,Destroy,Begin,End}OcclusionTest/Query`, `GetOcclusionTestInfo` (`0x7562f0..0x756370`) are
exported thin forwarders to the same vtable offsets; nothing calls them (no Engine.dll caller, no other DLL imports them:
Game.dll imports only `eCGfxAssistantAdmin::CopyCurrentRTToImage`, Importer.dll only `SaveImageToFile`). **[C]**

`eCDX9Device::Reset`/`ToggleNewMode` (`0x73fde0`/`0x73f690`) release and re-create the D3D queries around
`IDirect3DDevice9::Reset` (engine-gfx.md); irrelevant for Vulkan (no lost device).

## 2. Who issues them [C]

Scan of every `.pdata` function for `call/jmp [reg+0x510|0x518|0x520|0x528|0x530]`, then filtered by argument shape
(`Begin`/`End`/`GetInfo` pass the handle by value in `rdx`; the 6 small cold fragments at `0x692455`, `0x6ea8ee`,
`0x6f28c1`, `0x6f2fa1`, `0x6f3681`, `0x6f409c` and the pair in `0x4cf6e5/0x4cf6f4` pass `lea rdx,[local]`, i.e. other
classes' slots; `0x3068a2` is a property-reflection thunk table): the only coherent user is

| call site | slot | function |
|---|---|---|
| `0x58065e` | Create (lazy, once: `if (this+0xa8 == 0)`) | `eCLightmapPreprocessor::RenderRadiositySample` `0x580180` |
| `0x580dd9` / `0x580e02` / `0x580e25` | Begin / End / GetInfo(`wait = true`) | same |
| `0x57fff5` | Destroy (`this+0xa8`) | `eCLightmapPreprocessor::Destroy` `0x57fc50` |

Caller chain: `RenderRadiositySample` <- `RenderRadiosityPass` (`0x583b90`) <- ... `RenderLightmaps` (`0x587940`) <-
`eCStaticLightPreprocessor::FlushResults`; the preprocessor object is constructed only in
`eCStaticLightPreprocessor::ComputeLightmapLighting`. **[C]** (dump3 headers). Not reached in normal play **[I]**
(static lighting is precompiled in the shipped `.lrgeo`/lightmaps; trace confirms no calls in `testdemo` **[C]**).

Use pattern (dump2 lines 406-522): one query handle, per radiosity sample up to **21 iterations**
`do { set targets/shader; Begin(h); eCFrustumDatabase::ExecuteRadiosityPass(); End(h); GetInfo(h, pixels, wait=true);
if (pixels) { accumulate pass "RadiositySample" ... } } while (++i < 0x15 && pixels != 0)`. It only tests `!= 0`
(convergence: stop once a pass writes no pixel). Blocking read, result needed immediately.

Devirtualization check: of the 106 functions comparing against `eCDX9Device::vftable` (`0x9ea550`), none contains an
inlined `IDirect3DQuery9::Issue(1|2)` / `GetData(.., 4, ..)` sequence (`scan_devirt.py`). **[C]**

Not query-based (searched, no query calls) **[C]**: `eCSkydome_PS` sun (`CreateSunOcclusion` `0x6e3d70` builds a sky-object
billboard at `+0x1c8`, flag `+0x211`; `eCShaderSkyDome::SunOcclusion` is a material property), `eCLightStreaks_PS`
(static "god ray" polygons), `eCVegetation_PS` visible grid nodes and `eCFrustumElementItem::WasVisibleLastFrame`
(`0x6715e0`: `item+0xc == database+8`, a CPU frame stamp), `eCOccluder_PS` (CPU occluder volumes for the frustum database).
So sun flares through walls etc. cannot be caused by our stub.

## 3. Current state in rvk

`src/hal/vulkan/device.cpp:939-951`: handles from `nextQuery_` (`hal_vulkan.h:409`), Begin/End no-ops,
`GetOcclusionTestInfo` returns `bEResult_Ok` with `pixels = 100000`. Correct "safe" stub for the only user. Two small
deviations from DX9 (irrelevant today): Destroy/Begin/End/GetInfo accept any handle, and handles are never reused.

## 4. Vulkan design (if/when implemented)

### Vulkan rules that shape it
* An occlusion query must either begin **and** end inside the same render pass instance (dynamic rendering: same
  `vkCmdBeginRendering` scope), or begin and end **outside** any render pass and then contain whole render pass instances.
* Our `d3d9::Device` opens rendering lazily and closes it on many events: `setRenderTarget` / `setDepthStencil`
  (`src/d3d9/device.cpp:640-660`), `clear` (`:675`), `stretchRect` (`:750`), `readRenderTarget` (`:773`), `prepareTextures`
  when a texture needs a layout change (`src/d3d9/draw.cpp:116`), `submitFrame`/`present` (`device.cpp:249`, `:291`).
  A D3D9 Begin..End bracket can span any of these, so "begin inside the current scope" is not safe.
* **Decision: queries live outside render pass instances.** `Begin` and `End` call `endRendering()` first
  (`draw.cpp:13`), then `vkCmdBeginQuery`/`vkCmdEndQuery` on `frames_.cmd()`. The next draw reopens rendering via
  `beginRendering()` (`draw.cpp:20`) as usual. Cost: one render-pass split per Begin and per End (fine: a handful per
  frame at most; zero in the game today).
* A query must be reset before `vkCmdBeginQuery` and the reset must be outside a render pass: use **host reset**
  `vkResetQueryPool` (core 1.2, feature `hostQueryReset`, required by Vulkan 1.2 [I: spec feature-requirements], but it
  still has to be **enabled**: add `f12.hostQueryReset = VK_TRUE` in `src/vk/context.cpp:230-240`). Host reset is legal
  only when the slot is not in use by pending GPU work: guaranteed by recycling slots through `frames_.retire()`.

### Data model (`src/d3d9/`)
* `d3d9::Device` owns occlusion `VkQueryPool`s in chunks of 256 slots (grow by adding a pool; the engine caps live
  handles at `0xffff`) and a free list of `(pool, index)`. Optional feature `occlusionQueryPrecise` (Vulkan 1.0 feature,
  enable when supported in `context.cpp:218-226`, expose in `vk::Context::features()`): the engine only tests `!= 0`, so
  non-precise is enough; precise gives D3D9-like sample counts for logging/mods.
* `struct Query { Slot slot{none}; std::uint64_t endFrame = 0; enum {Idle, Building, Issued, Done} state; std::uint32_t
  result = 0; }` in a `std::vector`, handle = 1-based index with a free-index list (mirrors the DX9 layer; 0 = invalid).
* API on `d3d9::Device` (render thread only, like the rest of the device): `createQuery()`, `destroyQuery(h)`,
  `beginQuery(h)`, `endQuery(h)`, `queryResult(h, bool wait, std::uint32_t& pixels) -> bool`.

### Semantics
* **begin**: if the query owns a slot from an earlier issue, hand it to `frames_.retire()` (D3D9: re-issuing discards the
  old result). Take a free slot, `vkResetQueryPool(dev, pool, idx, 1)`, `endRendering()`, `vkCmdBeginQuery(cmd, pool, idx,
  precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0)`, state = Building. Track the single active query (D3D9 allows one active
  occlusion query; a Begin while another is Building -> end the other first or reject).
* **end**: `endRendering()`, `vkCmdEndQuery`, `endFrame = frames_.currentValue()`, state = Issued. Also force-end any
  Building query in `submitFrame` (`device.cpp:249`) so a query never straddles a command buffer (begin/end must be in
  the same command buffer).
* **result, wait = false** (D3D9 `GetData(.., 0)` polling): if `frames_.completedValue() < endFrame` -> return false
  (S_FALSE, `pixels` untouched: no driver call). Else `vkGetQueryPoolResults(.., 1, 16, data, 16,
  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT)`; if available, cache `min(value, 0xffffffff)`, state =
  Done, retire the slot, return true. Repeated calls after Done return the cached value (D3D9 keeps returning it).
  Latency: the result is ready once the frame containing End has completed, i.e. 1-2 frames with 2 in flight; D3D9 with
  the engine's BeginScene event spin delivers it within ~1 frame. Same order, and no stall.
* **result, wait = true** (the baker's case): if `endFrame == frames_.currentValue()` (not submitted yet) do what
  `readRenderTarget` already does (`device.cpp:800-803`): `submitFrame(VK_NULL_HANDLE, VK_NULL_HANDLE); beginFrame();`
  then `frames_.waitFor(endFrame)` (cheaper than `waitIdle`) and read with `VK_QUERY_RESULT_WAIT_BIT | 64_BIT`. Never use
  `WAIT_BIT` on an unsubmitted query (the spec allows it to hang).
* **destroy**: end it if Building, retire its slot, free the handle.
* No lost-device handling needed; on device teardown destroy the pools after `frames_.waitIdle()`.

### HAL glue (`src/hal/vulkan/`)
Replace the stubs at `device.cpp:939-951` (declarations `hal_vulkan.h:345-349`; drop `nextQuery_` at `:409`):
`Create` -> `dev_.createQuery()` (on exhaustion h = 0 and return 0, like DX9), `Begin/End` -> 1 on success,
0 on a bad handle; `GetOcclusionTestInfo(h, pixels, wait)` -> 1 only when a result was produced; bad handle ->
`pixels = 0xffffffff`, return 0 (DX9 behaviour, section 1). Keep a config switch (`[debug] occlusion = "visible"`) that
restores today's stub, since the stub is the proven-safe behaviour for the only caller.

### Test
`rvk_d3d9test` (no game): draw a full-screen quad inside Begin/End with a target change in between (exercises the
"outside render pass" rule), expect `pixels == w*h` with precise; draw behind a cleared depth of 0 -> expect 0; poll with
`wait = false` until ready, then `wait = true` on a fresh query in the same frame (mid-frame submit path). Run with
`-Validation`. In-game verification is not possible without driving the static-light preprocessor (editor command).

## 5. Doc follow-ups (not done here: read-only task)
* `docs/PLAN.md:63` / `:80`: occlusion queries are unused by the game (only the editor's radiosity baker); the "visible"
  stub is correct for it.
* `docs/research/hal.md:104`: "occlusion queries: not seen" is now explained (static proof, section 2).
