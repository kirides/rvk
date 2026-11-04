# Resource streaming, loading screens and frame-time spikes of the engine thread (2026-11-04)

Goal: find out what the still scene's frame-time spikes (p95 6.2 ms vs p50 5 ms, an occasional 15 - 19 ms frame per 5 s) and the loading
screens (frames of 300 - 620 ms) consist of, and what of it can be done cheaper or off the engine thread. Method, facts, and the
switches that came out of it. [C] = confirmed in the running game / disassembly, [I] = inferred.

## 1. Tooling

* `[debug] cpu_profile` (`run.ps1 -CpuProfile`) now also writes a **spike profile**: every sample taken more than `RVK_SPIKE_MS` (default 8,
  `-Environment 'RVK_SPIKE_MS=6'`, 0 = off) into a frame goes to a second map (a normal frame never lasts that long, so these samples
  belong to a spike frame or to a loading step). rvk.log `diag: spike: N samples >= 8.0 ms ...` + `diag: spike module|self|incl|stack`
  rows every 5 s. `tools/spikesum.py <rvk.log> [--from HH:MM:SS --to ...] --kind self|incl|module|stack` weights the windows by their sample
  count (0.5 ms per sample) and sums them (`--from/--to` select the loading windows or the still hold).
* `debug.stream_stats` + always-on counters: rvk.log `engine: stream:` every 5 s (packed reads, absorption images, sounds created).
* Scenarios: `rvk_perfsave` (new game, quick load of `out/fg/perf.save`, 90 s hold = loading windows and still scene in one log), `rvk_menufps`
  (three timed quick loads).

## 2. What the engine does [C unless marked]

* Resources are `eCResource2` objects in `eCResourceCache2` caches (`eCResourceModule2`): images, meshes, materials, motions, motion actors,
  sounds, speedtrees. A request is `QueueData(resource, mode)` (Engine 0x6852d0): mode 0 = **direct**: `LoadData` at once on the calling
  thread (DoRender / game logic); mode 1 = **lazy**: the file is opened with `CFXVirtualFile::Open(..., SFFAsynFileReadDesc{AsyncLoadHandler})`
  (an asynchronous read by FFC / FFCore), the completion handler `OnAsyncLoad` (another thread) appends (resource, file) to a queue under
  a critical section (`this + 0x3d8`), and `eCResourceModule2::ProcessCaches` (every frame, from `eCProcessor::Process`) calls
  `LoadQueuedData` -> `eCResourceCache2::LoadData` -> `eCResource2::LoadData` -> the class's `OnLoadData` **on the engine thread**.
  So file IO of lazy requests is already asynchronous; the parse / create step is not.
* `eCResource2::LoadData` (0x6828f0): header "GR01" + content version, then `eCResourceInputStream2::GetBuffer` (a mapping of the compiled
  data) and the class's `OnLoadData`. Flush on the soft limit (`FlushBytes`), `CacheResource` = LRU list move.
* Threads: FFCore has `CFFThread` / `g_FFCreateThread`, SharedBase `bCThread`; the engine uses them for the async file reads and a
  physics worker. There is no worker for decompression / creation: `OnLoadData` of every class runs on the main thread.
* The world is not streamed as resources: sectors are read as **object streams** (`gCSector::OnRead` -> `ImportDynamicLayer` -> `bCObjectBase::Read`
  with property sets) from packed volumes on the engine thread, one `eCArchiveFile::Read` per property (4 - 16 bytes).

## 3. What the spikes and loading frames consist of [C]

Profile `p1` (savegame `out/fg/perf.save`, wrapper, lod 2x, `RisenTest.exe`, vsync off, 0.5 ms samples, build with the spike profile).

### 3.1 The still scene (hold, 18 windows of 5 s)

Median frame 5 - 5.5 ms; frames over 8 ms are rare (128 samples = 64 ms of time beyond 8 ms in 90 s; `max` per window 6 - 19 ms,
most windows have none). The p95 of 6.0 - 6.1 vs the p50 of 5.3 - 5.5 ms is mostly the engine's own frame variation (the sun
cascades are updated every 2nd frame, `debug.shadow_stride`), not streaming. What is above 8 ms:

| share of the spike samples | what |
|---|---|
| **29 %** | **sound creation on the engine thread**: `gCEffectCommandPlaySound::OnStart` -> `eCSoundResource2::Play` -> `QueueData(mode 0)` -> `LoadData` -> `eCSoundResource2::OnLoadData` -> `eCAudioSound::Create` -> **`FMOD::System::createSound`** (the sound is decoded to PCM when the file is below a size limit: flag 0x100 `FMOD_CREATESAMPLE`, else 0x200 compressed sample). A sound that is not in the cache (first use or evicted) costs 1.3 ms on average, up to 18 ms (`engine: stream: sounds created 240 in 317 ms (longest 18.2 ms)` at the start of the run) |
| 25 % | our own draw path / driver (pipeline creation, first draws) |
| ~20 % | the engine's render (CullTree, `eCShaderBase::OnGetPasses` of the sky, particle `UpdateVertices`) |
| rest | amdvlk, ntdll alert (record threads) |

No texture / mesh / animation load, no file read and no decompression shows up in the still scene's spikes [C]; the resource
caches are warm and `texture uploads` are 0 KiB per frame. The "streaming spikes" of the still scene are the sounds the effects
start (ambient and effect sounds that left the cache) plus ordinary frame variance.

### 3.2 The loading screens (first three windows = new game + quick load, 4708 samples beyond 8 ms = 2.35 s)

| share | where | what |
|---|---|---|
| **39.8 % (module ntdll)**: `NtReadFile` 27.6 % + `NtSetInformationFile` 8.3 % (+ 4 %) | `eCArchiveFile::Read` -> FileSystem2 `CFFPackedDirectFileStream::Read` (0x23540) -> `g_FFSetFilePointer` + `g_FFReadFile` | **one seek and one read system call per property read**: the world sectors (`gCSector::OnRead` -> `ImportDynamicLayer` -> `bCObjectBase::Read`) are read 4 - 16 bytes at a time. 6.0 million such calls per load [C: `engine: stream: packed reads 6020555`] |
| **22.3 %** | `eCSkydome_PS::CreateAbsorbationImage` (called from `OnNotifyPropertyValueChangedExitEx` for `CloudTexture1` / `CloudTexture2`, i.e. whenever a world with a sky is read) | a 1024 x 1024 image from four 32-tap box sums per pixel with a 32-bit `div` for every tap: **128 divisions per pixel, 276 - 289 ms per image, two images per load** |
| 4.8 % | FMOD `createSound` of sounds the world starts while loading | |
| 2 % | `eCMotionResource2::OnLoadData` (the lazy queue's parse, EMotionFX) | |
| rest | entity creation (`AttachChild`, `OnRead` of entities, SpeedTree `operator=`), physics `FetchResults`, d3dx9 (texture loads), the HAL | |

The frames of 300 - 620 ms are the sector steps (`gCWorld::Reload` / `Activate` -> `SendEvent` -> one present per step): each step reads
a sector's object streams, so the system calls above are spread over them; the skydome's two 280 ms images land in the step that reads
the sky entity.


### 3.3 Walking (`rvk_rtwalk`, 55 s of walking and turning at the savegame)

35 samples (18 ms) beyond 8 ms in 11 windows, `max` 7 - 15 ms per window, p50 5.5 / p95 6.3 ms. No resource load, no `LoadQueuedData`, no file
read in the stacks: the cache-in around the hero (`eCPrefetchAdmin::CacheEntities`, already strided by `debug.prefetch_stride`) finds its
objects resident; walking in this area does not stream. (A walk into unvisited sectors would; the savegame region was warm after
the quick load, and this build has no way to test a fresh far region quickly. The profile tooling is the same: `diag: spike` rows.)

### 3.4 After the changes of section 5 (`p2`, same scene)

Loading windows: 996 ms beyond 8 ms (was 2354 ms). What is left: sector object parsing (`bCObjectBase::ReadObject` 29 %, pure engine CPU:
entity creation, `AttachChild`, SpeedTree buffers), the lazy resource queue (20 %, of which sounds 12 %: FMOD `createSound`), FMOD, d3d9 (our
HAL for the loading screen's own frames and first draws), `memmove`.

## 4. Off-thread options, evaluated

| # | option | verdict | estimate | risk |
|---|---|---|---|---|
| 1 | Prefetch file data on a worker (read ahead / decompress before the engine asks) | **not needed after 5.1**: what was slow was not the disk (the data is in the OS cache) but 6 million system calls; the buffer removes 99.7 % of them on the engine thread. Remaining `NtReadFile` after the fix: 4.6 % of the loading samples (~45 ms) | <= 45 ms per load | - |
| 2 | Texture side (DDS parse, staging, upload off-thread) | **nothing to move**: texture uploads are 0 KiB / frame in the world and ~330 KiB in the first frames; `d3dx9_39` is 3.9 % of the loading samples (the engine's image loads through D3DX, Engine side, not our HAL) | < 40 ms per load | high (engine-owned) |
| 3 | Run `OnLoadData` (parse + create) of the lazy queue on a worker | **rejected, not provable**: the engine's allocator (`bCMemoryAdmin`), reference counts (`bCObjectRefBase`, non-atomic), the resource lists (LRU `CacheResource`), `eCGfxImageData` creation through the HAL device and FMOD objects are all touched by `OnLoadData`; the only locking in this path is the critical section around the completion queue. The queue is processed in `eCResourceModule2::Process` and consumers poll the resource state, so a deferral would be legal, but running the parse concurrently with the game thread is not | the 12 - 20 % of the loading | silent corruption |
| 4 | Budget the lazy queue per frame (time-slice `LoadQueuedData`) | possible (consumers of a pending resource already fall back to `QueueData(mode 0)`), but there is nothing to smooth: in the still scene and in the walk the queue is empty; during loading screens every frame is a load step and slicing lengthens the load | 0 | medium |
| 5 | Parallelise the loading steps | the steps are sequential object-stream parses with engine state; not provable. What the loading screen can do is not wait: `menu-fps.md` | - | - |
| 6 | FMOD `createSound` off the engine thread: FMOD_NONBLOCKING | rejected: `playSound` on a sound that is still loading fails (`FMOD_ERR_NOTREADY`), and the first play is the request that triggered the load | - | sounds dropped |
| 7 | **FMOD sounds as compressed samples** (the decode moves to FMOD's mixer thread) | **built, opt-in** (5.3) | 8 - 12 ms -> 0.2 - 0.3 ms per sound | audio not checked by ear |
| 8 | Replace the engine's slow absorption image loop | **built** (5.2) | -565 ms per load | none (bit-identical) |

## 5. What was built

### 5.1 Packed file read buffer (`debug.fs_read_cache`, KiB, default 64; `engine/load_accel.cpp`)

`FileSystem2.dll` RVA 0x23540 = `CFFPackedDirectFileStream::Read` (non-exported; prologue checked, MinHook detour installed from the
`InitializeAdmin` detour): rcx is the stream's interface subobject; the fields live at negative offsets: -0x38 the volume's file stream,
-0x30 the packed file's base offset in the volume, -0x28 the read position (64 bit), -0x20 the size (32 bit). The engine function
seeks the volume (`g_FFSetFilePointer`, vtable +0x20) to base + position and reads (`g_FFReadFile`, +0x28), then advances the position:
two system calls per Read. The world's sector streams read 4 - 16 bytes at a time: **6.0 million Reads per load** [C]. The detour keeps a
direct-mapped table of 32 per-stream buffers: a miss reads ahead (2 KiB, doubling with every sequential miss up to the setting) through the
engine's own function (into the buffer, then the position is set to where a small read would have left it), a hit is a `memcpy` +
position update. Reads of more than a quarter of the buffer, reads that cross the end of the packed file and the engine's error path go
to the engine's function unchanged; an entry expires after 1 s without use and when the stream, volume, base or size differ.
**Verification** (`debug.stream_stats=2`): every buffered read is re-done through the engine's own function and compared (bytes and the
resulting position): 6,479,678 hits, 0 differ. Results: 99.7 % of the reads from the buffer (16 k refills of ~3 KiB), the loading
screens' `NtReadFile` + `NtSetInformationFile` 36 % of the thread -> 4.6 %.

### 5.2 Cloud absorption image (`debug.fast_absorb`, default 1)

`eCSkydome_PS::CreateAbsorbationImage` (Engine 0x6e2cb0) builds the 1024 x 1024 absorption image of each cloud texture
(`CloudTexture1` / `CloudTexture2`, Sky_Clouds_0n_Absorption) from the blue channel: four 32-tap box sums per pixel (right, left, down, up with
unsigned wrap-around), each tap a 32-bit `div` for the wrapped coordinate, times a float constant (Engine 0x9f1590), truncated, packed
`up<<24 | right<<16 | down<<8 | left`. 128 divisions per pixel: **238 - 289 ms per image, two per world load** [C]. The image is then
written to `#G3:/Data/Raw/Images/Sky/..tga` (our path guard refuses, as for every engine write into the data tree). The loop is patched
in the middle of the function (two 5 / 6-byte jumps at 0x6e34a7 and 0x6e3a15 to a stub in memory allocated within 2 GB of Engine.dll;
bytes checked first): the stub calls `absorbFast` (sliding-window sums: the sums are integers <= 8160, exact in float, so the order of
the engine's additions does not matter; the unsigned wrap `(x - k) % w` is done directly for the first 32 positions, which also
matches non-power-of-two sizes), then skips the engine's loop. `rvk_tests absorb_fast_matches_naive` compares it with a transcription of the
engine's loop for 8 sizes (incl. 33 x 31, 100 x 77 with padded pitch). **In-game**: `debug.fast_absorb=2` runs the engine's own loop after
computing ours and compares the two images: 1024 x 1024 x 2 per load, **0 rows differ** (4 loads). Cost with the fix: ~3 ms per image.

### 5.3 FMOD sounds as compressed samples (`debug.sound_compressed`, default **off**)

`FMOD::System::createSound` (exported by `fmod.dll`) is hooked: a call with `FMOD_CREATESAMPLE` (0x100, the game's choice below its size
limit, `eCSoundResource2::OnLoadData`) is made with `FMOD_CREATECOMPRESSEDSAMPLE` (0x200, the game's own choice above the limit) instead; if FMOD
refuses, the game's flags are used. Measured with `debug.stream_stats=1` (4 runs of 60 s, 257 sounds each): **320 ms -> 12.5 ms of
`createSound` on the engine thread, longest 12.6 -> 1.7 ms**, no refusals. The decode now happens on FMOD's mixer thread while a voice plays. It is
the same codec's output, but a sound that is played many times is decoded each time, and the audio was not compared by ear / recording, so it
stays opt-in: a player-side listening test is the missing gate.

## 6. Results (A/B, interleaved, default test copy, 3 arms each, `rvk_menufps` quick loads of the 4 MB savegame)

| | new game | quick load 1 / 2 / 3 |
|---|---|---|
| off (`fs_read_cache=0`, `fast_absorb=0`) | 6.39 / 6.08 / 6.08 s | 4.33, 4.07, 4.08 / 4.26, 4.14, 4.15 / 4.16, 4.08, 4.11 s |
| on (defaults) | 3.91 / 3.94 s | 2.80, 2.75, 2.78 / 2.77, 2.78, 2.76 s |

**Loading -33 % (quick load 4.13 -> 2.78 s) and -37 % (new game 6.2 -> 3.9 s)**; the loading profile's time beyond 8 ms per frame 2354 -> 996 ms.
Still scene: unchanged (p50 5.3 - 5.5 / p95 5.9 ms with and without; the changes only act while the engine reads packed files / builds the sky).

## 7. Not done / ideas

* The remaining loading cost is the engine's object parsing (`ReadObject` 29 %) and entity creation: a wholly engine-side cost.
* `eCConfigFile::ReadFile` reads the physics config line by line through the physical stream (`g_FFReadFile` per character): 1.8 % of the loading, the same buffer idea for `FUN_1800267e0` (FileSystem2 0x267e0) would remove it.
* Sound cache: the engine flushes sounds at its soft limit (`FlushBytes`); a larger sound cache would stop the replay decodes in the still scene (not looked at).

## 8. Gates (2026-11-04, build 9f19968 + b632899)

`rvk_tests` 216 / 216 (`absorb_fast_matches_naive`); in-game compares: `debug.fast_absorb=2` 0 rows differ (4 loads), `debug.stream_stats=2` 6,479,678 buffered
reads, 0 differ; `-Validation` run of the savegame load: 0 validation lines; `testdemo` 7 / 7 PASS with `-Validation` on Vulkan wrapper + full and
D3D12 wrapper + full (0 validation lines; own copy `risen-test-vk-stream`, instance 5); `refcompare` (Vulkan wrapper, stored reference
`out/refcompare/20261103-130010`): 11 PASS / 7 WARN / 2 FAIL (`sea_cliff`, `harbour_mouth` = the known sea-edge noise), no new FAIL. Note: a fresh
worktree's `scripts/refcompare.ps1` snapshot needs `dxcompiler.dll` in `build\refcompare-snapshot` (copy it by hand; the script copies only d3d9.dll / d3d9.pdb).
