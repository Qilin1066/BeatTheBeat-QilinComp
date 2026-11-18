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
- The difftest's entry stub at 0x80002F00 must be in the starting state on both sides (a function scanning memory reads it).
- Unicorn quirks:
  - its XER API doesn't set the flags, so the harness uses a stub
  - stale translation cache: call `ctl_remove_cache`
  - its write hook misses `dcbz`/`stmw`/`stswi`, so the harness compares all of MEM1
  - it reports the `dcbz` fault address differently
- Library split (`docs/code-analysis.md`): lift game/NW4R/MSL/runtime/etc. (74%); replace OS, DVD, GX, AX, pad, WPAD, NAND… (25%); drop MetroTRK/NdevExi2A (1%).

## Status (v0.6.0)

- 8,171 functions, all 157 jump tables proven, 98.80% of `.text` reached as code (the rest is identified as padding, dead code or data).
- 6,374 functions lifted (slice 1: integer, loads and stores, branches, jump tables).
- Difftest: 50,992 trials (8 per function), 0 unexplained mismatches; 136 s on 16 workers.
- `qilin lift` output is in fixed 256-function chunks and only changed files are rewritten, so rebuilds are incremental.

## Next steps, in order

1. **Lifter slice 3, floating point:** native double/float; `std::fma` for fused ops (keep `-ffp-contract=off` elsewhere); FPSCR; Broadway's `fres`/`frsqrte` estimate tables.
2. **Lifter slice 4, paired singles and GQR quantisation:** Unicorn lacks them, so write a reference model.
3. Register liveness, to cut `out()`/`in()` syncing around calls; difftest under UBSan.
4. Hardware-access profile, exception tables, timing model; then the runtime (OS, DVD, NAND, GX on Vulkan, AX, input).

## Conventions

- Work on `main`; push with `git push origin main`. Releases are `vX.Y.Z`; the version lives only in `project(... VERSION ...)`. Every release gets a `CHANGELOG.md` entry (Keep a Changelog); never rewrite past entries.
- Prose in docs and commit messages: plain, specific, British spelling.
