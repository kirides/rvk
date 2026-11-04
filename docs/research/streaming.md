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

