# CLAUDE.md

Static recompilation of **Beat the Beat: Rhythm Paradise** (Wii, PAL `SOMP01` rev 0) to native code. Same requirements and scope as the sister project Qilin1066/GBTGhoulies-Qilincomp.

## Requirements (non-negotiable)

- **Efficiency first, then accuracy, and fail loudly.** Never paper over an unknown: stop with a clear error (`rt::Stop`, exit codes, CMake `FATAL_ERROR`).
- **C++23** everywhere: recompiler, runtime and generated code.
- **Vulkan 1.3 only** for graphics. No OpenGL, D3D or Metal backends (macOS goes through MoltenVK).
- **Static recompilation only.** No interpreter, JIT or emulator at runtime, not even as a fallback.
- Targets: 64-bit **Windows, Linux, macOS** on **x86-64 and ARM64**. The low-end reference is ARM boards like the **Raspberry Pi 4**. No iPadOS, no 32-bit.
- Compiler minimums: GCC 14, Clang 19, Apple Clang (Xcode 16), MSVC 2022 17.10.
- No GPL code. The only third-party data is the CC0 decomp symbol list in `third_party/rhf/`.
- Never commit game data or anything derived from it: `main.dol`, `gen/`, `cfg.json`, ported symbol lists. These are gitignored.

## Layout

- `recomp/src/`: `dol` (loader, known-build SHA), `decoder` (table-driven, 222 ops, X-macro `QILIN_PPC_OPS`), `cfg` (function discovery, jump tables, coverage), `lifter` (one C++ function per guest function), `main.cpp` (the `qilin` CLI).
- `recomp/runtime/`: `guest.hpp` (Context, memory, flag and arithmetic helpers, `Stop`), `dispatch.cpp` (`rt::call`, SPRs, time base).
- `recomp/tests/tests.cpp`: unit tests (`ctest`).
- `tools/check/verify_decoder.py`: compares the decoder with binutils objdump.
- `tools/difftest/`: runs lifted code and the original under Unicorn and compares the results, one worker per CPU. Runs on Linux, macOS and Windows. Trials are seeded per function, so `--only`/`--range` replay a full run's trials exactly.
- `tools/analysis/port_symbols.py`: ports US decomp names to PAL.
- `docs/`: design and results. Keep them current. `recompiler-core.md` covers the decoder, CFG, lifter and difftest; `roadmap-and-design-targets.md` is the roadmap.

## Build and check

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
build/qilin coverage path/to/main.dol          # must exit 0 (no unknown words)
build/qilin lift path/to/main.dol --out gen
```

Differential test (`pip install unicorn`; on Windows with the Visual Studio generator the harness is `build-gen/Release/qilin_difftest.exe`):

```sh
cmake -S . -B build-gen -DQILIN_GEN_DIR=$PWD/gen && cmake --build build-gen --target qilin_difftest --parallel
python3 tools/difftest/difftest.py --harness build-gen/qilin_difftest --dol path/to/main.dol --gen gen [--only A,B] [--range LO-HI] [--jobs N]
```

Before a milestone commit, all of these must hold: tests pass, warnings are clean (`QILIN_WERROR`), generated code is warning-free under GCC, Clang and MSVC, and the difftest has **0 unexplained mismatches**. Any new Unicorn quirk goes in `known_difference` with a comment explaining why.

## Known facts worth not rediscovering

- The DOL SHA-256 is `2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe`. Entry point, sections and real start-up register values (r1 0x803E7488, r2 0x8032A680, r13 0x80328500) are in `docs/dol-analysis.md`.
- Broadway divide by zero or overflow: `divw` gives 0xFFFFFFFF for a negative dividend, else 0; `divwu` gives 0. (Unicorn differs; this is a known difference.)
- Generated code under MSVC: `[[maybe_unused]]` covers only a declaration's first declarator (so one declaration per local), and C4702 is disabled for generated code (optimiser-dependent).
- FPSCR is only touched by replaced or dropped code (base, OS, MetroTRK), so lifted code doesn't model it. `OSInit` calls `PPCSetFpNonIEEEMode`: the game runs with NI = 1 (denormal operands and results become signed zero, per the IBM 750GX/GL manual).
- Where Broadway's FP result has no source (fmuls-family frC that isn't a single, compares of denormals, `fres`/`frsqrte` tables), lifted code stops (`rt::fp_unknown`) or the function isn't lifted. Don't guess these, and don't take them from Dolphin's code (it's GPL).
- Paired singles follow IBM's *PowerPC 750CL RISC Microprocessor User's Manual* (cited by title; the repo doesn't link to or include copies of it): `lfs` and single-precision results fill ps0 and ps1; `lfd`, double ops and `fmr` leave ps1 alone (CodeWarrior saves FPRs with `stfd` and `psq_st`); GQR quantisation rounds toward zero and clamps. Unicorn has none: `tools/difftest/paired.py` is the reference model (keep it independent of `guest.hpp`).
- Register dataflow (`run()` in `lifter.cpp`): sync points are `@OUT@`/`@IN@` markers; stores are the dirty registers, reloads the live ones. Any runtime helper that writes a local by reference must be listed in `writes()` (today only `rt::psq_l`).
- Clang on Windows without installing LLVM: `pip install ziglang`, then `python -m ziglang c++ -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror -ffp-contract=off -fno-fast-math -Igen -Irecomp/runtime -c gen/x.cpp -o x.o` (not `-fsyntax-only`: zig adds `-c`). GCC 14 still has to be checked on Linux.
- UBSan: `-DQILIN_UBSAN=ON` (GCC/Clang), or on Windows `python -I tools/difftest/build_ubsan.py --gen gen --out <dir>` (zig's Clang). v0.10.0: no undefined behaviour in the full run.
- CR bits only where read: not worth doing. Clang already folds dead CR updates (`cr` is a local); CR reaching a sync is observable.
- The difftest's time base is deterministic (`QILIN_DETERMINISTIC_TIME` in the harness, emulated `mftb` under Unicorn): the k-th read of a trial is `0x100000000 + k * 0x1000`, so every trial replays exactly with `--only`.
- A difftest failure that doesn't replay may be hardware: the development PC's overclocked non-ECC RAM flipped bits (found 2026-11-18 with a buffer stress test). Every 16th trial checks MEM1 against the image on both sides.
- Unicorn scans code hooks linearly: register one hook per text section and filter in Python, never one per address (30x slower).
- Special-register reads (not LR, CTR, XER, GQRs) are detected dynamically in the difftest (calls through function tables, such as `__init_cpp`'s constructors, defeat static call trees).
- Unicorn's FP: it ignores NI (the harness counts flushes, so these are known differences), and `fctiwz` gives a 0 high word and `0x80000000` on positive overflow (corrected by hooks).
- The harness treats a top-level return to anywhere but the caller as a bad return, as lifted callers do.
- The difftest's entry stub at 0x80002F00 must be in the starting state on both sides (a function scanning memory reads it).
- Unicorn quirks:
  - its XER API doesn't set the flags, so the harness uses a stub
  - stale translation cache: call `ctl_remove_cache`
  - its write hook misses `dcbz`/`stmw`/`stswi`, so the harness compares all of MEM1
  - it reports the `dcbz` fault address differently
- Library split (`docs/code-analysis.md`): lift game/NW4R/MSL/runtime/etc. (74%); replace OS, DVD, GX, AX, pad, WPAD, NAND… (25%); drop MetroTRK/NdevExi2A (1%).

## Status (v0.10.0)

- 8,171 functions, all 157 jump tables proven, 98.80% of `.text` reached as code (the rest is identified as padding, dead code or data).
- 8,103 functions lifted (slices 1, 3 and 4: integer, floating point, paired singles). Left: 56 privileged and 7 FPSCR (replaced libraries), 5 `fres`/`frsqrte`.
- Difftest: 64,824 trials (8 per function), 0 unexplained mismatches; 170 s on 16 workers.
- Calls store only dirty registers and reload only live ones (lifted objects 10.6% smaller).
- Difftest under UBSan: no undefined behaviour.
- Generated code is warning-free under MSVC 17.14 and Clang 22; GCC 14 is unchecked (no Linux here), so v0.6.0-v0.10.0 aren't pushed yet.
- `qilin lift` output is in fixed 256-function chunks and only changed files are rewritten, so rebuilds are incremental.

## Next steps, in order

1. Hardware-access profile, exception tables, timing model; then the runtime (OS, DVD, NAND, GX on Vulkan, AX, input).

## Conventions

- **Never push (or tag, or publish anything) without the owner's explicit go-ahead, and only what is clearly legal for them to share with the world:** no game data or anything derived from it, no copied third-party text or code (IBM manuals are cited by title, not linked or quoted; nothing from Dolphin). Commit locally meanwhile.
- Work on `main`; push with `git push origin main` once told to. Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
