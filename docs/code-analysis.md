# Code analysis: libraries, names and the lifted/replaced boundary

Last updated 2026-11-18 (v0.4.0: the decomp's names ported to PAL; library boundaries; the boundary decided per library).

## Names from the decomp

`tools/analysis/port_symbols.py` ports the Rhythm Heaven Fever decomp's US function names (`third_party/rhf`, CC0) to the PAL build. There's no US executable to compare bytes with, so it matches **size sequences**: our functions (from `qilin cfg`, each up to the next one less trailing padding) against the decomp's, in address order per section. `difflib` aligns the two; in the gaps, groups whose sizes add up to the same total are paired.

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol [--symbols-out pal_symbols.txt] [--list KIND]
```

The PAL symbol list it writes is derived from the game binary, so it stays out of git, like `cfg.json`.

| Result | Decomp functions | What |
|---|---|---|
| anchored | 8,030 | Same size, in a run of at least three equal sizes |
| single | 13 | Same size, alone between differences |
| split | 5 | One decomp function, several of ours: the four register save/restore routines (the decomp has one function with an entry label per register; we have one function per entry, each falling into the next) and `SystemCallVector`, an OS template copied to low memory, whose end label code points at |
| merge | 4 | Several decomp functions, one of ours: hand-written debugger-stub code (`TRKInterruptHandler` …), not lifted |
| changed | 39 | Same place, different size: regional changes, all in game code except one in the OS (the title, logo and wrist-strap screens, save data, the message manager …) |
| us-only / pal-only | 12 / 53 | Regional code, and the debugger stub's interrupt vectors in `.init` (26 of the PAL-only), which the decomp counts as one table. `memset` is among the US-only only because the vectors after it confuse the alignment of `.init`: it is at the same address with the same size in both builds |

**8,043 of the decomp's 8,103 functions (99.3%) match exactly**, and every SDK and middleware library matches function for function except two functions in the OS and four in the debugger stub. Porting the names also checked our function boundaries against the decomp's, function by function, and found three CodeWarrior habits that `qilin cfg` now handles (`recompiler-core.md`): the unreachable `blr` after a final tail call, a function's epilogue after a final call through `bctr`, and static functions reached only by a tail call from inside another function's range.

## Libraries in the PAL build

From the ported names (the decomp's translation units, by directory), each library's PAL range runs from its first to its last function. The ranges are disjoint and in link order. Words are 4-byte instructions; *fp* counts floating-point instructions, *ps* paired singles, *priv* privileged instructions. **Plan**: *lift* (recompiled with the game), *replace* (native at the library's API), or *drop* (debugging code that never runs in a retail game).

| Library | PAL range | Functions | Words | fp | ps | priv | Plan |
|---|---|---|---|---|---|---|---|
| `.init`: OS start-up | `80004000`–`800043C4` | 9 | 241 | 0 | 0 | 2 | replace (the runtime starts the game) |
| `.init`: EABI runtime (`memcpy`, `__fill_mem`, `memset`) | `800043C4`–`80004744` | 3 | 224 | 16 | 0 | 0 | lift |
| `.init`: debugger interrupt vectors, `__init_data` tables | `80004744`–`80006720` | — | — | | | | drop (templates and data) |
| game | `80006F00`–`800DA6A4` | 3,205 | 216,553 | 30,664 | 2,189 | 0 | lift |
| revolution/base (CPU registers: HID, MMCR …) | `800DA6A4`–`800DA8E0` | 27 | 143 | 9 | 0 | 2 | replace |
| revolution/os | `800DA8E0`–`800E81FC` | 251 | 13,895 | 382 | 96 | 42 | replace |
| revolution/mtx | `800E8200`–`800E8CD8` | 21 | 694 | 524 | 295 | 0 | lift |
| revolution/dvd | `800E8CE0`–`800F1A28` | 121 | 9,042 | 0 | 0 | 0 | replace |
| revolution/vi | `800F1A30`–`800F646C` | 43 | 4,751 | 0 | 0 | 0 | replace |
| revolution/pad (GameCube controller) | `800F6470`–`800F7CDC` | 16 | 1,563 | 0 | 0 | 0 | replace |
| revolution/ai | `800F7CE0`–`800F828C` | 11 | 363 | 0 | 0 | 0 | replace |
| revolution/ax | `800F8290`–`800FB5D0` | 75 | 3,280 | 24 | 2 | 0 | replace |
| revolution/axfx (reverb, chorus, delay on the CPU) | `800FB5D0`–`800FC714` | 17 | 1,105 | 225 | 6 | 0 | lift |
| revolution/dsp | `800FC720`–`800FD268` | 14 | 722 | 8 | 0 | 0 | replace |
| revolution/gx | `800FD270`–`80104EF0` | 181 | 7,968 | 477 | 78 | 0 | replace |
| EABI runtime | `80104EF0`–`80107694` | 70 | 2,537 | 192 | 36 | 0 | lift |
| EABI MSL (C library) | `80107694`–`80112124` | 109 | 10,916 | 1,406 | 20 | 0 | lift |
| revolution/exi | `80112130`–`80113EF8` | 22 | 1,906 | 0 | 0 | 0 | replace |
| revolution/si | `80113F00`–`801155D8` | 21 | 1,462 | 0 | 0 | 0 | replace |
| MetroTRK (debugger stub) | `801155D8`–`8011A6A0` | 138 | 5,170 | 12 | 4 | 49 | drop |
| NdevExi2A (development-kit link) | `8011A6A0`–`8011B160` | 15 | 688 | 0 | 0 | 0 | drop |
| revolution/mem (heaps) | `8011B160`–`8011C6D0` | 33 | 1,372 | 0 | 0 | 0 | lift |
| revolution/euart (debug serial) | `8011C6D0`–`8011CA50` | 3 | 224 | 0 | 0 | 0 | replace (no output) |
| revolution/fs | `8011CA50`–`8011E1F0` | 28 | 1,512 | 0 | 0 | 0 | replace |
| revolution/ipc | `8011E1F0`–`8012067C` | 39 | 2,339 | 0 | 0 | 0 | replace |
| revolution/nand | `80120680`–`801244A8` | 78 | 3,978 | 8 | 0 | 0 | replace |
| revolution/sc (console settings) | `801244B0`–`801265D4` | 43 | 2,121 | 0 | 0 | 0 | replace |
| revolution/wenc (Wii Remote speaker encoder) | `801265D4`–`801268AC` | 1 | 182 | 7 | 0 | 0 | lift |
| revolution/arc (U8 archives) | `801268B0`–`80127288` | 12 | 630 | 0 | 0 | 0 | lift |
| revolution/cx (decompression) | `80127290`–`80127890` | 4 | 384 | 0 | 0 | 0 | lift |
| revolution/wpad (Wii Remote) | `80127890`–`8013EBE8` | 230 | 23,766 | 370 | 16 | 0 | replace |
| revolution/bte (Bluetooth stack, under WPAD) | `8013EBF0`–`8016C3C4` | 732 | 46,581 | 8 | 0 | 0 | replace (unused once WPAD is) |
| revolution/usb (under the Bluetooth stack) | `8016C3D0`–`8016D91C` | 14 | 1,363 | 16 | 0 | 0 | replace (unused once WPAD is) |
| revolution/kpad (Wii Remote processing over WPAD) | `8016D920`–`80175494` | 55 | 7,901 | 3,285 | 140 | 0 | lift |
| revolution/tpl (texture files) | `801754A0`–`801757D8` | 4 | 206 | 12 | 0 | 0 | lift |
| revolution/esp (e-ticket services) | `801757E0`–`80175E00` | 9 | 392 | 0 | 0 | 0 | replace |
| revolution/hbm (Home Button Menu) | `80175E00`–`801995B8` | 529 | 36,334 | 4,484 | 270 | 0 | lift |
| nw4r/db (debug screens) | `801995C0`–`8019B628` | 69 | 2,074 | 162 | 4 | 0 | lift |
| nw4r/g3d | `8019B630`–`8019CFEC` | 23 | 1,647 | 332 | 36 | 0 | lift |
| nw4r/lyt (layouts) | `8019CFF0`–`801A9140` | 256 | 12,372 | 1,983 | 46 | 0 | lift |
| nw4r/math | `801A9140`–`801A9684` | 10 | 337 | 178 | 31 | 0 | lift |
| nw4r/snd (sound system: sequencer, streams, voices) | `801A9690`–`801C966C` | 713 | 32,759 | 2,621 | 170 | 0 | lift |
| nw4r/ut (utilities, fonts, file streams) | `801C9670`–`801D4CE4` | 219 | 11,677 | 1,750 | 134 | 0 | lift |
| GameUtil (the game's own framework) | `801D4CE4`–`801EFBF0` | 554 | 27,587 | 3,731 | 140 | 0 | lift |
| RVLFaceLibrary (Miis) | `801EFBF0`–`801F968C` | 118 | 9,895 | 603 | 52 | 0 | lift |

Totals in `.text` and the code part of `.init`: **377,386 words lifted (74%)**, 127,612 replaced (25%), 5,858 dropped (1%).

**Checks of the boundary against the code:**
- **Every privileged instruction is on the replaced or dropped side:** 2 in `.init`'s start-up, 2 in `base`, 42 in the OS, 49 in the debugger stub, none anywhere else. Lifted code never touches the MSR, segment registers, the TLB or `rfi`.
- **Every unknown word (`qilin coverage`) is on the dropped or replaced side:** `.init`'s debugger table and data (48), the debugger stub (53), the OS's `__OSDBJump` template (1). Unknown is 0 in every lifted library.
- **The paired-single code is on the lifted side** (game 2,189, `mtx` 295, HBM 270, nw4r 421, GameUtil 140, KPAD 140), except GX's matrix and vertex helpers (78), which write through the write-gather pipe and are replaced with GX.

**Why each replaced library is replaced:** it drives hardware registers or IOS directly (OS, base, DVD, VI, PAD, AI, AX, DSP, GX, EXI, SI, FS, IPC, NAND, SC, ESP, WPAD, BTE, USB), as Ghoulies replaces the kernel and D3D. The API above each is what the game and the lifted libraries call, so that's where the native code starts. WPAD is replaced at its API, which leaves the Bluetooth and USB stacks below it with no callers.

**Why each lifted library is lifted:** it's software whose exact behaviour matters more than native speed (MSL's `printf` and maths, NW4R's sound sequencer timing, layout animation and font rendering, KPAD's pointer and acceleration filtering, AXFX's effects, the MTX maths with its paired-single rounding), and it reaches the hardware only through a replaced API. The Home Button Menu and the debug screens are lifted for the same reason, if the game reaches them.

**To decide when the runtime reaches them:** whether OS functions with no hardware access (`OSReport`, the alarm and thread bookkeeping that wraps the scheduler) are better lifted, as Ghoulies moved XAPI to the lifted side; and the start-up path, which the runtime runs instead of `__start`.

## The runtime's to-do list (v0.11.0)

`qilin hwaccess` finds, in every function, the loads and stores whose address is a constant outside cached MEM1. Constant propagation over each function's GPRs follows `lis`, `addi`, `ori` and `mr` through the control flow, and a call makes every register unknown. Accesses through pointers loaded from memory aren't seen; the runtime's checked memory stops on those instead. `tools/analysis/runtime_todo.py` joins that with the call graph and the library plan above. It lists what the lifted side needs from the replaced side: the replaced functions it calls directly, its direct hardware accesses, its indirect calls, and its functions the lifter can't lift yet. Its report is derived from the game and isn't committed: run it locally (`--symbols` adds names from `port_symbols.py`'s list).

On the game:

- **Lifted code calls 309 replaced functions directly**, from 615 lifted functions: 111 in GX, 63 in the OS, 44 in WPAD, 27 in AX, 23 in NAND, 11 each in VI and DVD, 7 in SC, and a few in base, AI, PAD and the debug UART, plus 1 in MetroTRK (dropped). This is the API the runtime provides.
- **The only hardware lifted code touches directly is the GX FIFO:** 322 stores to the write-gather pipe (0xCC008000) in 12 functions. These are NW4R's layout and font drawing, the Home Button Menu's copies of it, the Mii library and a few of the game's own drawing routines, where the SDK's inline GX functions were compiled into the caller. Every other hardware access (VI, the DSP interface, EXI, SI, PI, AI, MI, IPC: 689 more FIFO stores and 337 other accesses) is in replaced code. So the runtime needs one mechanism for lifted code's hardware accesses: taking over stores to the FIFO.
- **2,268 indirect calls** in lifted code go through the function table at run time; any that reach replaced code need the replacement to be in the table too.
- **5 lifted-side functions aren't lifted yet**, all because they use Broadway's estimate instructions: `PSMTXInverse`, `PSMTXInvXpose`, `PSVECNormalize` and `PSVECMag` in the MTX library, and NW4R's `FrSqrt`. They need either Broadway's tables from a clean source or native versions.
