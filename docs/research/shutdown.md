# Process exit: the game's exit crashes and the exit diagnostics

Status 2026-11-11. Earlier quit crashes that were ours (HAL resources after the device, the RT scene's teardown order, the D3D12
capture pool) are in debug-mode.md 8.2 A / B and dx12-game-port.md. This page covers what remained after those fixes:
`SharedBase.dll+0x7caa9` and `Engine.dll+0x8f1bd`. **Both are one bug of the game, not of the renderer**, and a third,
related one (the rich presence thread). All three are suppressed by `src/engine/exit_guard.*` in every render mode.

## 1. Measurements (test copy `risen-test-vk-shut`, `run.ps1 -GracefulExit`, scenario `testdemo`)

| build / mode | graceful exits | crashed | where |
|---|---|---|---|
| before, Vulkan + D3D12 (WER events of 2026-11-11 00:17 - 00:46) | 11 | 11 | 10x `SharedBase+0x7caa9`, 1x `Engine+0x8f1bd` |
| before, **Original** (system D3D9; our DLL only forwards) | 8 | 5 | 5x `SharedBase+0x7caa9` (+ 1 hang > 60 s at exit, not analysed) |
| exit guard, Original | 3 | 0 | |
| exit guard, Vulkan Wrapper / Full | 5 / 5 | 0 / 0 | |
| exit guard, D3D12 Wrapper / Full | 5 / 5 | 0 / 0 | |

## 2. The cause (confirmed by the stacks of the exit reports, section 4)

The main thread, `0x7caa9` case (Original mode, first-chance report):

```
SharedBase.dll+0x7caa9   bCObjectBase::Read(bCIStream&) +0x49: call [rax+8], "this" = the stream = Read's own address
Engine.dll+0x8f1bf       eCGuiResource2::Release +0x7f (returned from: call [vtable + 0x118])
Engine.dll+0x52f346      eCGuiBitmapProxy2::~eCGuiBitmapProxy2 +0x26
Game.dll+0x541fb8 ...    gCTutorialHintCollection::~gCTutorialHintCollection, gCTutorialAdmin::~gCTutorialAdmin
Engine.dll+0x72653c      eCDocArchive::~eCDocArchive
SharedBase.dll+0xa2405   bCAccessorCreator::~bCAccessorCreator
Game.dll+0x790bdb ...    Game.dll's CRT static destructors (_DllMainCRTStartup, DLL_PROCESS_DETACH)
ntdll                    LdrShutdownProcess (inside RtlExitUserProcess)
```

Game.dll holds a static `gCTutorialAdmin` (via a `bCAccessorCreator` / `eCDocArchive`). It is destroyed in Game.dll's
`DLL_PROCESS_DETACH`, i.e. inside ExitProcess, long after the engine's normal shutdown destroyed the GUI resources. Its
tutorial hints' `eCGuiBitmapProxy2` members still point at those destroyed `eCDeferredGuiBitmap2` objects. The proxy's
destructor calls `Release` on them; `eCGuiResource2::Release` decrements a dead refcount to 0 and calls vtable slot 0x118 of
the dead object:

* the memory still holds the vtable a destructor chain left behind (`bCObjectBase`'s, whose slot 0x118 is `bCObjectBase::Read`):
  `Read` runs with `this` = a code address, takes its "stream" vtable from the code bytes (`rax = 0x60ec834857565340` =
  the bytes of `push rbx; push rsi; push rdi; sub rsp, 0x60`) and faults at `0x7caa9` (non-canonical address);
* or the slot is garbage (non-canonical): the `call rdx` itself faults at `Engine.dll+0x8f1bd`.

Which face appears depends on what the heap left in the freed memory. Neither involves a HAL object, a texture, a device or
anything of ours; the same exits crash with the system D3D9 (Original mode). The task note "a dead stream" was the
symptom: the "stream" is a function's code bytes.

**Third fault (same exits, other thread):** `Game.dll+0x970c6e` = `gCSession::RichPresenceTime` +0x2e, "read of 0x510":
Game.dll's rich presence worker thread (loop at Game 0x970e60 / 0x9710e0) keeps calling `eCApplication::GetInstance()` after
WinMain's `eCApplication` (a local on the main thread's stack: `GetInstance` returned `0x5d20bfefe0`, the main thread's rsp
was `0x5d20bff7d0`) is destroyed and its stack memory reused: a null / garbage vtable. It happens a few ms before
RtlExitUserProcess (the reports' order), so usually ExitProcess terminates the thread while its unhandled exception is being
filtered (our 0-byte `_crash.dmp` files: MiniDumpWriteDump killed mid-write); the thread could win that race and end the
process with `0xC0000005` on its own. Seen in Original mode too. Two such threads exist (two call sites).

## 3. The mitigation (`src/engine/exit_guard.*`, installed from DllMain in every mode)

* `??1eCGuiBitmapProxy2@@QEAA@XZ` (Engine 0x52f320): while `ntdll!RtlDllShutdownInProgress()` (the loader is shutting the
  process down: DLL detach inside ExitProcess), only the member is cleared, `Release` is not called (the dead object is not
  touched at all).
* `?Release@eCGuiResource2@@UEAAKXZ` (Engine 0x8f140): during the same window, returns 1 without touching the object (any
  other holder of a GUI resource released by a static destructor).
* Harmless: a GUI resource's final release runs `OnFinalRelease` (`eCGuiResource2::OnFinalRelease` = 0x7faa0, COMDAT-folded
  with an empty function; `Release` even compares the slot with it) and frees memory the dying process returns anyway. No
  file, save or config write is on that path (they finished before ExitProcess). Outside the shutdown both functions run
  unchanged (one call to `RtlDllShutdownInProgress` per call: a PEB flag read).
* `??1eCApplication@@UEAA@XZ` (Engine 0x4c6570) sets a flag; `?RichPresenceTime@gCSession@@QEAA_NXZ` (Game 0x970c40) then
  parks the calling thread (`Sleep` loop until ExitProcess terminates it). Nothing joins that thread (it is still running at
  ExitProcess in every observed exit; all guarded exits completed within the 60 s watchdog).
* rvk.log at start: `engine: exit guard: on (...)`. A missing export = "partly on" / "FAILED": the game's behaviour stays.

## 4. Exit diagnostics (`src/core/crash_dump.*`)

* The unhandled-exception filter now writes a text report next to the minidump (`rvk_<stamp>_crash.txt`): exception,
  access type / address, registers with module + offset and two levels of pointees (object -> vtable -> slot), the stack
  unwound through `.pdata` (works through Engine / Game / SharedBase without symbols), and the raw stack words that point
  into modules. Our frames: `llvm-symbolizer --obj=d3d9.dll 0x180000000+<offset>`; engine frames: the export lists
  (`%TEMP%\risen_re\*.demangled.txt`, nearest export below the offset).
* Exit diagnostics (always on with `[debug] crash_dumps`): a hook on `ntdll!RtlExitUserProcess` (kernel32!ExitProcess
  and the CRT end there; a hook on kernel32!ExitProcess saw no call) marks the process as exiting; a vectored handler writes
  every access violation / illegal instruction / stack-buffer failure after that point, first chance and on any thread, as
  `rvk_<stamp>_exitfault_<n>.txt` (at most 8). Faults inside DLL detach never reach the unhandled-exception filter, and
  `MiniDumpWriteDump` does not finish at exit, so this text is the only record of them.
* `-Environment 'RVK_EXIT_TRACE=1'`: the vectored handler reports faults of the whole run (not only after ExitProcess;
  our SEH-guarded engine reads would show up too) and the caller of RtlExitUserProcess is written as `_exit_<n>.txt`
  (`run.ps1 -GracefulExit` does not count those as dumps).
* `run.ps1 -GracefulExit` no longer exempts `SharedBase.dll+0x7caa9`: any exit crash fails the run (exit code 3).

## 5. Gate results

2026-11-11, build of this commit: 23 graceful exits with the guard (Original 3, Vulkan Wrapper 5 + Full 5, D3D12 Wrapper 5 + Full 5),
all exit code 0, no WER event, no `exitfault` report (the runs had `RVK_EXIT_TRACE=1`: no first-chance fault anywhere in the
run, the rich presence fault included); `rvk_tests` 331 / 0, `rvk_d3d9test` Vulkan + D3D12 exit 0, `rvk_rhitest` PASS.
In the same hour an unguarded build in another copy kept crashing at `SharedBase.dll+0x7caa9`.
Scratch: `B:\DevDrive\rvk-work\scratch\guiexit` (`loop.ps1`, the reports `*_exitfault_*.txt`, `near.py` = nearest export).
