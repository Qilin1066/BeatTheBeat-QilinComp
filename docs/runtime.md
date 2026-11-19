# The runtime (roadmap step 3)

Last updated 2026-11-19 (v0.15.0: the game runs: AX lifted over a native DSP and audio DMA, saves on a host folder, safe points in lifted loops; it initialises, reaches its title scene and keeps running a frame per retrace. v0.14.0: GX's CPU side, VI. v0.13.0: the OS's threads and alarms, DVD. v0.12.0: the skeleton).

## Where bring-up stands (v0.15.0)

The game **initialises completely** (`Init Process Took … msecs`, its own report), saves on first boot (`Riq.dat` and its banner), runs its opening scenes, changes to its **title scene**, and **keeps running**: in a 40-second run, 2,383 frames copied out for 2,397 retraces, 45,401 draws, 13,307 audio frames (332.7 a second, the console's 333.3), no periodic interrupt dropped, 12 threads. Nothing is drawn on screen yet (the GX sink is headless) and nothing sounds (no voice has played: the DSP stops when one does), and with no remote connected the game waits at its title. Use `QILIN_RUN_SECONDS` for runs like these.

Next: **the Vulkan renderer** for GX, now that the game draws every frame; **the mixer** (the DSP stops at the first voice); **input** (a host backend for the Wii Remote).

## Diagnostics

- **Counters** with every stop, and with `QILIN_RUN_SECONDS=N` (which ends the run after N seconds, exit status 3): thread switches and threads started, interrupts (decrementer), files and bytes read, draws and vertices, frames copied out, retraces, audio frames, **periodic interrupts dropped** (an audio frame lost is music running slow), and where deliveries happened (idle, running code).
- **A guest call stack** with every stop: the stack frames' back chain and saved return addresses, from the running thread's registers as last synced.
- **Every guest thread** at the time limit: its state, priority, queue and call stack, and the scheduler's state (current thread, run-queue bits, reschedule count). This is how the loop-poll problem below was found.
- **The game's own reports** (`OSReport`, MSL's `printf`): `OSReport` is lifted and formats through MSL; MSL's console writes to the debug UART, which here goes to stderr prefixed `[game]`, as a development kit's serial port shows it. A deliberate difference from retail, where the UART isn't there; the only effect the game could see is `printf`'s return value, which `OSReport` ignores. (MSL also calls the debugger stub's console write on every console: on retail it reads the stub's "serial I/O" byte, 0, and returns; lifted as such.)

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

The Wii raises an interrupt between any two instructions; lifted code can't be entered there, so the runtime delivers interrupts at **safe points**, where the guest's stack pointer is known (handlers build their frames below it):

- when the game enables interrupts (`OSEnableInterrupts`, or `OSRestoreInterrupts` turning them on);
- **at every call into native code** (v0.15.0): the lifted caller has just stored the registers it changed, `r1` among them;
- **on every backward branch of lifted code** (v0.15.0), so a loop that waits for an interrupt gets it, as on the console. The lifter emits `if (rt::poll_due()) [[unlikely]] { c.r[1] = r1; rt::poll(c); }`: a flag test while an interrupt is queued, and a look at the clock every 1,024th time round for the timers. Found when the Mii library spun in `RFLInitRes` (a loop with no call in it) waiting for a NAND callback. 2,594 polls in the runtime's lift; in the differential test `rt::poll` does nothing;
- and while the scheduler is idle.

A delivery saves the whole context and puts it back, so the interrupted function's live registers (host locals) and the context agree again afterwards; delivery state is per host thread, so a switch during a delivery's reschedule leaves the next thread free to take interrupts. Interrupts are only delivered while enabled.

**Periodic interrupts catch up** (v0.15.0): when delivery comes late, each period that has passed runs in turn, as the console takes every audio DMA and every retrace; only a stall of more than 32 periods (about 100 ms of audio) drops the rest, counted. **Windows** rounds short waits up to its 15.6 ms tick: `qilin_run` asks for 1 ms timers (`timeBeginPeriod`) so the idle wait wakes for each 3 ms audio frame. Before both fixes, 80% of audio frames were lost (one per retrace instead of one per 3 ms).

**I/O completions take time** (v0.15.0): queued interrupts have a due time, and DVD and NAND completions arrive no sooner than 1 ms after the request. On a console a request takes milliseconds, and the game sets its "loading" flag after issuing one (`loadFile`: `DVDReadAsyncPrio`, then the flag; the callback clears it); a completion delivered at once, at the next safe point, cleared the flag before it was set and left `waitDVD` waiting forever. Disc speed itself isn't modelled: reads finish far sooner than on a console.

- **A queued interrupt** (a DVD completion, later VI and audio) runs as the SDK's dispatcher runs a handler: `OSDisableScheduler` (so a thread woken inside it doesn't switch there), the handler, `OSEnableScheduler`, `__OSReschedule` (which may switch threads). The three are lifted code.
- **The decrementer** runs the handler the OS installed for it (`__OSSetExceptionHandler`, native: the runtime keeps the table) with the interrupted context. `PPCMtdec` sets the deadline on the time base, as the alarm code's `InsertAlarm` asks. The SDK's decrementer callback then fires the due alarms and ends in `OSLoadContext`, which on the console never returns: natively it unwinds back to the delivery, which puts the interrupted registers back. Anywhere else `OSLoadContext` stops the game: it would be a context switch the runtime doesn't make.
- **Every register** is as the interrupted code left it after a delivery. Delivery state belongs to the host thread delivering, so a switch during the reschedule leaves the next thread free to take interrupts.

## DVD (`recomp/runtime/dvd.*`)

The disc's own file table (`sys/fst.bin`) gives the entry numbers and directories, so they are the console's; the contents come from `files/`. Lookup follows the SDK: from the root (nothing the game lifts changes the current directory), `.` and `..`, names compared without case. A file is found again on each read as the SDK finds it, by the disc offset its open stored in the 60-byte file info (+0x30; the length is at +0x34).

- Reads complete at once. An asynchronous read's callback arrives as an interrupt, after the call has returned, as on the console; until then the command block is busy and `DVDGetDriveStatus` says so.
- Bytes past a file's end are zero (the game rounds lengths up to 32; on the console they would be whatever follows on the disc).
- Cancels only change how a pending completion reports, since reads finish at once.

## GX (`recomp/runtime/gx.*`, `gx_fifo.cpp`; v0.14.0: the CPU side)

GX is replaced at its API, as the roadmap says: its own code drives the GPU's registers through pointers loaded from memory, which the runtime couldn't catch without checking every load and store. The native GX keeps the state the game's calls set up **as the API describes it** (vertex descriptors and formats, the transform unit's matrices, lighting, texture coordinate generation, TEV stages and colours, blending and depth, textures, copies), which is what the renderer will read. Until the Vulkan renderer exists, a headless sink receives the draws and copies and counts them.

- **The SDK's own defaults:** native `GXInit` calls the **lifted** `__GXInitGX`, which calls the native setters with the SDK's default arguments (it reads only constants and the SDK's default render mode, and touches no hardware). So the state after `GXInit` is the SDK's by construction.
- **Pure GX functions are lifted:** the texture, TLUT and light object functions (`GXInitTexObj*`, `GXGetTexObj*`, `GXInitTlutObj`, `GXInitLight*` …), `GXGetTexBufferSize`, `GXGetYScaleFactor`, `GXAdjustForOverscan` and `GXSetTevDirect`. They work only on what the caller passes, in the SDK's layout. The natives that consume those objects (`GXLoadTexObj`, `GXLoadTlut`, `GXLoadLightObjImm`) decode the SDK's layout, read from its getters (the texture object's size, address, format, wrap modes and mipmap flag; the TLUT's format, address and entries; the light's colour, attenuation, position and direction).
- **Numbers the game depends on come from the SDK's code:** `GXSetDispCopyYScale` returns the frame buffer's line count by the SDK's arithmetic (ported instruction by instruction); `GXSetTevOp` takes its presets from the SDK's own tables in the game's data, found through `GXSetTevOp`'s instructions and checked at first use; `GXClearVtxDesc` leaves the position direct, as the SDK's does.
- **The FIFO:** lifted code writes vertices straight to the write-gather pipe (`0xCC008000`) where the SDK's inline vertex functions were compiled into it: 322 stores in 12 functions (NW4R's quads, lines and glyphs, the Home Button Menu, the Mii library, a few of the game's own). The lifter turns exactly those stores (found by `hwaccess`'s constant propagation) into `rt::fifo_st8/16/32` calls, and `stfs`/`psq_st` there into the same with their conversions; every other store stays a plain memory store. Native `GXBegin` opens a draw, the stores append its vertex bytes, and the draw goes to the sink when its vertex count has arrived. `GXCallDisplayList` walks a display list's draw commands. FIFO data outside a draw, or any command other than a draw, a NOP or a vertex-cache invalidation, stops with its value: nothing in the game needs one yet. In the differential test these calls stop with a memory fault, as the original does under Unicorn, so its results are unchanged.
- **Vertex sizes** follow the descriptors and formats: matrix indices a byte each, indexed attributes one or two bytes (three indices for NBT3 normals), direct ones by their component count and type; a component type GX doesn't define stops.
- **Not modelled yet, and saying so:** fog range adjustment (stops if enabled), the 16-bit depth formats' scale and offset, performance metrics, the bounding box, and CPU access to the embedded frame buffer (the `GXPoke*` state is accepted; a peek would stop as an unregistered function).

## VI (`recomp/runtime/vi.cpp`)

`VIConfigure` records the render mode; `VISetNextFrameBuffer`, `VIFlush`, `VISetBlack` and the pre- and post-retrace callbacks are kept. **The retrace** is a periodic interrupt at the console's field rate (59.94 Hz in the 60 Hz setting, 50 Hz otherwise), run as the SDK's retrace interrupt runs: the pre-retrace callback, the flushed frame buffer goes on screen, the post-retrace callback, then the threads in `VIWaitForRetrace` wake. `VIWaitForRetrace` sleeps on a thread queue in guest memory through the lifted `OSSleepThread`, so other threads run meanwhile. A late retrace runs once, and the next is a period after it: the game never sees a burst of retraces after a stall.

## Input: a scripted remote (`recomp/runtime/input.*`, after v0.15.0)

`QILIN_INPUT` connects a virtual Wii Remote on channel 0 and presses its buttons on a schedule, for headless, repeatable runs: `QILIN_INPUT=A@5,A@8.5,B+A@12:0.3` presses A at 5 s and 8.5 s and B with A at 12 s for 0.3 s (0.1 s by default; buttons A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT). WPAD then drives KPAD as KPAD's code expects: the connect callback `(chan, 0)` a moment after `WPADInit`, then the sampling callback at the remote's report rate (200 Hz, to confirm: the game timestamps samples), in which KPAD reads `WPADRead`'s status (buttons +0, acceleration +2..+6, IR objects from +8, device +40, error +41; KPAD's ring keeps 66-byte records with the data format at +64). Nothing is made up for motion or the pointer: no IR objects, acceleration and calibration zero (KPAD's own default). The remote speaker's control protocol is modelled (commands succeed through their callbacks); its sound isn't played yet. With the script and the mixer, A at the title reaches `CSceneTitle::showTitleGirl`, which calls NW4R's `FrSqrt` (`frsqrte`); with the runtime's approximate estimates (`QILIN_APPROX_ESTIMATES`, see `recompiler-core.md`) a 90-second run pressing A every 2 s goes past the title, initialises the Mii library and runs NW4R's reverb through the aux round trip, with no stop.

## Input (`recomp/runtime/wpad.cpp`)

KPAD, the Wii Remote processing, is lifted; WPAD is native at its API. **Until there is a host input backend, no remote is connected:** a state a console is in whenever remotes are off, which KPAD and the Home Button Menu handle as on hardware. `WPADGetWorkMemorySize` is lifted, with the two tail calls under it, so the heap the game makes for WPAD has the console's size. The pointer's settings (sensitivity, the sensor bar's place) are console settings. GameCube controllers: none plugged in.

## Audio (`recomp/runtime/audio.cpp`; v0.15.0)

**AX is lifted whole** (its eight units: voices and priorities, the setters, PB sync, aux callbacks, each frame's command list): it is CPU code with no hardware access, and NW4R's sound system and the Home Button Menu write AX's voice structures directly, so whatever runs the voices must read the SDK's own structures. Native: **AI** (the audio DMA) and **the DSP**.

- **The protocol is AX's own**, read from its code: AX registers a DSP task (microcode, memory, four callbacks) and spins until the task's init callback has run, so `DSPAddTask` runs it before returning. Each **AI DMA interrupt** (every 96 stereo samples at 32 kHz: 3 ms, a periodic interrupt) calls AX's `__AXOutAiCallback`, which, if the DSP is idle, builds and sends the next frame: mail `0xBABE0080`, then the command list's address (the list built the frame before), and queues the next of three 384-byte output buffers. When the DSP has finished a frame, the task's resume callback runs (an interrupt), and AX either starts the pending frame or marks the DSP idle.
- **The command list** is 16-bit words, each command followed by its arguments, addresses in two halves: command 0 (the studio parameters), 1, 2 or 3 by output mode (the mix buffers), 4 (the first voice parameter block), then aux and output commands.
- **The command list in full** (from `__AXNextFrame`): 0 studio; 1/2/3 mix buffer by output mode (0 stereo, 1 surround, 2 Dolby Pro Logic II); 4 first PB; 5/6/7 aux A/B/C (return volume, input buffer, output buffer); 8/9 aux A/B for DPL2 (six buffers); 10 compressor (0x8000, a value, a table); 11 output (master volume, mix buffer, the AI buffer) or 12 for DPL2; 13 the four remote speakers' buffers (18 samples a frame, 6 kHz); 14 end.
- **The PB** (the DSP's copy, 320 bytes; AX's `__AXServiceVPB` copies the CPU's changes in by sync bit, and with none copies state, volume and position back): next +0, resampler type +8, mixer control +12 (32 bits), state +16, mix +20 (12 pairs of volume and per-sample change: L, R, aux A L/R, B L/R, C L/R, S, aux A/B/C S; NW4R's `SetVoiceMix` gives the enable and ramp bits), interaural delay +68, depop +82, envelope volume +106 and per-sample change +108, loop flag +110, format +112 (0 ADPCM, 0x0A PCM16, 0x19 PCM8), loop/end/current address +114/+118/+122, ADPCM coefficients +126, gain +158, predictor/scale +160, history +162/+164, resampling ratio (16.16) +166, fraction +170, four history samples +172, ADPCM loop context +180, one-pole low-pass +186 (on, y, b0, a1 with b0 = 32767 − a1, from `AXGetLpfCoefs`), biquad +194, remote +214. DSP addresses are physical: nibbles for ADPCM, bytes for PCM8, samples for PCM16 (NW4R's `GetSampleByDspAddress`). Volumes: 0x7FFF is 1.0 (NW4R scales by 32767); NW4R spreads every change over a frame's 96 samples.
- **The mixer** carries this out each frame. Exact (what game logic sees): positions, loops and ends (state 0 when a voice without a loop passes its end), DSP-ADPCM with its history and loop context, envelope and mix ramps, aux round trips (the bus to AX's input buffer, the callback's output from an earlier frame mixed back at the return volume), and the write-back. APPROX: the polyphase resampler (a Catmull-Rom cubic: its tables are in the DSP's ROM), rounding, and the compressor (saturation). Stops: biquad, interaural delay, surround or DPL2 output, any surround signal in stereo output, sample data outside MEM1/MEM2. The remote speakers get silence. The AI buffer is written L then R (the order to confirm when there is host output). `QILIN_AUDIO_DUMP=<file.wav>` writes what AI plays: the title's music comes out at sensible levels with nothing clipped.

## NAND (`recomp/runtime/nand.*`; v0.15.0)

The game's saves in a host folder laid out as the flash's own paths (`<root>/title/00010000/534f4d50/data/...`; the title's code from the disc ID). The root is `QILIN_NAND`, or the platform's per-user data folder (`%LOCALAPPDATA%\BeatTheBeat-QilinComp\nand`, `~/Library/Application Support/...`, `$XDG_DATA_HOME` or `~/.local/share/...`): never the repository, since saves are the game's data. The title's data directory and `/tmp` exist before the game runs, as a console's system makes them.

- Paths: absolute as given, relative from the title's home directory; `..` is refused (it would leave the folder).
- Results as the game's backup manager tests them (read from its code): `NANDGetType` -12 for "no save yet", `NANDCheck`'s answer bits (5: no space, 10: too many files; 0 here), `NANDGetStatus` as an existence check (owner and group aren't kept; the permission is owner read/write, as the game creates its files), `NANDMove` failing with EXISTS onto an existing file, as on the console.
- Files: create, open (by the address of the game's file info), read, write, seek, length, close. The first boot writes `/tmp/Riq.dat` (2,592 bytes, with a CRC-32 from the lifted `OSCalcCRC32`), moves it home, and writes the save banner (`NANDInitBanner` lifted).
- Asynchronous requests: accepted at once, their result through the callback 1 ms later. A safe open for reading is a plain open; **for writing** (the SDK's copy-and-commit) it isn't modelled yet and stops. The **Mii database** (`/shared2/menu/FaceLib/RFL_DB.dat`) is the console's, not the game's: the folder has it only if the owner copies in their own; without it the Mii library gets NOEXISTS and goes on, as on a console without one.

## Memory: the arenas (v0.15.0)

The game makes one heap of the whole MEM1 arena and one of the MEM2 arena (`memInitHeap`), so the arenas' bounds are its heap sizes. With `OSInit`'s fallback end the MEM1 heap was 480 KB short, and the game said so itself (`Can't Alloc Heap: Required 749120 B, Free 271088 B`). **MEM1's arena now ends where a console's apploader puts the disc's file table**: the top of MEM1 less `sys/fst.bin`'s size, rounded down to 32 bytes (`0x817ED8C0` for this disc), and the runtime's 64 KB guest scratch lives in the table's place (the game doesn't read the table there: DVD is native), so the game's heap is the console's size. Still to confirm with a Dolphin dump (`lowmem.py`).

## Other native pieces

- **The locked cache** (`LCEnable`, `LCDisable`, `LCQueueLength`): a 16 KB scratchpad filled by DMA. NW4R turns it on and off, but no lifted code addresses it (nothing outside the replaced libraries builds `0xE000xxxx`; only the debugger stub touches the DMA registers), so there is no DMA, and an access would stop under checked memory.
- **SC** (`SCInit`, `SCCheckStatus`, and the settings: progressive scan, 60 Hz, aspect, language, sound mode): the settings are the runtime's, so there is nothing to load.
- **Guest scratch** (`guest_alloc` in `machine.*`): 64 KB OSInit keeps back from the top of the MEM1 arena, for the runtime's own objects the game's code works on (GX's FIFO object, VI's thread queue).

## Native code so far

215 of the 1,969 replaced and dropped functions (v0.15.0; 178 of 2,052 at v0.14.0). v0.13.0's 48 were:

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

## Where bring-up stood at v0.14.0

`main` runs its whole start-up: the video mode, the file manager, the graphics manager (`GXInit` with the SDK's defaults, the display copy set up, the first frame copied out, `VIConfigure`, the first retraces), NW4R's 3D and layout libraries, the controller manager (WPAD, KPAD), and the Home Button Menu's manager, which loads its files from the disc. It stops at `AICheckInit`, where the Home Button Menu starts its sound.

**Exercised now** (v0.14.0's run): 5 files opened and 863,689 bytes read through asynchronous DVD reads, 17 interrupts (5 from the decrementer, through the lifted alarm code and `OSSleepTicks`; the rest retraces and DVD completions), 7 retraces, 1 frame copied out. Still not exercised: a switch between two threads (only one thread has slept and woken yet) and any draw.

Next: **audio** (AI, AX, DSP). NW4R's sound system (lifted) runs from AX's audio-frame callback, so AX is also the clock the game's rhythm timing follows, and it has to run at the console's rate from the start, sounding or not. Then the Vulkan renderer for GX, once the game draws.
