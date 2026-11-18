# The runtime (roadmap step 3)

Last updated 2026-11-18 (v0.12.0: the skeleton. `qilin_run` starts the game, runs `OSInit` natively and the static constructors as lifted code, and stops at the first replaced function without native code).

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

## Native code so far (`recomp/runtime/os.cpp`)

22 of the 2,135 replaced and dropped functions: `OSInit` (arenas, interrupts on), `OSDisableInterrupts`/`OSEnableInterrupts`/`OSRestoreInterrupts` (the enable flag the game sees), the twelve arena getters and setters, the cache range functions (nothing to do: guest memory has no caches behind it), `OSRegisterVersion` (the version string, to stderr) and `PPCHalt`.

### To confirm: what the loader hands `OSInit`

`OSInit` takes the arenas from low memory, which IOS and the apploader fill in before the game starts. Nothing in the DOL says what they put there, so these come from `OSInit`'s own code and need confirming against the original game in Dolphin, the reference (dump `0x80000000`–`0x80003400` at the entry point):

| Value | Now | From |
|---|---|---|
| MEM1 arena start | `0x803E94A0` | `OSInit`, when low memory gives no start: the end of the debugger stack after the main stack, rounded up to 32 bytes |
| MEM1 arena end | `0x81730000` | `OSInit`'s default when neither low memory nor the boot information gives one. A console gives one (the start of the disc's file table), so this is on the small side |
| MEM2 arena start | `0x90000800` | `OSInit` raises a lower start to this |
| MEM2 arena end | `0x933E0000` | The documented start of IOS's own heap at the top of MEM2 |

## Where bring-up stands

The static constructors run until one calls `OSInitMutex`. That function, like `OSLockMutex` and the thread, message-queue and alarm code around it, is bookkeeping on structures in guest memory: it reaches the hardware only through the interrupt enable (`OSDisableInterrupts`, native already), the context switch and the time base.

**Proposed for the next step (to be agreed, since it moves the lifted/replaced boundary of `code-analysis.md`):**

- **Lift the OS's software units** (mutexes, threads, message queues, alarms, the heap, time conversions) and replace only what is under them: the interrupt enable, the context switch, exceptions, the decrementer and the time base. The SDK's own scheduler then decides which thread runs, with the Wii's priorities and queues exactly, which matters for a rhythm game; and the native code shrinks to the part that can't be lifted (it uses the privileged instructions).
- **The context switch:** each guest thread gets a host thread, and one run lock lets a single one run at a time (the Wii has one core). `SelectThread`, where the SDK saves one context and loads the next, becomes native: it picks the next thread from the SDK's run queues in guest memory and hands the run lock over. Host threads rather than fibres or hand-written stack switching, to keep the runtime free of platform assembly; a switch costs a few microseconds and the game makes few.
- **Interrupts** (alarms from the decrementer, later VI retrace and audio) are delivered at safe points: when the game re-enables interrupts, and in the idle loop. Whether lifted loops also need a poll on their back edges is decided when a loop that waits on an interrupt is found.
- **Low memory** then has to hold what the lifted OS and the game's inlined SDK macros read (the bus and CPU clocks, the current thread, the thread queue): the values in the table above, plus those, confirmed in Dolphin.
