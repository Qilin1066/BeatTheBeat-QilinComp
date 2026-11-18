# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

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
