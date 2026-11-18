# BeatTheBeat-QilinComp

A static recompilation of **Beat the Beat: Rhythm Paradise** (Nintendo, Wii; *Rhythm Heaven Fever* in North America) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as [GBTG-Qilincomp](https://github.com/Tecboy1066/GBTGhoulies-Qilincomp)'s.

> **No game data is included, and none ever will be.** You need your own copy of the game: the Europe/Australia release, disc `SOMP01`, revision 0. The tools read `main.dol` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 1, **analysis**, is in progress; the recompiler core (step 2) has its first pieces.

| Piece | State |
|---|---|
| DOL loader (sections, BSS, entry point, flat image; the PAL build recognised by SHA-256) | ✅ |
| Broadway decoder (all 222 instructions of the 750CL with the paired singles; reserved bits and invalid forms rejected) | ✅ 0 unexplained disagreements with GNU binutils over ~38 million swept words and all 512,944 words of the game's code |
| Function discovery + CFG (tail calls, vtables and callbacks, CodeWarrior jump tables, register save/restore chains) | ✅ 8,301 functions, all 157 jump tables proven; every word of the code accounted for (`qilin coverage`: 98.83% code, the rest padding, dead code and 102 identified words of data and debugger stub, none of it lifted) |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep | ✅ |
| Library boundaries and names from the Rhythm Heaven Fever decomp, ported to PAL | next (see `docs/dol-analysis.md`) |
| Lifter → C++, differential test | later (see roadmap) |
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
```

### Decoder check

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol   # every word of the game's code
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep                  # the encoding space (CI runs this)
```

This compares the decoder with GNU objdump (`-M broadway,raw`): mnemonic and every operand must match, and a word must be valid for both or neither. It needs Python 3 and a PowerPC objdump: `apt install binutils-powerpc-linux-gnu`, or devkitPPC's `powerpc-eabi-objdump` with `OBJDUMP` set. Reviewed differences (binutils accepts some encodings Broadway reserves and lacks `mftb`) are counted by class; anything else fails.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `dol` loader, `sha256`, `decoder` (Broadway), `cfg` (function discovery + control flow), `main` (CLI) |
| `recomp/tests/` | Unit tests on a synthetic in-memory DOL, the decoder and a synthetic program, so they need no game files |
| `tools/check/` | The decoder cross-check against GNU objdump |
| `docs/` | Roadmap and design targets, prior art, analysis and status notes |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, PowerPC → C++, compiled ahead of time.** No interpreter, no JIT, no emulator underneath. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** GX is translated at API level to Vulkan, with a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The SDK's hardware-facing libraries are replaced natively**, not recompiled: OS, GX, AX, DVD, VI, WPAD and NAND. The C/C++ runtime, the SDK's pure-software libraries and NintendoWare (NW4R) are lifted with the game, for exact behaviour.
- **Ground truth is Dolphin** running the original game, rhythm timing included. Names come from the CC0 Rhythm Heaven Fever decompilation.

See `docs/` for the full roadmap and the analysis notes.
