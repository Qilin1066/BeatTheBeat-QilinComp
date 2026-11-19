# BeatTheBeat-QilinComp

A static recompilation of **Beat the Beat: Rhythm Paradise** (Nintendo, Wii; *Rhythm Heaven Fever* in North America) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as [GBTG-Qilincomp](https://github.com/Qilin1066/GBTGhoulies-Qilincomp)'s.

> **No game data is included, and none ever will be.** You need your own copy of the game: the Europe/Australia release, disc `SOMP01`, revision 0. The tools read `main.dol` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 1, **analysis**, is nearly done; step 2, the **recompiler core**, is in progress: the lifter's integer, floating-point and paired-single slices run, checked against the original code: 8,103 of the game's 8,171 functions lift.

| Piece | State |
|---|---|
| DOL loader (sections, BSS, entry point, flat image; the PAL build recognised by SHA-256) | ✅ |
| Broadway decoder (all 222 instructions of the 750CL with the paired singles; reserved bits and invalid forms rejected) | ✅ 0 unexplained disagreements with GNU binutils over ~38 million swept words and all 512,944 words of the game's code |
| Function discovery + CFG (tail calls, vtables and callbacks, CodeWarrior jump tables, register save/restore chains) | ✅ 8,171 functions, all 157 jump tables proven; every word of the code accounted for (`qilin coverage`: 98.80% code, the rest padding, dead code and 102 identified words of data and debugger stub, none of it lifted) |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep | ✅ |
| Names from the Rhythm Heaven Fever decomp ported to PAL; library boundaries; what's lifted, replaced or dropped | ✅ 8,043 of the decomp's 8,103 functions match exactly; 74% of the code lifted (`docs/code-analysis.md`) |
| Hardware-access profile, exception tables, timing model | next (see `docs/dol-analysis.md`) |
| Lifter → C++, slice 1: integer, loads and stores, branches, calls, jump tables, the function table | ✅ 6,374 of 8,171 functions (the rest need floating point, or are privileged code that's replaced); generated code warning-free under GCC 14 and Clang 19 |
| Differential test against the original code under Unicorn (PowerPC 750CL) | ✅ every lifted function, 64,824 trials, 0 unexplained mismatches; found and fixed one lifter bug; parallel, under 3 minutes on 16 cores |
| Lifter slice 3: floating point (non-IEEE mode as the game sets it, PowerPC NaNs, bit-exact `lfs`/`stfs`; FPSCR is only in replaced code) | ✅ 7,549 of 8,171 functions lift; 60,392 difftest trials, 0 unexplained mismatches |
| Register dataflow: around each call, store only what changed and reload only what's read | ✅ lifted objects 10.6% smaller, difftest unchanged |
| Runtime to-do list: replaced functions lifted code calls, its direct hardware accesses (`qilin hwaccess`, `tools/analysis/runtime_todo.py`) | ✅ 309 replaced functions called; the GX FIFO is the only hardware lifted code touches |
| Differential test under UBSan (`-DQILIN_UBSAN=ON`, or `tools/difftest/build_ubsan.py` on any host) | ✅ no undefined behaviour in 64,824 trials |
| Lifter slice 4: paired singles and GQR quantisation (from IBM's 750CL manual; a reference model in the difftest, since Unicorn has none) | ✅ 8,103 of 8,171 functions lift; the rest are replaced libraries and 5 estimate-instruction users |
| Runtime skeleton: `qilin_run`, the registry of native functions, the runtime's own lift (`qilin lift --runtime`) | ✅ starts the game and stops, naming it, at the first replaced function without native code (`docs/runtime.md`) |
| Runtime: the OS's threads and alarms (lifted, over a native switch and interrupt delivery), DVD over an extracted disc | 🔨 the game runs its start-up up to `GXInit`; the thread, interrupt and DVD code isn't exercised yet (the game reaches GX first) |
| Runtime: OS, DVD, NAND, GX on Vulkan, AX audio, input | later (see roadmap) |

The repository's first version (0.1.0-alpha.1, a Dolphin-based preview, and a native boot probe on top of DolRecomp) was written with ChatGPT and has been retired. `docs/prior-art.md` §1 says what it did and what carried over.

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## Building

The whole project (recompiler, runtime and generated code) is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older, or if the target is 32-bit. There are no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Using it

Extract `main.dol` from your disc first (`docs/dol-analysis.md` says how, with Dolphin).

```sh
build/qilin info    path/to/main.dol [--json info.json]   # SHA-256, supported build, entry point, sections, BSS (JSON)
build/qilin profile path/to/main.dol                      # instruction profile of the code
build/qilin insns   path/to/main.dol [-o listing.txt]     # every word of the code, decoded
build/qilin disasm  words.bin [--base 0x80004000]         # a file of big-endian words, decoded
build/qilin cfg     path/to/main.dol [--json cfg.json]    # function discovery + control flow, summary
build/qilin coverage path/to/main.dol [--list]            # every word: code, padding, dead or unknown (exit 3 if any unknown)
build/qilin suspects path/to/main.dol                     # functions with problems, for review
build/qilin lift    path/to/main.dol --out gen            # lift to C++ (gen/ is gitignored: never commit it)
```

### Differential test

```sh
cmake -S . -B build-gen -DQILIN_GEN_DIR=$PWD/gen && cmake --build build-gen --target qilin_difftest
python3 tools/difftest/difftest.py --harness build-gen/qilin_difftest --dol path/to/main.dol --gen gen
python3 tools/difftest/difftest.py ... --only 801E9140         # replays exactly that function's trials from the full run
```

Runs every lifted function from random states, as lifted C++ and as the original code under Unicorn's PowerPC 750CL model (`pip install unicorn`), and compares registers, memory and how each stopped. It uses one worker per CPU (`--jobs`) and runs on Linux, macOS and Windows. Details in `docs/recompiler-core.md`.

### Running the game (bring-up)

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol --symbols-out pal_symbols.txt
build/qilin lift path/to/main.dol --out gen-rt --runtime --symbols pal_symbols.txt
cmake -S . -B build-rt -DQILIN_RUNTIME_GEN_DIR=$PWD/gen-rt && cmake --build build-rt --target qilin_run
build-rt/qilin_run path/to/extracted-disc
```

`qilin_run` takes the folder Dolphin's **Extract Entire Disc** writes (`sys/` and `files/`); a `.wbfs` or `.iso` can't be read directly, since a Wii disc is encrypted with a key this project can't include. It doesn't get far yet: it stops at the first SDK function the runtime has no native code for, and says which, who called it, and what the run did before. `docs/runtime.md` has the details. Keep `pal_symbols.txt` and the extracted disc outside the repository.

### Decoder check

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol   # every word of the game's code
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep                  # the encoding space (CI runs this)
```

### Names and libraries

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol [--symbols-out pal_symbols.txt]
```

Ports the decomp's US function names to PAL addresses and prints each library's PAL range (`docs/code-analysis.md`). The symbol list is derived from the game: don't commit it.

This compares the decoder with GNU objdump (`-M broadway,raw`): mnemonic and every operand must match, and a word must be valid for both or neither. It needs Python 3 and a PowerPC objdump: `apt install binutils-powerpc-linux-gnu`, or devkitPPC's `powerpc-eabi-objdump` with `OBJDUMP` set. Reviewed differences (binutils accepts some encodings Broadway reserves and lacks `mftb`) are counted by class; anything else fails.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `dol` loader, `sha256`, `decoder` (Broadway), `cfg` (function discovery + control flow), `lifter` (Broadway → C++), `libraries` (what's lifted, replaced or dropped), `main` (CLI) |
| `recomp/runtime/` | What lifted code needs at run time: `guest.hpp` (context, big-endian memory, arithmetic and flag helpers), `dispatch.cpp` (the function table). The game itself: `run.cpp` (`qilin_run`), `machine` (guest memory), `native` (the registry of native functions), `os.cpp` (the OS, natively) |
| `recomp/tests/` | Unit tests on a synthetic in-memory DOL, the decoder and a synthetic program, so they need no game files |
| `tools/check/` | The decoder cross-check against GNU objdump |
| `tools/difftest/` | The differential test: `harness.cpp` runs lifted code, `difftest.py` the original under Unicorn |
| `tools/analysis/` | `port_symbols.py`: the decomp's names and libraries ported to PAL |
| `third_party/rhf/` | The Rhythm Heaven Fever decomp's symbols and splits (CC0) |
| `docs/` | Roadmap and design targets, prior art, analysis and status notes |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, PowerPC → C++, compiled ahead of time.** No interpreter, no JIT, no emulator underneath. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** GX is translated at API level to Vulkan, with a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The SDK's hardware-facing libraries are replaced natively**, not recompiled: OS, GX, AX, DVD, VI, WPAD and NAND. The C/C++ runtime, the SDK's pure-software libraries and NintendoWare (NW4R) are lifted with the game, for exact behaviour.
- **Ground truth is Dolphin** running the original game, rhythm timing included. Names come from the CC0 Rhythm Heaven Fever decompilation.

See `docs/` for the full roadmap and the analysis notes.
