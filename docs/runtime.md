# The runtime (roadmap step 3)

Last updated 2026-11-19 (v0.13.0: the OS's threads, mutexes, messages, time and alarms lifted over a native thread switch and interrupt delivery; DVD over an extracted disc; `main` runs up to `GXInit`. v0.12.0: the skeleton).

## Building and running

The runtime has its own lift, next to the differential test's. The test lifts every library, to check the lifter on as much code as possible. The runtime's lift (`--runtime`) turns the functions of replaced and dropped libraries (`code-analysis.md`) into calls to native code, except the units and functions the plan lifts out of them.

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol --symbols-out pal_symbols.txt
build/qilin lift path/to/main.dol --out gen-rt --runtime --symbols pal_symbols.txt
cmake -S . -B build-rt -DQILIN_RUNTIME_GEN_DIR=$PWD/gen-rt && cmake --build build-rt --target qilin_run --parallel
build-rt/qilin_run path/to/extracted-disc       # the folder Dolphin's Extract Entire Disc wrote
```

`qilin_run` takes the **extracted disc's folder** (`sys/main.dol`, `sys/fst.bin`, `files/`): Dolphin → right-click the game → Properties → Filesystem → right-click the disc → **Extract Entire Disc**. The `.wbfs` or `.iso` can't be read directly: a Wii disc is encrypted with a key this project can't include, and the extraction writes the files decrypted. Given `main.dol` alone, it runs until the game first touches the disc.

`pal_symbols.txt`, `gen-rt/` and the extracted disc are derived from the game: never commit them (`gen-rt/` and `pal_symbols.txt` are gitignored; keep the disc outside the repository). Exit status: 0 the game ended itself, 1 it stopped, 2 the runtime couldn't start.

When it stops, `qilin_run` says what it has done so far (thread switches, threads started, interrupts delivered, files opened, bytes read), so a run shows which parts of the runtime it exercised, then why it stopped.

## How replaced code is reached

- **`qilin lift --runtime`** (`recomp/src/main.cpp`, the plan in `recomp/src/libraries.*`): a function in a replaced or dropped library becomes `rt::native(c, index)`, and `natives.cpp` lists them (address, name, library). `symbols.cpp` lists every function's name, for reports and for the functions the runtime calls by name. Because these stand in the function table like any other function, direct calls, tail calls and calls through pointers all arrive the same way.
- **Lifted out of replaced libraries** (`libraries.cpp`): whole translation units by name (`kLiftUnits`), single functions by name (`kLiftAnyway`), and exceptions inside a lifted unit that stay native (`kNativeAnyway`). Names, not addresses, so the repository keeps no list of PAL addresses. The lift fails if a name in the plan isn't in the symbol list.
- **The registry** (`recomp/runtime/native.*`): native code registers by the decomp's function name, `QILIN_NATIVE(OSGetTick) { ... }`. At start-up `bind_natives()` joins registrations to the generated table and refuses to start if a registered name isn't a replaced function of the game, is ambiguous, or is registered twice.
- **Names checked against code:** a native whose name could sit on the wrong function states the instructions it expects, `QILIN_NATIVE_CODE(PPCMtdec, 0x7C7603A6u, 0x4E800020u)` (`mtspr DEC, r3; blr`), and `qilin_run` refuses to start if the function of that name doesn't have them. This guards against what v0.13.0 found: in a run of equal-size functions, size alignment had put eight names of the base library three functions early, `PPCMtdec` on the function that writes HID0 (`code-analysis.md`; `port_symbols.py` now checks accessor names against their code).
- **Fail loudly:** a replaced function without native code stops the game when it is called (`Stop::Unregistered`), and `qilin_run` names it, its library and its caller:
  `stopped: no native code yet for 800FD630 (GXInit) [gx], called from 801D7C98 (__ct__15CGraphicManager…+0x9C)`.
  The same report covers the other stops: an access outside the memory lifted code may use, an indirect call to something that isn't a function, a return to the wrong place, a trap, a function that isn't lifted, and a game no thread of which can run (`Stop::Idle`).

## Start-up

`qilin_run` (`recomp/runtime/run.cpp`) does what `__start` does in the supported build (`dol-analysis.md`), without lifting it:

1. Loads the DOL into MEM1 (24 MB at `0x80000000`); MEM2 (64 MB at `0x90000000`) is zeroed. The rest of the 4 GB guest space is reserved and inaccessible. Then binds the natives (after loading: some check the code they replace).
2. Sets the registers `__init_registers` and `__init_hardware` set: the stack pointer with `__start`'s terminating frame, the small-data bases, HID2 with paired singles and quantised loads enabled. Reads `SelectThread`'s globals from its code (below).
3. Calls `OSInit` (native), `__init_user` (lifted: the static constructors), `main(0, 0)` (lifted), then `exit` (lifted: the static destructors, then `PPCHalt`, native, which ends the run with status 0). Each call has the link register `__start` would give it.

`OSInit` sets the arenas, writes the bus and CPU clocks to low memory (the SDK's tick conversions read them: 243 MHz and 729 MHz), runs the lifted units' initialisers in its own order (`__OSInitAlarm`, then `__OSThreadInit`) and turns interrupts on.

`__start`'s other steps aren't needed: `__init_data` copies sections onto themselves and the loader leaves BSS zeroed; the debugger checks are for development kits; and the disc passes no arguments.

**Bring-up runs with checked memory.** Lifted code may use MEM1, low memory included (the lifted OS keeps its globals there), and MEM2. The uncached mirrors and the hardware registers stop with the address and the instruction, so each one lifted code really uses is found and then provided on purpose. Native code reads and writes guest memory unchecked.

## Threads

Agreed with the owner on 2026-11-18: the OS's software is lifted, and only what is under it is native.

- **Lifted units** (`kLiftUnits`): `OSThread.c`, `OSMutex.c`, `OSMessage.c`, `OSTime.c`, `OSAlarm.c`. None has a privileged instruction. The SDK's own code decides which thread runs and when an alarm fires, with its priorities and queues exactly. From `OSContext.c`, `OSClearContext`, `OSGetCurrentContext` and `OSGetStackPointer` are lifted too.
- **`SelectThread` is native** (`recomp/runtime/threads.cpp`), following the build's own code step by step: run queues, the bitmask of non-empty queues, the reschedule flag, the switch callback, the idle context. Lifted, its idle loop would spin forever, waiting in a loop with no call in it for an interrupt to make a thread ready. Only its context save and load differ: each guest thread runs on its own host thread, which keeps the guest registers in its own `rt::Context`, and a switch hands a run token to the next thread's host thread and waits for its own turn. One guest thread runs at a time, as on the Wii's single core. Host threads rather than fibres or stack switching, to keep the runtime free of platform assembly.
- **Its globals come from its own instructions** at start-up (eight D-form instructions at known offsets: the small-data offsets from `r13` and two `lis`/`addi` pairs). If they aren't where the supported build has them, `qilin_run` refuses to start.
- **A new thread** starts on its own host thread the first time it is chosen, with the registers `OSCreateThread` left in its context and the interrupt enable its saved MSR gives it, at its entry point; when the entry returns, it goes on into `OSExitThread`, as the link register `OSCreateThread` set sends it. A thread that has exited ends its host thread when it switches away. `OSInitContext` is native so that a thread structure reused for a new thread retires the host thread still waiting on the old one.
- **Idle:** with no thread ready, the scheduler waits for the next interrupt and delivers it, until one makes a thread ready, as the original's idle loop does. If nothing could ever arrive (no alarm set, nothing queued), the game is stuck and stops (`Stop::Idle`), naming the thread that went to sleep last.

OSThread's layout, from the code: the saved context at +0 (GPRs, CR +128, LR +132, CTR +136, XER +140, FPRs +144, SRR0 +408, SRR1 +412, state +418, GQRs +420, ps1 +456), state +712 (1 ready, 2 running, 4 waiting, 8 moribund), priority +720, queue +732, links +736/+740, stack +772/+776. Low memory: current context `0x800000D4`, FPU context `0x800000D8`, active threads `0x800000DC`/`0x800000E0`, current thread `0x800000E4`.

## Interrupts and alarms (`recomp/runtime/interrupts.*`)

The Wii raises an interrupt between any two instructions; lifted code can't be entered there, so the runtime delivers interrupts at **safe points**: when the game enables interrupts (`OSEnableInterrupts`, or `OSRestoreInterrupts` turning them on), and while the scheduler is idle. Whether a lifted loop that waits for an interrupt without enabling them also needs a safe point is decided when one is found.

- **A queued interrupt** (a DVD completion, later VI and audio) runs as the SDK's dispatcher runs a handler: `OSDisableScheduler` (so a thread woken inside it doesn't switch there), the handler, `OSEnableScheduler`, `__OSReschedule` (which may switch threads). The three are lifted code.
- **The decrementer** runs the handler the OS installed for it (`__OSSetExceptionHandler`, native: the runtime keeps the table) with the interrupted context. `PPCMtdec` sets the deadline on the time base, as the alarm code's `InsertAlarm` asks. The SDK's decrementer callback then fires the due alarms and ends in `OSLoadContext`, which on the console never returns: natively it unwinds back to the delivery, which puts the interrupted registers back. Anywhere else `OSLoadContext` stops the game: it would be a context switch the runtime doesn't make.
- **Every register** is as the interrupted code left it after a delivery. Delivery state belongs to the host thread delivering, so a switch during the reschedule leaves the next thread free to take interrupts.

## DVD (`recomp/runtime/dvd.*`)

The disc's own file table (`sys/fst.bin`) gives the entry numbers and directories, so they are the console's; the contents come from `files/`. Lookup follows the SDK: from the root (nothing the game lifts changes the current directory), `.` and `..`, names compared without case. A file is found again on each read as the SDK finds it, by the disc offset its open stored in the 60-byte file info (+0x30; the length is at +0x34).

- Reads complete at once. An asynchronous read's callback arrives as an interrupt, after the call has returned, as on the console; until then the command block is busy and `DVDGetDriveStatus` says so.
- Bytes past a file's end are zero (the game rounds lengths up to 32; on the console they would be whatever follows on the disc).
- Cancels only change how a pending completion reports, since reads finish at once.

## Native code so far

48 of the 2,080 replaced and dropped functions:

- `os.cpp`: `OSInit`, the interrupt enable (`OSDisableInterrupts`, `OSEnableInterrupts`, `OSRestoreInterrupts`), the twelve arena getters and setters, the cache range functions (nothing to do: guest memory has no caches behind it), the exception table (`__OSSetExceptionHandler`, `__OSGetExceptionHandler`), `PPCMtdec`, `OSRegisterShutdownFunction` (kept for when shutdown is native), `OSRegisterVersion` (the version string, to stderr) and `PPCHalt`.
- `threads.cpp`: `SelectThread`, `OSSetCurrentContext`, `OSInitContext`, `OSLoadContext`.
- `dvd.cpp`: `DVDInit`, `DVDConvertPathToEntrynum`, `DVDFastOpen`, `DVDOpen`, `DVDClose`, `DVDReadPrio`, `DVDReadAsyncPrio`, `DVDGetDriveStatus`, `DVDCancel`, `DVDCancelAsync`, `DVDCancelAllAsync`.
- `vi.cpp`: `VIInit`, `VIGetTvFormat`, `VIGetDTVStatus`, and SC's `SCGetProgressiveMode`, `SCGetEuRgb60Mode`, `SCGetAspectRatio`, `SCGetLanguage`.

### Console settings (`recomp/runtime/console.hpp`)

The game picks its video mode from the console's settings (SC) and VI: 480p when progressive scan is on and the cable allows it, otherwise interlaced by TV format and the 60 Hz setting; 4:3 when the aspect setting is 0; its language folder from the language. On a Wii these are the owner's system settings. Here they are the runtime's, until the settings file exists: 60 Hz (EuRGB60), progressive, 16:9, English. **Compare with Dolphin only with the same settings on both sides:** they decide 50 or 60 Hz.

### To confirm: what the loader hands `OSInit`

`OSInit` takes the arenas from low memory, which IOS and the apploader fill in before the game starts. Nothing in the DOL says what they put there, so these come from `OSInit`'s own code and need confirming against the original game in Dolphin, the reference:

| Value | Now | From |
|---|---|---|
| MEM1 arena start | `0x803E94A0` | `OSInit`, when low memory gives no start: the end of the debugger stack after the main stack, rounded up to 32 bytes |
| MEM1 arena end | `0x81730000` | `OSInit`'s default when neither low memory nor the boot information gives one. A console gives one (the start of the disc's file table), so this is on the small side |
| MEM2 arena start | `0x90000800` | `OSInit` raises a lower start to this |
| MEM2 arena end | `0x933E0000` | The documented start of IOS's own heap at the top of MEM2 |
| Bus, CPU clock | 243 MHz, 729 MHz | The Wii's; the time base runs at a quarter of the bus clock |

**Checking them:** in Dolphin, turn on the debugging interface, set a breakpoint at the entry point `0x80004050` in the code view, start the game, and when it stops dump MEM1 from the memory view (it writes `mem1.raw` into Dolphin's dump folder; menu names vary between versions). Then:

```sh
python3 tools/analysis/lowmem.py path/to/mem1.raw
```

It prints every low-memory word `OSInit` and `__start` read (arenas, clocks, console type, IOS version, the boot information) next to the runtime's value, and flags the ones that differ. The dump holds the whole game image, so it stays out of git (`*.raw` is ignored).

## Where bring-up stands

`main` runs: the game's init (VI, the console settings, the video mode), its file manager (`DVDInit`, the language), and its graphics manager, whose constructor calls `GXInit`. That is the next stop.

**Not exercised yet:** the game reaches `GXInit` before it opens a file, sleeps, sets an alarm or creates a thread (`qilin_run` reports 0 of each). So the thread switch, interrupt delivery, alarms and the DVD reads are built and wired in, and compile warning-free on every target, but no run has gone through them. The first frames after GX will.

Next: **GX** (roadmap step 4). GX is the largest replaced library (181 functions, 111 of them called from lifted code), and lifted code also writes the GX FIFO directly (322 stores in 12 functions). Its CPU side (the state the setters build, the command stream the FIFO and display lists carry) is needed whatever draws it; the Vulkan renderer comes on top.
