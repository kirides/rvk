# Ray tracing tier: capability probe, settings, buffers, proof (M6 do-now item 8)

The optional ray tracing tier on top of the Vulkan 1.3 floor (docs/PLAN.md section 1, docs/research/enhancements-roadmap.md
3.1 and 5 item 8): what the GPU offers is probed, the extensions are enabled when present **and allowed** (`rt.allow`), a
capability report feeds the overlay and a capability-resolution step of the settings registry (so future enhancement
settings declare "requires ray query" and fall back to parity when unmet), vertex / index buffers are allocated as possible
BLAS build inputs, and a small self-test proves BLAS -> TLAS -> ray query end to end on this GPU. Nothing renders with ray
tracing yet: the first consumer is M9 (RT reflections). With the tier off or unavailable the renderer is exactly the floor
path.

Legend: **[C]** confirmed (measured, in the game or in tests), **[I]** inferred / proposed.

## 1. What exists

| piece | files |
|---|---|
| probe: RT extensions + feature bits, acceleration structure / RT pipeline properties, driver name | `src/vk/probe.{h,cpp}` (`DeviceReport::rt`, `driverName` / `driverInfo`) |
| device: enable the tier when available and allowed, retry without it when `vkCreateDevice` fails, one log line | `src/vk/context.{h,cpp}` (`ContextOptions::allowRayTracing`, `DeviceFeatures::rayTracing`, `Context::capabilities`) |
| capability report (pure) | `src/core/caps.{h,cpp}` (`caps::Cap`, `caps::Report`, `caps::current()`) |
| capability resolution of settings | `src/core/settings.{h,cpp}` (`Spec::requirements`, `Spec::unmetValue`, `Setting::available` / `effective`, `Registry::setCapabilities`) |
| setting `rt.allow` | `src/core/settings.cpp` (`registerRendererSettings`), read in `src/hal/vulkan/device.cpp` |
| overlay: "Capabilities" section, greyed settings with the reason | `src/overlay/overlay.cpp` |
| tool API: `RVK_CAP_RAY_QUERY` (rvk_capabilities), `RVK_SETTING_UNAVAILABLE` (rvk_setting_info flags) | `include/rvk_api.h`, `src/proxy/d3d9_exports.cpp`, `src/api/settings_api.cpp` |
| conditional buffer usage | `src/d3d9/resources.cpp` (`Buffer::gpu`, static VB / IB), `src/vk/frames.cpp` (upload ring) |
| AS helpers + self-test | `src/vk/raytracing.{h,cpp}` (`rt::AddressBuffer`, `rt::buildBottomLevel` / `buildTopLevel`, `rt::buildBarrier`, `rt::selfTest`), `shaders/test/rt_query.comp` |
| tests | `tests/caps_tests.cpp`, `tests/settings_api_tests.cpp`; `rvk_vktest --rt` (GPU, synchronization validation) |
| switches | `[debug] rt_selftest` (`run.ps1 -RtSelfTest`), `run.ps1 -Setting 'rt.allow=false'`, `refcompare.ps1 -Setting ...`; `[debug] perf` prints a GPU-memory line |

## 2. Probe and device

### 2.1 What is probed [C]

`vk::inspectDevice` (used by the startup probe and by `Context::pickPhysicalDevice`) checks, on Vulkan 1.3 devices:

* extensions `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_deferred_host_operations` (the tier),
  `VK_KHR_ray_tracing_position_fetch`, `VK_EXT_opacity_micromap`, `VK_KHR_ray_tracing_maintenance1` (optional extras) and
  `VK_KHR_ray_tracing_pipeline` (information only);
* their feature bits (`accelerationStructure`, `rayQuery`, `rayTracingPositionFetch`, `micromap`, `rayTracingMaintenance1`,
  `rayTracingPipeline`; feature structs are chained only for extensions that exist) and `bufferDeviceAddress`;
* `VkPhysicalDeviceAccelerationStructurePropertiesKHR` (max geometries / instances / primitives, AS descriptor limits,
  `minAccelerationStructureScratchOffsetAlignment`) and, as information, `maxRayRecursionDepth` / `shaderGroupHandleSize`;
* the driver's name and version string (`VkPhysicalDeviceVulkan12Properties::driverName` / `driverInfo`).

The tier = acceleration structures + ray query + deferred host operations + buffer device address.

### 2.2 Enabling (decision) [C]

* `Context::create` enables the tier **only when the device has it and `ContextOptions::allowRayTracing`** (= `rt.allow`'s
  start value; `rt.allow` is a restart setting): the three extensions + `accelerationStructure` + `rayQuery`, plus
  position fetch / opacity micromaps / maintenance1 when available. `VK_KHR_ray_tracing_pipeline` is never enabled (inline
  ray query only, roadmap 3.1).
* If `vkCreateDevice` fails with the tier, the device is created again without it (reason "vkCreateDevice failed with the
  ray tracing extensions"): an optional tier never costs the floor path.
* Not enabled: `descriptorBindingAccelerationStructureUpdateAfterBind`, `shaderInt64` (see 5.1). The TLAS reaches
  shaders by its **device address** (`accelerationStructureEXT(uvec2)` in GLSL = `OpConvertUToAccelerationStructureKHR`),
  so it fits the push-constant / BDA ABI (a FrameBlock field later) without a descriptor binding.
* One log line: `vulkan: ray tracing enabled (...)` or `vulkan: ray tracing off: <reason>`; then the capability report as
  `caps:` lines (section 3).

### 2.3 This machine [C] (rvk.log, `rvk_vktest --rt`)

AMD Radeon RX 7900 XTX, **AMD proprietary driver 26.9.2 (LLPC)**, Vulkan 1.4.355, loader 1.4.363:

| item | result |
|---|---|
| `VK_KHR_acceleration_structure` / `VK_KHR_ray_query` / `VK_KHR_deferred_host_operations` | yes / yes / yes (tier enabled) |
| `VK_KHR_ray_tracing_position_fetch` | yes (enabled) |
| `VK_EXT_opacity_micromap` | **no** (alpha-tested foliage needs the any-hit-style candidate loop, roadmap 3.3) |
| `VK_KHR_ray_tracing_maintenance1` | yes (enabled) |
| `VK_KHR_ray_tracing_pipeline` | yes (information: max recursion 31, shader group handle 32 bytes) |
| acceleration structure limits | max geometries 16,777,216; max instances 16,777,216; max primitives 536,870,912; scratch offset alignment 256; AS descriptors 1,048,576 per stage / per set |

Other optional features in the report: EDS3, vertex input dynamic state, robustness2 (null descriptors), memory budget,
device fault, anisotropy 16x enabled; graphics pipeline libraries available but not used (`[vulkan] pipeline_libraries`,
the AMD driver crash of M5).

## 3. Capability report and resolution

### 3.1 Report [C]

`caps::Report` (pure, `src/core/caps.h`): `known`, GPU, driver, API version, one `Entry` per `caps::Cap` (`available`,
`enabled`, `reason` when not enabled, `detail`), plus property lines. Capabilities (append-only enum):
`RayTracingHardware` (the GPU has the tier: what `rt.allow` itself needs), `RayQuery` (tier enabled this session: what RT
features need), `PositionFetch`, `OpacityMicromap`, `RayTracingPipeline` (never enabled), `ExtendedDynamicState3`,
`VertexInputDynamicState`, `GraphicsPipelineLibrary`, `Robustness2`, `MemoryBudget`, `DeviceFault`, `SamplerAnisotropy`.
Reasons are player-readable: "the GPU / driver has no VK_KHR_ray_query", "disabled by the setting rt.allow (takes effect
after a restart)", "vkCreateDevice failed with the ray tracing extensions", "ray tracing is off: ..." for the extras.

`HalDevice` builds it after device creation (`Context::capabilities`), logs it (`caps:` lines), publishes it
(`caps::setCurrent`: `rvk_capabilities` sets `RVK_CAP_RAY_QUERY` from it) and hands it to the registry
(`Registry::setCapabilities`). The overlay shows it in **"Capabilities"** (GPU, driver, Vulkan version, a table
enabled / available-off / unavailable with the reason, AS limits).

### 3.2 How an enhancement declares a requirement

```cpp
auto& refl = reg.add({.key = "rt.reflections", .label = "Ray-traced reflections", .category = "Ray tracing",
                      .type = settings::Type::Enum, .options = {"off", "low", "high"},
                      .requirements = caps::bit(caps::Cap::RayQuery)});   // optional: .unmetValue = ...
reg.onChange(refl, [&](const settings::Setting& s) { /* create / destroy RT resources from s.asEnum() */ }, owner);
if (refl.asEnum() != "off") { ... }   // effective value
```

* `value()` is the player's request (file, API, A/B store it); `effective()` / `asBool()` / `asInt()` / `asFloat()` /
  `asEnum()` are what applies: the request while every required capability is enabled, else `unmetValue` (default: the
  spec's default = the parity path). A feature that reads `asX()` therefore falls back gracefully without own checks.
* Until the report exists (before device creation, or in passthrough / trace mode) a setting with requirements is
  unavailable ("no Vulkan device"). `setCapabilities` re-resolves every such setting; when its effective value changes,
  its callbacks run at the next frame boundary like any change (so a feature can create its resources there). Settings
  registered after the report are resolved at once.
* Overlay: greyed (`BeginDisabled`), showing the effective value, "(unavailable)" next to it, the tooltip has the reason
  and "your choice (x) is kept". API: `rvk_setting_info` flag `RVK_SETTING_UNAVAILABLE`; `rvk_setting_get` returns the
  request.
* Startup log: `settings: <key> = <request> requested, applies as <fallback>: <reason>` for each unmet request.
* `rt.allow` itself: Bool, default true, Restart, `requirements = RayTracingHardware`, `unmetValue = 0` (greyed and off on
  a GPU without the tier). The device reads its *start value* (not the effective one: the report does not exist yet).

Unit tests (`tests/caps_tests.cpp`): mask / reason texts, unavailable until a report, request kept in the file, callback on
the effective change only, unmet value clamped, `rt.allow` registered with its requirement; `settings_api_tests` checks the
API flag.

## 4. Buffer usage flags (allocate once, ready for BLAS)

With the tier enabled [C]:

| buffer | before | added |
|---|---|---|
| static VB / IB (`Buffer::gpu`, device-local, a new `VkBuffer` per update) | `TRANSFER_DST | VERTEX_BUFFER` or `INDEX_BUFFER` | `SHADER_DEVICE_ADDRESS | STORAGE_BUFFER | ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR` |
| upload ring chunks (dynamic VB / IB, DrawUP, constants; `Frames::newChunk`) | `VERTEX | INDEX | STORAGE | UNIFORM | TRANSFER_SRC | SHADER_DEVICE_ADDRESS` | `ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_KHR` |

Without the tier nothing changes (the usage bit needs the `accelerationStructure` feature; validation checks it).
`STORAGE_BUFFER` on the static buffers is for compute passes that convert / read vertex data (e.g. non-FLOAT3 positions,
VS capture, roadmap 3.3); BDA reads alone would not need it. VMA adds `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` itself
(allocator created with `BUFFER_DEVICE_ADDRESS`).

### 4.1 Cost [C]

`rvk_shadows` (beach view, ~1450 draws per frame) with `-Perf -Vsync off`, interleaved off / on / off / on
(`-Setting 'rt.allow=false'` vs the default), three 5 s windows per run (the first of off 1 contains the load hitch and is
left out). The session sits at the ~80 fps present cadence (AMD Radeon Chill, performance.md 2), so fps cannot show
small costs: compare the CPU / GPU times and the memory.

| run | fps | CPU engine+HAL / frame | of it draw calls | GPU / frame | GPU memory (VMA blocks / allocations) |
|---|---|---|---|---|---|
| off 1 | 79.9 | 5.24 / 5.39 ms | 2.16 / 2.21 ms | 2.10 / 2.12 ms | 836.1 / 514.3 MiB, 918 allocations |
| on 1 | 79.9 | 5.27 / 5.31 / 5.31 ms | 2.17-2.19 ms | 1.87-1.88 ms | 836.1 / 514.3 MiB, 918 |
| off 2 | 79.9 | 5.27 / 5.35 / 5.26 ms | 2.19-2.21 ms | 1.88-1.89 ms | 836.1 / 514.3 MiB, 918 |
| on 2 | 79.9 | 5.28 / 5.44 / 5.29 ms | 2.19-2.21 ms | 1.87-1.90 ms | 836.1 / 513.3 MiB, 917 |

**No measurable cost**: CPU and GPU times overlap within run-to-run noise (the higher GPU time of off 1 is that run's
warm-up, not the flags); memory blocks are identical, allocation bytes equal within one allocation (streaming state).
Device creation with the tier: +0 ms visible (the extensions are enabled in the same `vkCreateDevice`). Raw logs:
`out\rt-perf\{off1,on1,off2,on2}.log` (not committed).

## 5. Proof: BLAS -> TLAS -> ray query

### 5.1 `rt::selfTest` [C]

One triangle (FLOAT3, 32-bit indices, host-visible buffers with `BUILD_INPUT_READ_ONLY`) -> BLAS (`PREFER_FAST_TRACE`) ->
TLAS of two instances: A identity, custom index 7, opaque; B rotated 180 degrees about z, 1 unit further, custom index 9,
`FORCE_NO_OPAQUE` -> `shaders/test/rt_query.comp` (GLSL, `GL_EXT_ray_query`, built by `rvk_glslc` like every hand-written
shader) traces 64 x 64 orthographic rays; non-opaque candidates are confirmed by a stand-in alpha test (barycentric
u < 0.5, the pattern M9's alpha-tested foliage needs); output per pixel = hit flag, primitive index, round(t * 4), custom
index. Every pixel is compared with the analytic result (64 edge pixels skipped). Own command pool + fence, submitted
through `Context::queueSubmit` (safe beside the present thread).

* `shaderInt64` is not enabled by the renderer, so the TLAS address is passed as `uvec2` (`accelerationStructureEXT(uvec2)`);
  a `uint64_t` push constant declared the `Int64` capability and validation flagged it (VUID-VkShaderModuleCreateInfo-pCode-08740).
* `rvk_vktest --rt` (validation + synchronization validation): **PASS**: 1326 opaque hits, 1001 alpha-tested hits, 1705
  misses, 0 wrong, BLAS 720 B, TLAS 904 B; 9 ms with a warm driver shader cache (~1-2.4 s cold, mostly the pipeline
  compile); then a device with `allowRayTracing = false`: tier off with the `rt.allow` reason, report says hardware yes /
  ray query no, self-test skipped; 0 validation messages. On a GPU without the tier it prints SKIP and exits 0.
* In the game (`[debug] rt_selftest`, `run.ps1 -RtSelfTest`, testdemo with validation): `rt selftest: PASS (2318.7 ms)`
  with the same counts, on the game's device, before the first frame; 0 validation messages over the run.

### 5.2 Deferred to M9: a BLAS of a real mesh from the draw records

An in-game debug view (BLAS of one static mesh + a ray-query hit mask through an injection pass) is **not** part of this
step: it needs the world transform of the draw (entity `+0x60` or the instance constants), the position element / stride of
the declaration (FLOAT3 at offset 0 for the 52-byte static vertex, others need conversion), the sub-mesh index ranges, a
lifetime rule for the BLAS (buffer generation, `Frames::retire`), and a TLAS address in the FrameBlock: that is the
RT instance registry + BLAS cache of roadmap 3.2, the first M9 work item. Everything it builds on exists now: buffers with
the right usage, `rt::buildBottomLevel` / `buildTopLevel` / `buildBarrier`, draw records with VB / IB ids, generations,
ranges and declarations, the injection points and pass layout.

## 6. Verification

* rvk_tests 78 / 78 (new: `caps_*`, `settings_api_info_get_set` with the unavailable flag); `rvk_vktest` (contract),
  `--passes`, `--rt` (validation + synchronization validation) and `rvk_d3d9test` (whose device now runs with the tier, so
  the static buffers get the new usage) pass with 0 validation messages [C].
* Game, testdemo with `-Validation -RtSelfTest`: PASS, self-test PASS, 0 validation messages; `rvk_shadows` with
  `-Validation` (tier on, world with ~1440 draws, overlay open): 0 validation messages [C].
* **refcompare against the stored D3D9 reference `out\refcompare\20261021-190139`**:
  * tier on (default settings): **13 / 13 PASS**, run `out\refcompare\20261021-225418` (an earlier run,
    `20261021-224715`, had 12 PASS + lava_cave INVALID by the scenario's pose check, "camera eye differs by 4.13 > 4.0",
    SSIM 0.9985: camera placement, not pixels; PASS on the rerun);
  * `-Setting 'rt.allow=false'`: 12 PASS + 1 WARN (beach_sun bad32 0.25 % > 0.2 %, the known sea noise), run
    `out\refcompare\20261021-225041`; rvk.log "ray tracing off: disabled by the setting rt.allow".
* Overlay (`-Commands '1500: overlay open','1550: click 106 533' -OverlayInScreenshots`): category "Ray tracing" with
  "Allow ray tracing (after a restart)", and the "Capabilities" section (GPU, driver, Vulkan 1.4.355, the table with
  reasons) [C].
* Costs: section 4.1 (none measurable).

## 7. Open

* Opacity micromaps are not offered by this driver: alpha-tested foliage will use the candidate loop (5.1 shows it works).
* `shaderInt64` stays off; if a future RT shader wants 64-bit integer math, enable it as an optional feature (all desktop
  GPUs with the tier have it [I]).
* An AS-specific synchronization hazard test (like `--passes --hazard`) is not there yet; the self-test's barriers are
  checked by synchronization validation only in the positive case.
* A player-facing "ray tracing" preset group comes with M9's first setting (`rt.reflections`), declared as in 3.2.
