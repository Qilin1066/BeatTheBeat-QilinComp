# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

### Fixed
From a full review of the runtime (three independent reads, each finding checked against the code before fixing):
- **DVD:** `DVDCancel` and `DVDClose` finish a pending request before returning, its callback run with "cancelled", as the SDK's synchronous cancel does; before, the completion still wrote the file info 1 ms later, after the game may have freed it. Requests are numbered, so a completion for a reused command block does nothing.
- **Threads:** whether a thread is ending is read before the run token goes (the next thread may rewrite its OSThread); a retired host thread no longer leaves a pointer to its destroyed context for reports. `OSLoadContext` unwinds only from the decrementer's handler, and stops, naming itself, anywhere else, instead of escaping uncaught.
- **The time limit** reports at the game's next safe point (an interrupt), not from its own thread while the game runs; a game stuck without safe points still ends 5 s later.
- **GX:** `GX_TEX_DISABLE` on a TEV stage's map is kept (it was truncated into a lookup of map 0); one or three normals follow the vertex format's element count alone, as the command processor parses, and NBT normals stop in the renderer instead of misreading the rest of the vertex; display lists, textures, TLUTs and copies are checked against guest memory; TEV selectors, ops and channels GX doesn't define, fog type 8, copy and display rectangles outside the EFB, a halving copy larger than its source, and a non-positive viewport stop instead of drawing wrong.
- **Renderer:** the depth buffer starts at the far plane (it was undefined); a texture is re-checked after each texture or TLUT load as well as each frame, so a buffer rewritten between draws isn't drawn stale; textures unused for 10 s are freed (the cache grew without bound); an EFB copy's guest bytes are hashed once a frame, not per draw; the sampler's minimum LOD never exceeds its maximum; the window must offer an 8-bit UNORM format that blits can write (no sRGB fallback that would wash the picture out).
- **Audio:** a zero or out-of-memory address in AX's command list stops, instead of becoming a write into low memory.
- **NAND:** a path component with `\` or `:` is refused (on Windows it could leave the flash's folder); paths longer than the flash's 63 characters stop instead of being cut short; read and write buffers are checked against guest memory; a seek outside the file stops (the console's answer isn't pinned down).
- **WPAD:** `WPADDisconnect` disconnects (the connect callback hears it, reports stop) and a button press reconnects, as on a console.
- **OS:** the UART, `__write_file`, `OSPanic` and `OSRegisterVersion` check their guest text.
- **Input script:** `A@:0.3`, `A@nan` and `A@1:` are refused (tested).
- **Interrupts, efficiency:** lifted loops take the slow path only when something is due now, not whenever an I/O completion is queued 1 ms ahead.
- **VI:** `VIWaitForRetrace` delivers what came due while it slept when it turns interrupts back on.

## [0.16.0] - 2026-11-19
The game is seen, heard and controlled: `qilin_run` opens a window, draws with Vulkan 1.3, mixes the game's sound and plays it, and takes the keyboard (or a script) as a Wii Remote. It boots to the title and past the first A. Not yet checked by eye, and not playable end to end.

### Added
- **A scripted Wii Remote** (`QILIN_INPUT`, `recomp/runtime/input.*`): a virtual remote on channel 0 presses buttons on a schedule for headless, repeatable runs. WPAD drives KPAD as KPAD's code expects (connect callback, then sampling callbacks at the report rate, `WPADRead`'s status), with the data format, pointer control and the remote speaker's control protocol; no motion or pointer data is made up. With it, A at the title takes the game to its first sound.
- **The DSP mixer** (`recomp/runtime/audio.cpp`): AX's command list carried out in full, read from AX's and NW4R's code. Exact where game logic looks (play positions, loops and ends, DSP-ADPCM with its loop context, envelope and mix ramps, aux round trips, the write-back AX reads); approximations marked in the code (the polyphase resampler, the compressor); unsupported features stop. `QILIN_AUDIO_DUMP=<file.wav>` records what AI plays.
- **Approximate estimates in the game's build** (`QILIN_APPROX_ESTIMATES`, the owner's decision): where Broadway's `frsqrte`/`fres` table is needed, the exact result rounded to 12 fraction bits (IBM's stated accuracy), marked APPROX. The differential test's build still stops there. The game now runs past the title.
- **A window, the Vulkan renderer, sound and the keyboard** (`host.cpp`, `gx_vulkan.cpp`, `gx_texture.cpp`, `shaders/`): `qilin_run` opens a window and draws GX with Vulkan 1.3 (GX's transform and lighting on the CPU, the TEV in one fragment shader, GX's texture formats decoded and cached), plays AI's output on the default audio device and takes the keyboard as a Wii Remote. SDL3, Khronos' Vulkan headers and glslang are fetched at configure time, pinned by hash, never committed. `QILIN_HEADLESS=1` (or `-DQILIN_HEADLESS_ONLY=ON`) runs as before. Unsupported GX features stop with a message.
- **More of GX on Vulkan:** copies from the EFB to textures (with the copy formats' conversions), mipmaps with the texture object's filters, LOD range and bias (layout read from the SDK's `GXInitTexObjLOD`), lines and points, fog, manual texture-coordinate scaling; Z textures now stop instead of being ignored.
- **The runtime's to-do list from its own lift:** of the 260 replaced functions the game's code can call, 40 had no native code; NAND's asynchronous calls, `NANDPrivateOpen`, `NANDDelete`, `OSPanic`, the OS's exits, `__DVDTestAlarm`, `GXGetPixelFmt`, `__write_file` and `WPADGetInfoAsync` now do, and `OSAlloc.c` is lifted. What's left (display-list recording, the ROM font, the balance board, MotionPlus, pairing) stops when reached.
- **`QILIN_FRAME_STATS=N`:** numbers about every Nth frame (coverage, mean colour), to tell drawn frames from empty ones without looking.
- **Tests:** GX's texture formats on synthetic tiles (I4, I8, IA8, RGB565, both RGB5A3 forms, RGBA8's split halves, C8 through a TLUT, CMPR's two modes, padded sizes, an unknown format stopping) and the input script's parser (presses, combinations, durations, every malformed form).

### Changed
- **One dependency fewer:** the shaders' SPIR-V is committed beside their GLSL with a hash stamp the configure checks; glslang is fetched only with `-DQILIN_BUILD_SHADERS=ON`. A default build needs SDL3 and the Vulkan headers.
- **The input script** is parsed apart from the environment (`parse_input_script`, tested), and its times count from the runtime's start, as documented, instead of from the first `WPADInit`.

### Fixed
- **The renderer** stops on a vertex's matrix index past GX's matrix memory, and on an indexed vertex attribute outside MEM1 and MEM2, instead of reading beyond them.
- **Copies to textures** compare the guest bytes they cover by the copy's own size, so a copy read in a format of another size isn't dropped.

The differential test: 8,108 functions, 64,864 trials, 0 unexplained mismatches.

## [0.15.0] - 2026-11-19
The game runs: it initialises completely, saves on first boot, reaches its title scene and keeps running, a frame copied out on every retrace, with the audio clock at the console's rate. Nothing is shown or heard yet (headless GX; the DSP stops at the first voice), and with no remote connected the game waits at its title.

### Added
- **Audio** (`recomp/runtime/audio.cpp`): AX lifted whole (eight units: CPU code with no hardware access, whose voice structures NW4R writes directly); AI's DMA interrupt every 3 ms (96 stereo samples at 32 kHz); the DSP takes AX's mail (`0xBABE0080`, the command list), decodes the list's set-up and the voice chain, outputs silence while no voice plays and stops when one does. The protocol is AX's own, read from its code.
- **NAND** (`recomp/runtime/nand.*`): saves in a per-user host folder laid out as the flash (`QILIN_NAND` to choose it); create, open, read, write, seek, length, close, move, type, status, check, the home directory, and the asynchronous safe open for reading. Results as the game's backup manager reads them. The first boot writes `Riq.dat` and its banner.
- **Safe points in lifted code:** at every call into native code, and on every backward branch (the lifter emits a poll with the loop's stack pointer: 2,594 in the runtime's lift). A loop waiting for an interrupt gets it, as on the console.
- **The estimate instructions:** `frsqrte` and `fres` lift, exact where the architecture defines them (zeros, infinities, NaNs, negative square roots) and stopping where Broadway's table would be needed. The last five functions that weren't lifted (`PSMTXInverse`, `PSMTXInvXpose`, `PSVECNormalize`, `PSVECMag`, NW4R's `FrSqrt`) now lift: 8,108 functions in the differential test, all 6,202 in the runtime's lift.
- **More lifted out of replaced libraries:** `OSReport`, `OSVReport`, `OSCrc.c`, `NANDInitBanner`, `NANDSetUserData`/`NANDGetUserData`, and the debugger stub's console write (`__TRK_write_console`, `GetUseSerialIO`), which MSL calls on every console.
- **Natives:** the console type (retail), the debug UART (the game's reports to stderr, `[game]`), power and reset callbacks, GameCube pads (none plugged in), `WPADProbe` and `WPADDisconnect` (no remote), `DCFlushRangeNoSync`.
- **Diagnostics:** a guest call stack with every stop; `QILIN_RUN_SECONDS` (ends a run with the counters, every thread's state and stack, and the scheduler's state; exit status 3); counters for audio frames, dropped periodic interrupts and where deliveries happen.
- **Tests:** the estimates, a callee-saved register keeping its constant across a call, a loop's poll.

### Changed
- **Periodic interrupts catch up** when delivered late (each missed audio frame or retrace runs; only a stall beyond 32 periods drops, counted), and `qilin_run` asks Windows for 1 ms timers. Before, four of five audio frames were lost.
- **I/O completions take 1 ms** (queued interrupts have a due time): an instant completion overtook the game's own bookkeeping (it sets its "loading" flag after issuing a read) and left it waiting forever.
- **MEM1's arena ends below the disc's file table**, as a console's apploader puts it (from `sys/fst.bin`'s size), and the runtime's guest scratch lives in the table's place: the game's heap is the console's size (it was 480 KB short, and the game said so).
- **`hwaccess` keeps callee-saved registers across calls** (r1, r2, r13, r14-r31, as the EABI does): 27 more FIFO stores found in lifted code (349 in the runtime's lift), among them the game's sprite drawing (`CCellAnim::draw`).
- The time base has its own file (`recomp/runtime/timebase.cpp`).

## [0.14.0] - 2026-11-19
GX's CPU side: the state the game's GX calls set up, the vertex stream lifted code writes to the FIFO, and VI's retrace. The game initialises its graphics (with the SDK's own GX defaults), controllers and Home Button Menu, loads files from the disc through asynchronous reads, sleeps on alarms, and stops where the Home Button Menu starts its sound. The thread, interrupt, alarm and DVD code from v0.13.0 has now run.

### Added
- **The GX FIFO in lifted code:** the lifter turns the stores `hwaccess` finds on the write-gather pipe (322 in 12 functions in the runtime's lift; `stw`, `sth`, `stfs`; and `psq_st` in the GX library itself) into `rt::fifo_st8/16/32` and `rt::fifo_psq_st`. Other stores stay plain. In the differential test they stop as the original does under Unicorn: 64,824 trials, 0 mismatches, as before.
- **Native GX** (`recomp/runtime/gx.*`, `gx_fifo.cpp`): about 110 setters, loaders and copies keeping GX's state as the API describes it; `GXBegin` and the FIFO stores make draws, `GXCallDisplayList` walks a list's draws, anything else stops. A headless sink counts draws and copies until the Vulkan renderer exists.
- **The SDK's own GX defaults and numbers:** native `GXInit` calls the lifted `__GXInitGX`; `GXSetTevOp` takes its presets from the SDK's tables in the game's data (found through its code); `GXSetDispCopyYScale` ports the SDK's line arithmetic. Lifted out of GX: the texture, TLUT and light object functions, `GXGetTexBufferSize`, `GXGetYScaleFactor`, `GXAdjustForOverscan`, `GXSetTevDirect`, `__GXInitGX`; the natives decode those objects in the SDK's layout, read from its getters.
- **VI:** `VIConfigure`, frame buffers, flush, black, retrace callbacks, and the retrace as a periodic interrupt at the field rate (59.94 or 50 Hz) that wakes `VIWaitForRetrace` (which sleeps through the lifted `OSSleepThread`). Periodic interrupts in `interrupts.cpp`.
- **Input:** WPAD's start-up as KPAD and the controller manager use it, with no remote connected until there is a host input backend; `WPADGetWorkMemorySize` lifted (the console's heap size); GameCube pads: none. Console settings for the pointer and the sound mode.
- **SC** start-up (`SCInit`, `SCCheckStatus`, `SCGetSoundMode`), the **locked cache** (no lifted code addresses it), and **guest scratch** for the runtime's own guest objects.
- `qilin_run` also reports draws, vertices, frames copied out, copies to textures and retraces.
- **Tests:** the lifter hands exactly the FIFO stores to the runtime.

## [0.13.0] - 2026-11-19
The OS and the disc under the game: the OS's threads, mutexes, messages, time and alarms are lifted over a native thread switch and interrupt delivery, and the game's files come from an extracted disc. `qilin_run` gets through the static constructors and `main`'s start-up (video mode, file manager) to its graphics manager's `GXInit`. The new thread, interrupt, alarm and DVD read code is wired in but not yet exercised: the game reaches `GXInit` before it uses any of it.

### Added
- **The OS's software lifted, by translation unit** (`docs/runtime.md`): `OSThread.c`, `OSMutex.c`, `OSMessage.c`, `OSTime.c` and `OSAlarm.c` are lifted out of the replaced OS (`kLiftUnits` in `libraries.cpp`), with exceptions by name (`kNativeAnyway`: `SelectThread`). Names, not addresses. `qilin lift --runtime` fails if a unit or function the plan names isn't in the symbol list, which now carries each function's unit.
- **The thread switch** (`recomp/runtime/threads.cpp`): `SelectThread` ported step by step from the build's own code, each guest thread on its own host thread with a run token handed between them; its globals are read from its instructions and checked at start-up. A new thread starts with the registers `OSCreateThread` left; an exited one ends its host thread. `OSSetCurrentContext`, `OSInitContext` (retires a host thread whose context is reused) and `OSLoadContext` native.
- **Interrupts at safe points** (`recomp/runtime/interrupts.*`): delivered when the game enables interrupts and while the scheduler is idle; queued handlers run as the SDK's dispatcher runs them (scheduler off, handler, scheduler on, reschedule); the decrementer runs the OS's installed handler, whose closing `OSLoadContext` unwinds back to the delivery. `PPCMtdec` sets the deadline; the exception table is native. Idle with nothing that could ever wake a thread stops (`Stop::Idle`).
- **DVD over an extracted disc** (`recomp/runtime/dvd.*`): `qilin_run` takes the folder Dolphin's Extract Entire Disc writes. The disc's own `fst.bin` gives entry numbers and directories; lookup as the SDK's (from the root, `.`/`..`, no case); files found again by the disc offset in the file info, as the SDK does; asynchronous completions arrive as interrupts after the call returns. Eleven DVD functions.
- **Natives that check the code they replace** (`QILIN_NATIVE_CODE`): `qilin_run` refuses to start if the function of that name doesn't begin with the stated instructions (`PPCMtdec`, `PPCHalt`). Natives are bound after the DOL is loaded.
- **VI and SC basics** (`recomp/runtime/vi.cpp`, `console.hpp`): `VIInit`, the TV format and cable, and the console settings the game picks its video mode and language from (60 Hz, progressive, 16:9, English by default; runtime settings until the settings file exists).
- **What a run exercised:** with a stop, `qilin_run` prints its thread switches, threads started, interrupts, files opened and bytes read. Stops are reported from any host thread with function names (`rt::report_stop`); native code calls lifted functions with `rt::call_guest`; `rt::time_base()` is the one 64-bit time base for `mftb` and the decrementer.
- `OSInit` writes the bus and CPU clocks to low memory (243 and 729 MHz), which the SDK's tick conversions read, and runs `__OSInitAlarm` and `__OSThreadInit` in its own order.
- **`tools/analysis/lowmem.py`**: reads a Dolphin MEM1 dump taken at the entry point and prints the low-memory words `OSInit` and `__start` read, next to the runtime's values, flagging those that differ. For confirming the boot values in `docs/runtime.md`.

### Changed
- Low memory is open to lifted code under checked memory: the lifted OS keeps its globals there.
- **`port_symbols.py` checks names against code:** the base library's register accessors must read or write their register (`SIGNATURES`); a name the size alignment put elsewhere moves to the matching function nearby, or the port stops. Eight names moved.
- **The game/base boundary** (`code-analysis.md`, `libraries.cpp`) moves from `0x800DA6A4` to `0x800DA6C0`: the three functions before it are the game's (they return constants and game code calls them), so they are lifted.

### Fixed
- **Eight wrong names in the base library.** A run of equal-size functions had put `PPCMfmsr` … `PPCSync` three functions early, `PPCMtdec` on the function that writes HID0. Natives bound by those names would have replaced the wrong functions; none was yet.
- **`guest.hpp` on zig's macOS target:** with `<chrono>` included before `<cfloat>`, zig's bundled macOS headers lose `DBL_MIN` and `FLT_MIN`, so `dispatch.cpp` didn't compile. The smallest normal values now come from `std::numeric_limits`: same values, no macros. Found by cross-compiling the runtime for Linux and macOS on x86-64 and ARM64 with zig, which now all pass (the first check of `machine.cpp`'s non-Windows memory mapping).

## [0.12.0] - 2026-11-18
The runtime skeleton: `qilin_run` starts the game. It runs `OSInit` natively and the static constructors as lifted code, and stops at the first replaced function it has no native code for, naming it and its caller.

### Added
- **`qilin lift --runtime --symbols <list>`**: the runtime's own lift. Functions of replaced and dropped libraries become calls to native code (`rt::native`), listed in a generated `natives.cpp`; `symbols.cpp` names every function for reports. The differential test's lift is unchanged and still lifts every library.
- **The library plan in code** (`recomp/src/libraries.*`): the lifted, replaced and dropped ranges of `docs/code-analysis.md`, and the functions lifted out of replaced libraries by name: `__init_user`, `__init_cpp`, `exit`, and GX's light-object functions, which only fill in a structure.
- **The registry of native functions** (`recomp/runtime/native.*`): native code registers by the function's name with `QILIN_NATIVE(name)`, so the repository holds no list of PAL addresses. Start-up refuses a registration that matches no replaced function, several, or is repeated. A replaced function without native code stops the game when called (`Stop::Unregistered`).
- **`qilin_run`** (`recomp/runtime/run.cpp`, `machine.*`; CMake `QILIN_RUNTIME_GEN_DIR`): maps MEM1 and MEM2 in a reserved 4 GB guest space, loads the DOL, sets up what `__start` sets up, then calls `OSInit`, `__init_user`, `main` and `exit`. Every stop is reported with function names. Bring-up runs with checked memory: low memory, the uncached mirrors and hardware registers stop lifted code.
- **The first native OS functions** (`recomp/runtime/os.cpp`, 22): `OSInit`, the interrupt enable, the arenas, the cache range functions, `OSRegisterVersion`, `PPCHalt`.
- **`docs/runtime.md`**: how the runtime is built and reached, the boot values still to confirm in Dolphin, and a proposal for the next step: lift the OS's thread, mutex and alarm bookkeeping and replace only the context switch, interrupts and timers under it.
- **Tests** for the library plan, the symbol list reader and the registry.

## [0.11.0] - 2026-11-18
The runtime's to-do list: what lifted code needs from the libraries and hardware the runtime replaces.

### Added
- **`qilin hwaccess`** (`recomp/src/hwaccess.*`): the loads and stores in each function whose address is a constant outside cached MEM1, found by constant propagation over the GPRs through the control flow, classified by region (hardware block, GX FIFO, uncached mirrors, MEM2, EFB, locked cache).
- **`tools/analysis/runtime_todo.py`**: joins the call graph, `qilin hwaccess` and the library plan into the to-do list. Lifted code calls 309 replaced functions directly, and its only direct hardware access is the GX FIFO: 322 stores in 12 functions, from inlined GX calls in NW4R, the Home Button Menu, the Mii library and the game. 5 lift-side functions need the estimate tables. The report is derived from the game and stays out of git.
- **Tests** for the address map and the constant propagation (GX FIFO and VI addresses found, cached MEM1 ignored, registers unknown after a call).

## [0.10.0] - 2026-11-18
The differential test under UBSan, finding no undefined behaviour in lifted code or the runtime across all 64,824 trials. The difftest is now deterministic and isolates its trials.

### Added
- **`QILIN_UBSAN`** (CMake): builds lifted code and `qilin_difftest` with `-fsanitize=undefined -fno-sanitize-recover=all`, for GCC and Clang. CMake refuses it under MSVC, which has no UBSan.
- **`tools/difftest/build_ubsan.py`**: builds the UBSan harness with zig's Clang (`pip install ziglang`) on any host, Windows included, with the project's warnings as errors.
- **A deterministic time base in the difftest** (`QILIN_DETERMINISTIC_TIME`): the k-th time-base read of a trial gives the same value on both sides, so trials that read it are compared (they were known differences) and every trial replays exactly.
- **Trial isolation checks:** Unicorn's whole CPU state is restored before each trial; every 16th trial checks on both sides that MEM1 is the image again; a failure lists the functions the worker ran, for `--only` to replay; a memory mismatch shows which side changed the block, and to what.

### Changed
- **Found:** the rare non-reproducible stops these checks caught were bit flips in the development machine's overclocked non-ECC RAM, not the code. A stress test without any project code reproduced them.
- The roadmap drops "CR bits only where read". `cr` is a local, so the host compiler already removes CR updates nothing reads: Clang folds a record-form update followed by a compare to the compare alone. CR that reaches a call or return is observable, so it has to be computed anyway.

## [0.9.0] - 2026-11-18
Register dataflow around calls: lifted code stores only the registers it changed since the last sync, and reloads only those it reads before overwriting. The game's lifted objects are 10.6% smaller, and the differential test results are unchanged (64,824 trials, 0 unexplained mismatches).

### Changed
- **Sync points** (before a call, return or tail call; after a call) move what two dataflow passes over the function choose, where they used to move every register the function uses: the dirty registers (forward, may) are stored; the live ones (backward, a later store counting as a read) are reloaded. Reads and certain writes come from the emitted code statement by statement, erring towards one store or reload too many. The `out()`/`in()` lambdas are gone.

### Added
- **Tests** for the minimal stores and reloads on the synthetic program. A planted dataflow bug (no reloads after calls) shows up in the difftest as 24 mismatching functions.
- **A Clang check on Windows** without installing LLVM (`ziglang`), described in CLAUDE.md: the generated code is warning-free under Clang 22 with `-Werror`.

## [0.8.0] - 2026-11-18
Lifter slice 4, paired singles: 8,103 of the game's 8,171 functions lift (554 more). The rest are privileged code and FPSCR access in libraries that get replaced, plus five functions that use Broadway's estimate instructions. A reference model stands in for Unicorn, which has no paired singles: 64,824 trials, 0 unexplained mismatches.

### Added
- **Lifter, slice 4**, from IBM's 750CL user's manual: each FPR's ps1 (`c.ps1[]`); quantised loads and stores through the GQRs (types, scales, rounding toward zero, clamping, NaN and infinity by sign, W = 1); paired arithmetic, multiply-adds, sums, merges, moves and compares. `lfs` and single-precision results now fill both halves; double-precision instructions leave ps1 alone, which CodeWarrior's FPR saves in prologues rely on. A reserved GQR type, or a multiplier lane that isn't a single, stops.
- **`fsel`**, which stops on a denormal selector under non-IEEE mode.
- **Difftest reference model for paired singles** (`tools/difftest/paired.py`), independent of the lifted code's helpers: emulates paired-single instructions and GQR moves in a code hook and keeps ps1. Trials get random ps1 values and GQRs; ps1 is compared bit for bit.
- **Tests** for GQR decoding, quantisation, `psq_l`/`psq_st` and the lifter's paired-single output.

### Changed
- **Difftest hooks:** one code hook per text section, filtered in the driver, where there was one per instruction of interest. Unicorn scans its hooks linearly; trials are now faster than before slice 4 (19 ms on one core).
- **Time-base and special-register reads are found dynamically**, in the trial that makes them, replacing a static call-tree analysis that couldn't see calls through function tables and excused whole call trees.

## [0.7.0] - 2026-11-18
Lifter slice 3, floating point: 7,549 of the game's 8,171 functions lift (1,175 more), in the non-IEEE mode the game sets, and every one runs against the original code: 60,392 trials, 0 unexplained mismatches.

### Added
- **Lifter, slice 3:** FP loads and stores, single- and double-precision add, subtract, multiply and divide, fused multiply-adds (`std::fma`), `frsp`, `fctiwz`, moves, sign operations and compares. Through runtime helpers: non-IEEE mode (FPSCR[NI], which `OSInit` sets), with denormal operands and results flushed to zero of their sign; PowerPC's NaN rules (operand order, the positive default QNaN); the architecture's bit conversions for `lfs` and `stfs`; `fctiwz` saturation and the 750 family's high word.
- **Fail loudly on unsourced Broadway behaviour:** a single-precision multiply whose frC isn't a single, and a compare of a denormal, stop (`rt::fp_unknown`). FPSCR access (only in replaced code), FP record forms and `fres`/`frsqrte` aren't lifted, and the reason is reported.
- **Differential test with FP state:** random FPRs (singles, specials, NaN payloads, denormals) compared bit for bit; MSR[FP] set in Unicorn; hooks correct Unicorn's `fctiwz` (high word, positive overflow); the harness counts non-IEEE flushes, a known difference since Unicorn ignores NI.
- **Tests** for the FP helpers (non-IEEE flushing, NaN selection, fused rounding, `fctiwz`, `lfs`/`stfs` round trips) and the lifter's FP output.

### Changed
- The harness checks a top-level function's return as a lifted caller does: a return anywhere but the caller (a stack smashed by random input) is a bad return, a known difference where Unicorn jumps there.

## [0.6.0] - 2026-11-18
A faster loop: the differential test runs in parallel on Linux, macOS and Windows, a full run at 8 trials per function takes about 2 minutes on 16 cores (v0.5.0 took about 45 minutes at 4 trials on two cores), and `qilin lift` rewrites only what changed, so rebuilds are incremental. 50,992 trials, 0 unexplained mismatches.

### Added
- **Parallel differential test:** `--jobs` workers (default: one per CPU), each with its own Unicorn and `qilin_difftest`. Trials are seeded per function (`--seed` and its address), so results don't depend on scheduling, and `--only` (now a comma-separated list) or the new `--range LO-HI` replays exactly the trials of a full run.
- **The differential test on Windows:** a reader thread replaces `select` for the harness's time-out; the harness reserves the guest space but commits only MEM1, and sets binary mode on its pipes.

### Changed
- **`qilin lift`:** files of 256 consecutive functions counted over every function, lifted or not (35 files, was 25 of 300 lifted functions), so boundaries don't move when more functions lift; a file is written only when its content changes, and stale chunks are removed.
- **Generated code** declares each local separately: MSVC applies `[[maybe_unused]]` only to a declaration's first declarator. It now builds with MSVC `/W4 /WX`, less C4702 (unreachable code after inlining a callee that always stops), which is disabled for generated code.
- **Faster trials** (39 ms to 19 ms on one core): Unicorn's MEM1 is a buffer of the driver's, compared with `memcmp`; neither side copies all of MEM1 per trial; the random heap is drawn in bulk. The trials themselves changed with this, so counts differ from v0.5.0's.
- The default is 8 trials per function, and the results are 50,992 trials.

### Fixed
- **Difftest:** the entry stub at `0x80002F00` was written only into Unicorn's memory, so a function reading memory down to it saw different bytes on each side (a false mismatch, `801E9140`). It is now part of the starting state on both sides.

## [0.5.0] - 2026-11-18
The lifter's first slice and the differential test: 6,374 of the game's functions become C++, and every one runs against the original code under Unicorn: 25,496 trials, 0 unexplained mismatches.

### Added
- **Lifter, slice 1** (`recomp/src/lifter.*`): integer arithmetic with carry and overflow, compares, logical, rotate and shift, every integer load and store, the condition-register and XER instructions, traps, `dcbz`, and all control flow: labels and `goto`, jump tables as a `switch`, direct calls with a check of the return address, tail calls, fall-through into the next function, indirect calls through the function table. Registers live in host locals and are written back only around calls and returns. Floating point, paired singles and privileged instructions stop the lifter with the reason.
- **Runtime** (`recomp/runtime/`): `guest.hpp` (the context, big-endian memory, checked in bring-up builds, and the arithmetic and flag helpers, with Broadway's results for division by zero), `dispatch.cpp` (the function table, special registers, the time base).
- **`qilin lift`:** C++ in 300-function files, stubs that stop for what isn't lifted, the function table, and `lift_report.txt` with the reasons.
- **Differential test** (`tools/difftest/`): lifted functions against the original code under Unicorn's PowerPC 750CL model, from random states, comparing registers, memory and how each stopped; works around four Unicorn problems (XER flags, stale translations, a write hook that misses `dcbz`, `stmw` and `stswi`, `dcbz` fault addresses).
- **Tests:** the lifter's output for the synthetic program, the runtime helpers.

### Fixed
- `ori`, `oris`, `xori` and `xoris` with an odd immediate no longer update CR0 (found by the differential test).

## [0.4.0] - 2026-11-18
The decomp's names on the PAL build, and the library boundaries: 8,043 of the decomp's 8,103 functions match ours exactly, and every library has a PAL range and a plan (lift, replace or drop).

### Added
- **`third_party/rhf`:** the Rhythm Heaven Fever decomp's `symbols.txt` and `splits.txt` (US build, CC0).
- **`tools/analysis/port_symbols.py`:** ports the names to the PAL build by aligning function size sequences, classifies every difference (split, merge, regional change, one side only), and prints each library's PAL range. `--symbols-out` writes a PAL symbol list (derived from the game: not committed).
- **`docs/code-analysis.md`:** the 44 library ranges of the PAL build with function, instruction, floating-point, paired-single and privileged counts, and the lifted/replaced/dropped boundary: 74% of the code lifted, 25% replaced, 1% dropped. Every privileged instruction and every unknown word is on the replaced or dropped side.
- **Tests** for the new CFG rules.

### Fixed
- **Function boundaries, checked against the decomp's:** the `blr` CodeWarrior keeps after a final tail call or trap, and the epilogue after a final call through `bctr`, belong to the function (dead code), not to a new one; a branch to a stack-frame set-up is a tail call to a new function, even inside the branching function's range. 8,301 functions became 8,171, and exact matches with the decomp rose from 7,849 to 8,043.

## [0.3.0] - 2026-11-18
Function discovery and control flow, and the first analysis of the PAL `main.dol`: the build hash confirmed, the decoder agreeing with binutils on every word of the game's code, 8,301 functions, every jump table proven, and every word of the code accounted for.

### Added
- **Function discovery + CFG** (`recomp/src/cfg.*`): strong seeds (entry, `bl` targets), recursive descent bounded by the next start, tail calls, `lis`/`addi` pointers, data pointers outside jump tables, gaps; CodeWarrior jump tables proven by their bound check, including bases held in a data-pool register and bounds checked with a conditional return; functions that fall into the next (register save/restore chains); the get-PC idiom; validation of weak seeds. Details in `docs/recompiler-core.md`.
- **Coverage accounting:** every word of the text is code, padding (zero or `nop`), dead (unreachable code the compiler left inside a function, with nothing pointing at it) or unknown.
- **`qilin cfg`, `coverage`, `suspects`.**
- **Tests:** a synthetic program with a function found each way, a switch, a save chain, the get-PC idiom, an indirect tail call, padding and dead code, and a rejected weak seed.
- **`docs/dol-analysis.md`:** the PAL build's sections (named from `__init_data`'s own tables), instruction profile, decoder cross-check, functions and coverage; the 102 unknown words identified (data in `.init`, an SDK template, the debugger stub). The 0.1 phase's observation about `__init_data`'s tables is confirmed.

### Changed
- The supported build's hash is confirmed on the user's copy of `SOMP01` revision 0.

### Removed
- `COPYING` (GPLv3). It was added in the 0.1 phase because that code patched GPL-licensed projects; nothing in the repository derives from them now.

## [0.2.0] - 2026-11-18
A restart on GBTG-Qilincomp's architecture and requirements: our own PowerPC → C++23 recompiler, the system libraries replaced natively, Vulkan 1.3 only. The first pieces are the DOL loader and a Broadway decoder checked against GNU binutils.

### Added
- **The design:** `docs/roadmap-and-design-targets.md` (the Ghoulies targets applied to the Wii: register model, endianness, memory map and mirrors, the system-library boundary, GX and AX, hardware scope, Vulkan baseline, roadmap), `docs/prior-art.md` (the 0.1 phase, DolRecomp/ModernGekko, the Rhythm Heaven Fever decomp, other Wii and PowerPC recompilations, Dolphin, binutils), `docs/naming.md`, `docs/dol-analysis.md`, `docs/recompiler-core.md`.
- **DOL loader** (`recomp/src/dol.*`): sections, BSS (with the small-data sections inside it), entry point, a flat guest image; `std::expected` errors for anything malformed. The supported build is recognised by the SHA-256 of `main.dol` (our own SHA-256, `recomp/src/sha256.*`).
- **Broadway decoder** (`recomp/src/decoder.*`): all 222 instructions of the 750CL with the paired singles, table-driven, with operand fields and flags for the analysis passes and the lifter. Reserved bits set, invalid forms and instructions the 750CL lacks decode as invalid. Text in objdump's `-M broadway,raw` form.
- **`qilin` CLI:** `info`, `insns`, `disasm`, `profile`, `--version`.
- **Decoder cross-check** (`tools/check/verify_decoder.py`) against GNU objdump: every word of the game's text sections, or a sweep of the encoding space with every single-bit neighbour of each valid word. Over ~38 million words (8 seeds), 0 unexplained disagreements; the reviewed differences are listed in the script and in `docs/recompiler-core.md`.
- **Tests** (`qilin_tests`, no game files): SHA-256 vectors, a synthetic DOL and nine malformed ones, 63 encodings pinned to objdump's text, 18 words that must not decode, and for each of the 222 instructions a canonical word plus every reserved bit set.
- **CI** with warnings as errors on Linux x64 (GCC 14, Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, Windows x64 (MSVC), plus the decoder sweep.

### Changed
- **Scope** is GBTG-Qilincomp's: 64-bit Windows, Linux and macOS on x86-64 or ARM64, and ARM boards with Vulkan 1.3. iPadOS is no longer a target.
- **Version** lives in `CMakeLists.txt` (the `VERSION` file is gone).

### Removed
- **The 0.1 phase's code:** the Windows build script and launcher, the ModernGekko and RecompCore patches, and the native boot probe (`native/`). They depended on DolRecomp (generated C, an interpreter fallback) and on a Dolphin-based runtime. The `native-boot` branch was merged into `main` first, so its history is kept; `v0.1.0-alpha.1` is commit `751b042`.
- The 0.1 phase's documents (`BUILD.md`, `NATIVE-PORT.md`, `PROJECT-STATUS.md`, `PROVENANCE.md`, `VALIDATION.md`). What still holds is in `docs/prior-art.md` §1 and `docs/dol-analysis.md`.

## 0.1.x native probe (never released; superseded by 0.2.0)

These were the 0.1 phase's "Unreleased" entries, kept as written.

- Added a C++23 startup probe that runs locally compiled PAL `SOMP01` code and stops at the first unsupported operation.
- Lowered `dcbf` and `dcbst` at compile time for the native probe's coherent RAM model.
- Verified that Windows x64 passes the initial cache-flush loop and reaches the SDK's `mfhid0` instruction.
- Replaced the cold-boot data and cache setup with native initialization while retaining the SDK DMA error-handler registration. Verified that the PAL WBFS build reaches the performance monitor setup in `OSInit` on Windows x64.
- Replaced the PAL startup monitor reset with an inactive native profiling state. The Windows x64 build now reaches the HID4 configuration in `OSInit`.

## 0.1.0-alpha.1 — 2026-11-18

- Added the initial build for PAL `SOMP01` revision 0 with a C++23 host and a Windows x64 ahead-of-time module.
- Required Vulkan 1.3 in the renderer and added device enumeration and extension handling for MoltenVK. Added a macOS build option for a supplied MoltenVK dynamic library.
- Verified Windows Vulkan startup, native dispatch, and the A+B transition from the title screen to save-slot selection.
- Recorded the exact source revisions, local build procedure, and the Windows gameplay validation milestone.
