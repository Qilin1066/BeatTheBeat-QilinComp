# The runtime (roadmap step 3)

Last updated 2026-11-18 (v0.13.0 in progress: the OS's threads, mutexes, messages and time lifted over a native thread switch; `main` runs up to `DVDInit`. v0.12.0: the skeleton).

## Building and running

The runtime has its own lift, next to the differential test's. The test lifts every library, to check the lifter on as much code as possible. The runtime's lift (`--runtime`) turns the functions of replaced and dropped libraries (`code-analysis.md`) into calls to native code.

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol --symbols-out pal_symbols.txt
build/qilin lift path/to/main.dol --out gen-rt --runtime --symbols pal_symbols.txt
cmake -S . -B build-rt -DQILIN_RUNTIME_GEN_DIR=$PWD/gen-rt && cmake --build build-rt --target qilin_run --parallel
build-rt/qilin_run path/to/main.dol
```

`pal_symbols.txt` and `gen-rt/` are derived from the game: never commit them (`gen-rt/` is gitignored; keep the symbol list outside the repository). Exit status: 0 the game ended itself, 1 it stopped, 2 the runtime couldn't start.

## How replaced code is reached

- **`qilin lift --runtime`** (`recomp/src/main.cpp`, the plan in `recomp/src/libraries.*`): a function in a replaced or dropped library becomes `rt::native(c, index)`, and `natives.cpp` lists them (address, name, library). `symbols.cpp` lists every function's name, for reports and for the functions the runtime calls by name. Because these stand in the function table like any other function, direct calls, tail calls and calls through pointers all arrive the same way.
- **The registry** (`recomp/runtime/native.*`): native code registers by the function's name, the decomp's, with `QILIN_NATIVE(OSGetTick) { ... }`. Names, not addresses: the PAL addresses come from the symbol list at lift time, so the repository holds no list of them. At start-up `bind_natives()` joins registrations to the generated table and refuses to start if a registered name isn't a replaced function of the game, is ambiguous, or is registered twice.
- **Fail loudly:** a replaced function without native code stops the game when it is called (`Stop::Unregistered`), and `qilin_run` names it, its library and its caller:
  `stopped: no native code yet for 800E28B0 (OSInitMutex) [os], called from 800E8184 (__init_cpp+0x24)`.
  The same report covers the other stops: an access outside the memory lifted code may use, an indirect call to something that isn't a function, a return to the wrong place, a trap, a function that isn't lifted.
- **Lifted all the same** (`lift_anyway` in `libraries.cpp`, by name): functions inside replaced libraries that are software with nothing under them. So far: `__init_user`, `__init_cpp` and `exit` (start-up and shutdown around `main`, linked at the end of the OS), and GX's light-object functions (`GXInitLightColor`, `GXInitLightSpot` …), which only fill in the structure the caller passes. The lift fails if one of these names isn't in the symbol list.

## Start-up

`qilin_run` (`recomp/runtime/run.cpp`) does what `__start` does in the supported build (`dol-analysis.md`), without lifting it:

1. Loads the DOL into MEM1 (24 MB at `0x80000000`); MEM2 (64 MB at `0x90000000`) is zeroed. The rest of the 4 GB guest space is reserved and inaccessible.
2. Sets the registers `__init_registers` and `__init_hardware` set: the stack pointer with `__start`'s terminating frame, the small-data bases, HID2 with paired singles and quantised loads enabled.
3. Calls `OSInit` (native), `__init_user` (lifted: the static constructors), `main(0, 0)` (lifted), then `exit` (lifted: the static destructors, then `PPCHalt`, native, which ends the run with status 0). Each call has the link register `__start` would give it.

`__start`'s other steps aren't needed: `__init_data` copies sections onto themselves and the loader leaves BSS zeroed; the debugger checks (the boot information's debug flag, `InitMetroTRK`) are for development kits; and the disc passes no arguments.

**Bring-up runs with checked memory.** Lifted code may use MEM1 from the DOL's base up and MEM2. Low memory (the loader's and the OS's globals below `0x80004000`), the uncached mirrors and the hardware registers stop with the address and the instruction, so each one lifted code really uses is found and then provided on purpose. Native code reads and writes guest memory unchecked.

## Native code so far

31 of the 2,096 replaced and dropped functions:

- `os.cpp`: `OSInit` (arenas, the lifted units' initialisers, interrupts on), `OSDisableInterrupts`/`OSEnableInterrupts`/`OSRestoreInterrupts` (the enable flag the game sees), the twelve arena getters and setters, the cache range functions (nothing to do: guest memory has no caches behind it), `OSRegisterVersion` (the version string, to stderr) and `PPCHalt`.
- `threads.cpp`: `SelectThread`, `OSSetCurrentContext`, `OSInitContext`.
- `vi.cpp`: `VIInit`, `VIGetTvFormat`, `VIGetDTVStatus`, and SC's `SCGetProgressiveMode`, `SCGetEuRgb60Mode`, `SCGetAspectRatio`.

### To confirm: what the loader hands `OSInit`

`OSInit` takes the arenas from low memory, which IOS and the apploader fill in before the game starts. Nothing in the DOL says what they put there, so these come from `OSInit`'s own code and need confirming against the original game in Dolphin, the reference (dump `0x80000000`–`0x80003400` at the entry point):

| Value | Now | From |
|---|---|---|
| MEM1 arena start | `0x803E94A0` | `OSInit`, when low memory gives no start: the end of the debugger stack after the main stack, rounded up to 32 bytes |
| MEM1 arena end | `0x81730000` | `OSInit`'s default when neither low memory nor the boot information gives one. A console gives one (the start of the disc's file table), so this is on the small side |
| MEM2 arena start | `0x90000800` | `OSInit` raises a lower start to this |
| MEM2 arena end | `0x933E0000` | The documented start of IOS's own heap at the top of MEM2 |

**Checking them:** in Dolphin, turn on the debugging interface, set a breakpoint at the entry point `0x80004050` in the code view, start the game, and when it stops dump MEM1 from the memory view (it writes `mem1.raw` into Dolphin's dump folder; menu names vary between versions). Then:

```sh
python3 tools/analysis/lowmem.py path/to/mem1.raw
```

It prints every low-memory word `OSInit` and `__start` read (arenas, clocks, console type, IOS version, the boot information) next to the runtime's value, and flags the ones that differ. The dump holds the whole game image, so it stays out of git (`*.raw` is ignored).

## Threads (v0.13.0, in progress)

Agreed with the owner on 2026-11-18: the OS's software is lifted, and only what is under it is native.

- **Lifted units** (`kLiftUnits` in `recomp/src/libraries.cpp`, by translation unit, so no PAL addresses are listed): `OSThread.c`, `OSMutex.c`, `OSMessage.c`, `OSTime.c`. None of them has a privileged instruction. The SDK's own code decides which thread runs, with its priorities and queues exactly. From `OSContext.c`, `OSClearContext`, `OSGetCurrentContext` and `OSGetStackPointer` are lifted too.
- **Native in a lifted unit** (`kNativeAnyway`): `SelectThread`. Lifted, its idle loop would spin forever: it waits, in a loop with no call in it, for an interrupt to make a thread ready.
- **The switch** (`recomp/runtime/threads.cpp`): `SelectThread` follows the build's own code step by step (run queues, the bitmask of non-empty queues, the reschedule flag, the switch callback, the idle context). Only its context save and load differ. Each guest thread runs on its own host thread, which keeps the guest registers in its own `rt::Context`; a switch hands a run token to the next thread's host thread and waits for its own turn. One guest thread runs at a time, as on the Wii's single core. Host threads rather than fibres or stack switching, to keep the runtime free of platform assembly.
- **`SelectThread`'s globals are read from its own instructions** at start-up (eight D-form instructions at known offsets: the small-data offsets from `r13` and the two `lis`/`addi` pairs). If they aren't where the supported build has them, `qilin_run` refuses to start.
- **A new thread** starts on its own host thread the first time it is chosen, with the registers `OSCreateThread` left in its context, at its entry point; when the entry returns, it goes on into `OSExitThread`, as the link register `OSCreateThread` set sends it. A thread that has exited ends its host thread when it switches away. `OSInitContext` is native so that a thread structure reused for a new thread retires the host thread still waiting on the old one.
- **Idle:** with no thread ready, the original enables interrupts and waits. Until the runtime raises interrupts, that stops the game (`Stop::Idle`), naming the thread that went to sleep last.
- **Low memory is open to lifted code** under checked memory: the lifted OS keeps its globals there (the current thread at `0x800000E4`, the current context at `0x800000D4`, the active thread list).

**Not exercised yet:** start-up creates no thread and no mutex is contended before the stop below, so the switch has not run yet. The first thread the game creates will exercise it.

### Console settings (`recomp/runtime/console.hpp`)

The game picks its video mode from the console's settings (SC) and VI: 480p when progressive scan is on and the cable allows it, otherwise interlaced by TV format and the 60 Hz setting; 4:3 when the aspect setting is 0. On a Wii these are the owner's system settings. Here they are the runtime's, until the settings file exists: 60 Hz (EuRGB60), progressive, 16:9. **Compare with Dolphin only with the same settings on both sides:** they decide 50 or 60 Hz.

## Where bring-up stands

`main` runs: the game's init (`VIInit`, the console settings, the video mode) and then its file manager, whose constructor calls `DVDInit`. Next:

1. **Interrupts**, delivered at safe points: when the game re-enables interrupts, and in the idle loop. The handler runs in an interrupt context (so `SelectThread` doesn't switch inside it), then the reschedule runs, as the SDK's dispatcher does. DVD completion callbacks, alarms (the decrementer), VI retrace and audio all arrive this way.
2. **DVD over an extracted disc.** Reading the `.wbfs` would need the Wii's disc key, which this project can't include; Dolphin's **Extract Entire Disc** writes the files decrypted. The native DVD layer reads them, using the disc's own `fst.bin` so the game's file entry numbers are the console's.
