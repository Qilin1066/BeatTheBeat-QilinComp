# Recompiler core: status (roadmap steps 1–2)

Last updated 2026-11-18 (v0.5.0: lifter slice 1, integer and control flow, and the differential test against Unicorn; v0.4.0: function boundaries checked against the decomp's, three CodeWarrior habits handled; v0.3.0: function discovery and CFG, coverage accounting, everything run on the PAL `main.dol`; v0.2.0: DOL loader, Broadway decoder, the binutils cross-check, CI).

## Language and toolchain
Everything is **C++23**: the recompiler, the runtime and the generated code. The C++23 features in use so far are `std::expected` (the DOL loader), `std::print`/`std::format` (the tools), `std::ranges` algorithms and `std::string::contains`.

Minimum compilers are GCC 14, Clang 19, Xcode 16 and Visual Studio 2022 17.10. CMake checks these at configure time, and refuses 32-bit targets. Clang 18 is excluded because it can't use libstdc++'s `std::expected`.

Runtime code (what players run: the DOL loader and SHA-256 so far) avoids `std::format`, because the macOS runtime is built for 10.15 and Apple's libc++ only has it from 13.3. The developer tools are built for macOS 13.3.

CI builds with warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow`, MSVC `/W4`) and runs the tests on six configurations:
- Linux x64 with GCC 14 (plus the decoder sweep, below)
- Linux x64 with Clang 19
- Linux ARM64 with GCC 14
- macOS ARM64 and macOS x64 (Intel): the runtime built for macOS 10.15, the tools for 13.3
- Windows x64 with MSVC

## Done

### DOL loader (`recomp/src/dol.*`, `sha256.*`)
It parses the header's 7 text and 11 data slots, BSS and entry point, and builds the flat guest image from the lowest section to the end of the highest section or BSS. Sections inside the BSS range (the small-data sections a Wii linker puts there) keep their bytes; the rest of BSS is zero. `Dol::load`/`parse` return `std::expected<Dol, std::string>`: a malformed file is an error message, never a crash. It rejects:
- sections whose bytes lie outside the file;
- sections or BSS outside MEM1 (`0x80000000`–`0x81800000`);
- sections that overlap in memory;
- code that isn't word-aligned;
- an entry point that isn't an instruction in a text section.

The SHA-256 of the whole file identifies the build; `known_build()` lists the one this port supports. The SHA-256 is our own (FIPS 180-4), tested against the standard vectors and the padding edges.

### Decoder (`recomp/src/decoder.*`)
Our own, table-driven. One row per base instruction (**222**: the 750CL's integer, floating-point, load/store, branch, condition-register, system, cache and synchronisation instructions, and the paired singles), with its opcode, extended opcode, operand fields in objdump's order, and flags (record/overflow/link/absolute bits, branch, load, store, float, paired, privileged, cache, sync) for the analysis passes and the lifter. The decode tables are built once: one slot per primary opcode, and for the five primary opcodes with extended opcodes (4, 19, 31, 59, 63) a slot per value of the 10-bit extended-opcode field (bits 21–30), filled from the rows (10-bit, 9-bit XO, 6-bit and 5-bit A-form extended opcodes). Bit numbers in these docs are IBM's, from 0 at the most significant. Building the tables fails if two rows claim the same encoding.

**Every bit an instruction doesn't define must be zero**, so a word decodes as invalid when:
- a reserved bit is set (including the fields later Power ISA versions added: branch hints, `L` fields, `TH`, `EH`, …);
- it's an invalid form: a load with update whose base is 0 or its target; a store, floating-point or paired-single update with base 0; `lmw`/`lswi` with the base among the registers loaded; `lswx` with `rD` equal to `rA` or `rB`; a `bc`/`bclr`/`bcctr` with a non-zero "z" bit in BO; `bcctr` that decrements CTR; a compare with `L=1` (64-bit);
- it isn't a 750CL instruction (`fsqrt`, `fsqrts`, `tlbia`, the 603e's `tlbld`/`tlbli`, 64-bit and AltiVec instructions …).

**Cross-check against GNU binutils 2.42** (`tools/check/verify_decoder.py`, `objdump -M broadway,raw`): the text of every word, mnemonic and every operand, must match, and a word must be valid for both or neither.

| Set | Words | Result |
|---|---|---|
| Sweep of the encoding space, 8 seeds | ~38 million | **0 unexplained disagreements**; 4.15 million valid words per seed identical |
| Every word of the PAL `main.dol`'s text sections | 512,944 | **0 unexplained disagreements**; 505,952 valid and identical |

The sweep takes random words for every primary opcode and every extended-opcode value (each register field left zero half the time, so valid forms of instructions with reserved fields come up), then every single-bit neighbour of every word either side accepts, which tests the edge of each instruction's reserved bits. CI runs one seed (about 4.8 million words, 30 s).

The reviewed disagreements, all counted by class and none failing the check:
- **binutils leaves out `mftb`** in its `broadway` and `750cl` sets; the 750CL has it, and the SDK's `OSGetTime` uses it.
- **binutils accepts what the 750CL reserves:** later ISAs' fields (`bclr`/`bcctr` BH, `dcbt`/`dcbtst` TH, `lwarx` EH, `dcbf`/`sync`/`tlbie`/`mtmsr` L, `mtfsf` L and W, `sc` LEV, `mfocrf`/`mtocrf`), compare `L=1`, the reserved bit 9 of compares, bit 15 of `fres`/`frsqrte`, bit 31 of the paired-single indexed forms.
- **binutils accepts instructions the 750CL doesn't have:** `fsqrt`, `fsqrts`, `tlbia`, `tlbld`, `tlbli`.
- **binutils doesn't check some invalid forms:** paired-single updates with `rA=0`, `lswi` with `rA` among the registers loaded, `bcctr` that decrements CTR.
- **Formatting only:** objdump's raw mode prints `mfcr`'s absent field as `-1`. Elsewhere our text follows objdump's (`addi r3,0,0`, `bclr 20,lt,0`, `lwarx r3,r4,r5,0`, `psq_l f1,8(r0),0,0` although `rA=0` means 0 there).

**Unit tests** (`recomp/tests/tests.cpp`, no binutils or game files), for the decoder: 63 encodings pinned to objdump's exact text; 18 words that must not decode, each with its reason; for each of the 222 instructions, a canonical word that must decode to it and, for every bit it doesn't define, the same word with that bit set, which must not decode; field accessors (sign extension, branch targets, the paired-single fields) and mnemonic suffixes.

### Function discovery + CFG (`recomp/src/cfg.*`)
CodeWarrior makes this simpler than on x86: every instruction is one aligned word, switch tables and constants live in data sections, so text holds code and padding, and each function is one contiguous range. It runs in 1.2 s on the game.

1. **Strong seeds:** the entry point and every `bl` target from a linear sweep of the text. A `bl` to the next instruction is the get-PC idiom (`bl $+4; mflr`), not a call.
2. **Recursive descent**, each function bounded by the next known start. Calls continue; `blr`, `bctr`, `b`, `rfi` and an unconditional trap end a path; conditional returns and branches fork it. Execution that runs past the bound into the next function is recorded (`falls_into`): CodeWarrior's register save/restore routines are one routine with an entry point per register.
3. **Tail calls:** a `b` (or `bc`) past the bound to a *clean boundary* (a valid word that starts its section or follows code that can't fall through, or padding) becomes a start. So does one into another function's span when the branching function is strongly seeded: that span was explored before the start was known, and CodeWarrior never branches between functions except to an entry. `lis`/`addi` (or `ori`) pairs in code that build an address at a clean boundary are starts too (callbacks, thread entries).
4. **Jump tables**, CodeWarrior's pattern (`cmplwi`, `bgt`, `lis`, `slwi`, `addi`, `lwzx`, `mtctr`, `bctr`, in any order). The search walks back from the `bctr` on the straight line, follows the one branch into a block above an early return, and finds the table base in a data-pool register (`lis r18` early in the function, `addi r3, r18, n` at the switch: one writer, and for a volatile register no call after it; calls preserve r14–r31 under the EABI). **A table is proven** when a `cmplwi` on the index register, followed by `bgt` (or `bge`, or a conditional return) on that CR field, bounds it, and every entry lands inside the function on a valid instruction; otherwise it's read while entries are plausible and reported. Every table in the game is proven.
5. **Weak seeds, in rounds until nothing changes:** pointers into text stored in data (vtables, `.ctors`, `extabindex`, callback tables), except words inside a jump table: every table a linear scan of the code can prove is excluded first, so a function reached only through a vtable doesn't have its case labels taken for functions. Then the first word of each run of unreached code that starts at a clean boundary and isn't inside a function's span (gaps). A weak seed must not fall inside another function's span.
6. **CodeWarrior's leftovers.** Checking our boundaries against the decomp's (`code-analysis.md`) found three habits, now handled. (a) After a function's final tail call (`b` or `bctr`), or a trap, CodeWarrior keeps the function's own `blr`; after a final call through `bctr` (the OS's `Run`), its whole epilogue (`addi r1, r1, n` … `blr`). That code is unreachable and belongs to the function: gap seeding steps over it and coverage counts it dead, unless something branches to it or points at it, in which case it's a function of its own (a lone `blr` reached through a vtable is an empty method). (b) A stack frame is only ever set up at a function's entry, so a branch to `stwu r1, -n(r1)` (possibly after `mflr r0`) at a clean boundary is a tail call to a new function, even inside the branching function's range: the Bluetooth stack's state machine reaches each state handler that way.
7. **Validation:** a weakly seeded function that reaches an invalid word or runs off its section is dropped and never re-seeded; a strongly seeded one is a suspect. Branches to targets that are neither inside the function nor a clean boundary are suspects too.

**Coverage** (`qilin coverage`) accounts for every word of the text: **code** (reached), **padding** (zero or `nop`, not reached), **dead** (not reached, inside a function, after code that can't fall through, and no branch in reached code nor any pointer in data targets any word of it) or **unknown**. Results on the game are in `dol-analysis.md`: 98.80% of `.text` is code, and the 102 unknown words are identified data and debugger-stub code that isn't lifted.

**Tests:** a synthetic program with a function found each way (entry, call, tail call, `lis`/`addi`, vtable, gap), a switch with its case after the default, a register-save chain, the get-PC idiom, an indirect tail call, a tail call into a frame set-up, the leftover `blr` and epilogue, padding and dead code, checked function by function and word by word; and a data pointer to code that runs into an invalid word, which validation must reject.

### Lifter, slice 1 (`recomp/src/lifter.*`, runtime `recomp/runtime/guest.hpp` and `dispatch.cpp`)
One C++ function per guest function, `void fn_80004050(rt::Context& c)`. Slice 1 covers the integer instruction set: arithmetic with carry and overflow, compares, logical, rotate and shift, every integer load and store (update, indexed, byte-reversed, multiple, string, `lwarx`/`stwcx.`), the condition-register instructions, `mfcr`/`mtcrf`/`mcrxr`, LR/CTR/XER moves, `mftb`, `dcbz`, traps and `sc`, and all control flow.

- **Register model:** the GPRs a function uses, LR, CTR, CR and XER are host locals, loaded from the context on entry and written back only around calls and returns (`out()`/`in()`), so the compiler keeps them in host registers in between. CR is one 32-bit local in the architectural layout; XER holds SO, OV and CA.
- **Memory:** `rt::ld32`/`st32`… byte-swap over `g_mem + addr`. With `QILIN_CHECKED_MEMORY` (bring-up and the differential test), every access is checked against the mapped regions and an access outside them stops with its guest address and instruction.
- **Control flow:** labels on branch targets and `goto`; proven jump tables become a `switch` whose default stops (`rt::bad_jump`); direct calls are C++ calls; after each call the caller checks that the callee returned to the next instruction (`c.ret`), so a `longjmp`-style return stops instead of misbehaving; tail calls and fall-through into the next function are a call and `return`; `bctrl`, `blrl` and unresolved `bctr` dispatch through the generated sorted function table (`rt::call`), and an unknown target stops. `bl` to the next instruction just loads LR.
- **Semantics** are the 750CL manual's, with Broadway's measured results where the architecture leaves them undefined: `divw` by zero or `0x80000000 / -1` gives all ones for a negative dividend and 0 otherwise, `divwu` by zero gives 0 (as Dolphin records them). Cache and synchronisation instructions other than `dcbz` are no-ops: there are no caches or reordering to manage. Special registers other than LR, CTR and XER go through `rt::mfspr`/`mtspr`, which allow only the GQRs, HID2 and the SPRGs and stop on anything else.
- **Fail loudly:** a function is lifted whole or not at all. Floating point (slice 3), paired singles (slice 4) and privileged instructions make the lifter stop with the instruction and the reason; `qilin lift` writes a stub that stops with "not lifted" and lists the reasons in `lift_report.txt`.

On the game, `qilin lift` takes 1.5 s: **6,374 of the 8,171 functions lift** (78%). The rest: 1,722 use floating point, 19 paired singles, 56 privileged instructions (the OS and the debugger stub, which are replaced). The generated code compiles warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Werror` under GCC 14 and Clang 19, and with `/W4 /WX` under MSVC 17.14 (less C4702, unreachable code, which MSVC's optimiser reports after inlining a callee that always stops; it follows inlining decisions, so the lifter can't avoid it). Each local is its own declaration, because MSVC applies `[[maybe_unused]]` only to a declaration's first declarator.

**Incremental output:** lifted code goes in files of 256 consecutive functions, counted over every function the CFG found, lifted or not (35 files in all). File boundaries therefore depend only on the function list: lifting more functions (a new slice, a fix) changes only the files that hold them. `qilin lift` rewrites a file only when its content changes and removes chunk files that no longer exist, so an unchanged file keeps its timestamp and a rebuild recompiles only what changed. A second lift of the same input writes nothing. The banner names qilin's version, so a version bump rewrites every file once.

### Lifter, slice 3: floating point (`recomp/src/lifter.cpp` `floating`, runtime helpers in `guest.hpp`)
What the game's lifted code uses: every FP load and store (`lfs`/`lfd`/`stfs`/`stfd` with update and indexed forms, `stfiwx`), add, subtract, multiply and divide in single and double precision, the fused multiply-adds (`fmadd`, `fmsub`, `fnmadd`, `fnmsub` and their single forms), `frsp`, `fctiwz`, `fmr`, `fneg`, `fabs`, `fnabs`, `fcmpu`, `fcmpo`. FPRs are host `double` locals synced like the GPRs (`c.f[]`). **7,549 of the 8,171 functions now lift** (1,175 more). Of the rest, 560 need paired singles (534 of them stop at a `psq_st`), 56 are privileged, 5 touch FPSCR and 1 uses `frsqrte`. Two facts from the game decide the design:

- **FPSCR isn't modelled.** Every `mffs`, `mtfsf` and `mtfsb` is in `revolution/base` or the OS (replaced) or MetroTRK (dropped). Lifted code never reads or writes FPSCR, so it computes no FPSCR flags. It runs in the mode `OSInit` sets and nothing changes: round to nearest, exceptions disabled, non-IEEE mode. The runtime that replaces the OS must stop if anything asks for another mode.
- **The game runs in non-IEEE mode.** `OSInit` calls `PPCSetFpNonIEEEMode` unconditionally (FPSCR[NI] = 1). The 750 family's manual (IBM 750GX/GL user's manual, table 2-5) says that in this mode, denormal operands are treated as zero and denormal results are returned as zero, with the sign kept. Single-precision instructions judge "denormal" by the single format, double-precision ones by the double. Every arithmetic helper flushes its operands and result (`rt::ni_s`, `rt::ni_d`: a compare and a select).

Exactness:
- **NaNs** follow PowerPC's rules, not the host's: the first NaN operand in the order frA, frB, frC, made quiet; for an invalid operation, the positive default QNaN `0x7FF8000000000000` (x86's is negative). `fnmadd`/`fnmsub` leave a NaN's sign alone. A single-precision NaN keeps the top 23 fraction bits. Each helper tests its result for NaN on the unlikely path.
- **Single precision** computes in double and rounds to single (double rounding, as Broadway's single-precision instructions are taken to work, and as Unicorn computes them). The fused forms use `std::fma`, one rounding; `-ffp-contract=off` keeps the compiler from fusing anything else.
- **`lfs` and `stfs` are the architecture's bit conversions:** `stfs` copies bits without rounding (a host conversion would round) and denormalises below the single range; `lfs` keeps NaN payloads, signalling NaNs included. `fneg`, `fabs` and `fnabs` are bit operations.
- **`fctiwz`** truncates and saturates (positive overflow gives `0x7FFFFFFF`, NaN and negative overflow `0x80000000`). The high word, which the architecture leaves undefined, is `0xFFF80000`, as the 750 family returns. Whether Broadway sets something else there for a negative input that truncates to zero (as some emulators record) is open; no program reads it.

**Where no source pins Broadway's result down, lifted code stops** (`rt::fp_unknown`, a `Stop` of kind Unsupported) instead of guessing:
- a single-precision multiply (`fmuls`, `fmadds` and the like) whose frC isn't exactly a single. Broadway rounds frC to fewer bits first, by a rule with no source here; when frC is a single, as compiled code always leaves it, that rounding is a no-op;
- an FP compare of a denormal operand: the manual's rule for non-IEEE mode covers arithmetic, not compares.

Not lifted, with the reason in `lift_report.txt`: FPSCR access, record forms (CR1 from FPSCR), and `fres`/`frsqrte`, whose results are Broadway's estimate table, for which there is no clean source. Paired singles' second halves (ps1), which `lfs` and single-precision arithmetic also write in paired-single mode, come with slice 4.

### Lifter, slice 4: paired singles (`recomp/src/lifter.cpp` `paired`, runtime helpers in `guest.hpp`)
The source is IBM's **750CL RISC Microprocessor User's Manual** (preliminary v1.0, 2007): the 750CL is the commercial part closest to Broadway. It defines the paired-single register model, the GQRs (section 2.1.2.11, tables 2-19 and 2-20), quantised loads and stores ("Paired-Single Load and Store Instructions", tables 2-46 to 2-48), and every paired-single instruction (chapter 12). **8,103 of the 8,171 functions now lift** (554 more). The 68 left are privileged code (56) and FPSCR access (7), both in replaced libraries, and the estimate instructions (5: `fres`, `frsqrte`).

- **Register model:** each FPR holds a pair. ps0 is the FPR as double-precision instructions see it (`f{n}` locals, `c.f[]`); ps1 is a second value (`p{n}` locals, `c.ps1[]`). Lifted code runs in paired-single mode (HID2[PSE] = 1, set by the OS; the runtime that replaces it must keep it so). Per the manual, single-precision arithmetic, `lfs` and its forms write the result to both halves; double-precision instructions, `lfd` and `fmr` write ps0 and leave ps1 alone; `stfs` stores ps0. CodeWarrior depends on this: functions that use paired singles save each nonvolatile FPR with both `stfd` and `psq_st` and restore it with `psq_l` then `lfd`. This prologue and epilogue is what blocked 534 of the 560 functions. ps1 is undefined after `frsp` and `fsel`: `frsp` duplicates its result, as the other single-precision instructions do, and `fsel` leaves ps1 alone, as the double-precision instructions do. Compiled code can't depend on either.
- **Quantisation:** `psq_l`/`psq_st` and their update and indexed forms convert through the GQR the instruction names (SPRs 912-919, read from the context): a type (single, u8, u16, s8, s16) and a signed 6-bit scale, separately for loads and stores. A load gives integer × 2^-scale. A store gives value × 2^scale, rounded toward zero and clamped to the type's range: +∞, and NaNs with the sign clear, go to the top of the range; −∞, and NaNs with the sign set, to the bottom. A single passes through unconverted, except that a denormal stores as 0.0. With W = 1 only ps0 is transferred, and a load sets ps1 to 1.0. A reserved type (1-3) stops (`rt::fp_unknown`). Both loads of a pair complete before either half is written, so a fault leaves frD as it was.
- **Arithmetic:** each lane is the single-precision instruction of the same kind (`rt::fadds`, `rt::fmas` and so on), so non-IEEE mode, NaNs and rounding to single are slice 3's. The lane mappings of `ps_sum0/1`, `ps_muls0/1`, `ps_madds0/1` and the merges are the manual's. A pair is computed into temporaries before frD is written, since frD can also be a source. As with `fmuls`, a multiply stops if a multiplier lane it uses isn't a single. Compares (`ps_cmpu0/1`, `ps_cmpo0/1`) set the CR field from their lane and stop on a denormal.
- **Not lifted:** `ps_res` and `ps_rsqrte` (Broadway's estimate table), record forms, `ps_sel` and `dcbz_l` (none in the game's lifted code).

### Register dataflow around calls (`recomp/src/lifter.cpp`, v0.9.0)
Lifted code caches guest registers in host locals, and the context (`rt::Context`) must hold them wherever something outside the function looks: before a call, a return or a tail call, and after a call. Until 0.9.0 every such point wrote back, and reloaded, every register the function used. Now the emitter marks each sync point, and two dataflow passes over the function's instructions decide what each one moves:

- **Dirty** (forward, may): a register written since the last sync on some path. Only these are stored before a call, return or tail call; the context already holds the rest. A register counts as written only where the code can write it: an assignment target (`=`, compound assignments, `++`, `--`) or an argument of `rt::psq_l`, the one runtime helper that writes locals through references. A new helper like it must be added to `writes()`.
- **Live** (backward): read before being overwritten, where a store at a later sync counts as a read. Only these are reloaded after a call. Any other local is stale after the call, but nothing reads or stores it before it is overwritten. That second condition is why later stores count as reads: a stale local is never written back over the callee's value.

Reads and certain writes come from the emitted text, statement by statement. In `x = expr` only `expr` is read. From the first condition in an instruction's code on, everything mentioned counts as read and nothing as certainly written. Every approximation errs on the side of a store or reload too many. A conditional call or return keeps the path that skips it.

On the game, the lifted code's objects are 10.6% smaller (Clang 22, `-O2`). The differential test gives exactly the same results as before. A planted bug in the dataflow (no reloads after calls) shows up as mismatches in 24 functions of the first 414.

### Differential test (`tools/difftest/`)
`qilin_difftest` (built with `-DQILIN_GEN_DIR=…`) runs lifted functions on test cases; `difftest.py` runs the original code under Unicorn's PowerPC 750CL model from the same state and compares. Each trial gives the function random registers (a third of them pointers into a 64 KB random heap whose words point back into it a third of the time, some small integers, some near `r13`), the game's real `r2`/`r13`, a random stack, and MEM1 holding the DOL image. It compares all 32 GPRs, LR, CTR, CR, XER, where the function returned to, every changed 32-byte block of MEM1, and how it stopped: a memory fault (instruction and address), a trap, or a call into a function that isn't lifted yet (on both sides, the call is where the trial ends). Whole call trees run on both sides.

```sh
qilin lift main.dol --out gen
cmake -S . -B build-gen -DQILIN_GEN_DIR=$PWD/gen && cmake --build build-gen --target qilin_difftest
python3 tools/difftest/difftest.py --harness build-gen/qilin_difftest --dol main.dol --gen gen \
    [--trials 8] [--jobs N] [--only ADDR[,ADDR...]] [--range LO-HI] [--first N] [--seed N]
```

**Running it:** functions are shared out among `--jobs` workers (default: one per CPU), each with its own Unicorn and its own `qilin_difftest` for its lifetime. A function's trials come from a generator seeded with `--seed` and the function's address alone, so results don't depend on the number of workers, the order, or the selection: `--only` or `--range` on a mismatching function replays exactly the trials of the full run. It runs on Linux, macOS and Windows: a reader thread collects the harness's results, so a hung harness times out the same way everywhere (the harness used to need `select`), and on Windows the harness reserves the 4 GB guest space but commits only MEM1, and sets binary mode on its pipes.

Each trial touches all 24 MB of MEM1 on both sides, so the cost is in memory traffic, not emulation. Between trials both sides keep MEM1 equal to the image. A trial's writes go in, and afterwards only the pages it changed, and the written ranges, are put back. Unicorn's MEM1 is a buffer of the driver's own (`mem_map_ptr`), compared to the starting state with `memcmp` (no 24 MB read out of Unicorn), and the random heap is drawn in bulk. A trial costs about 19 ms on one core, down from 39 ms.

The entry stub (`mtxer`, then a branch to the function) is part of the starting state on **both** sides, like the heap and stack. Until 0.6.0 only Unicorn had it, so a function that read memory down to `0x80002F00` saw different bytes on each side: a false mismatch, first hit at 8 trials per function by `801E9140`, which walks an array of 12-byte records until it finds a flag bit.

Four Unicorn 2.1 problems are worked around, each found as a run of false mismatches and confirmed with a minimal test:
- Its register API stores XER without setting the SO, OV and CA flags the instructions use: XER goes in through a `mtxer` stub and comes out through `mfxer`.
- It keeps translations of code that changed: the stub's translation is dropped each trial.
- Its memory-write hook misses `dcbz`, `stmw` and `stswi`, so changes are found by comparing all of MEM1 with the trial's starting state (the harness does the same), and every changed page is restored. Before this, trials left memory behind for the next ones, which showed up as 80 mismatching functions in the OS, AX and GX.
- For a `dcbz` that faults, it reports where its internal 8-byte zeroing loop faulted; the lifted code reports the address the instruction used, as the hardware's DAR does. A fault at the same `dcbz` in the same 32-byte block counts as agreement.

**Floating point** (v0.7.0): trials also give the function random FPRs: mostly singles, as game code holds them, plus small exact values, doubles that aren't singles, infinities, NaNs with payloads (quiet and signalling) and denormals. All 32 FPRs are compared bit for bit, along with memory. Unicorn needs MSR[FP] set. A hook on each `fctiwz`, and on the instruction after it, corrects two things about Unicorn's result: QEMU 5 leaves the undefined high word 0 where the 750 family gives `0xFFF80000`, and it gives `0x80000000` for a positive overflow where the architecture says `0x7FFFFFFF`. Unicorn ignores FPSCR[NI], so the harness counts the values non-IEEE mode flushed, and a trial with a flush is a known difference. A trial where lifted code stopped on a case Broadway's result isn't pinned down for is skipped and counted. A planted bug (single-precision results not rounded to single) shows up as mismatches in 13 functions of the game's own code, as FPR and memory differences.

A top-level function that returns somewhere other than its caller (a stack smashed by random input, first seen in `SDP_AddServiceClassIdList`) now stops in the harness with a bad return, the check every lifted caller makes after a call, where it used to count as returning.

**Paired singles** (v0.8.0): Unicorn has none, so `tools/difftest/paired.py` is a reference model of them. It is written from the 750CL manual independently of the lifted code's helpers: the same specification, different code, with exact fused multiply-adds through `fractions`. Unicorn keeps ps0 (its FPRs); the model keeps ps1 and the GQRs. It emulates each paired-single instruction, and `mtspr`/`mfspr` of a GQR, in a code hook, then moves the PC past the instruction. After an instruction that writes both halves (`lfs`, single-precision arithmetic, `frsp`), it copies ps0's result into ps1. Trials give ps1 random values like the FPRs, and give the GQRs random types and scales; GQR0 is 0 in 70% of trials, as the OS sets it. ps1 is compared bit for bit, like the FPRs. A planted bug (quantised stores rounding to nearest instead of toward zero) shows up as mismatches in 65 functions.

**Hooks:** Unicorn scans its list of code hooks linearly, so a hook per instruction of interest (thousands of them) made trials 30 times slower. There is now one code hook per text section, and the driver filters the address against a set. That covers paired singles, `fctiwz`, divisions and time-base reads, and trials take 19 ms on one core, less than before.

**Time base and special registers, dynamically:** a trial is a known difference if, under Unicorn, it actually ran `mftb` or an `mfspr` of a special register the runtime models differently (anything but LR, CTR, XER and the GQRs). This replaces a static call-tree analysis that missed calls through function tables (`__init_cpp` runs the static constructors, and `__sinit_Random.cpp` seeds a global `CRandom` from `OSGetTick`) and excused whole call trees whether or not a trial reached the read.

Six differences are by design and counted, not failed:
- a cache instruction on an unmapped address faults under Unicorn and is a no-op lifted;
- an indirect call to an address that isn't a function stops lifted code (`bad_jump`) while Unicorn runs whatever is there;
- a return to an address other than the caller's stops lifted code (`bad_return`) while Unicorn jumps there;
- a denormal flushed to zero in non-IEEE mode, which Unicorn doesn't implement;
- a division by zero (or `0x80000000 / -1`), which the architecture leaves undefined: lifted code gives Broadway's result, Unicorn leaves the dividend in the target register (a code hook on each of the game's 184 `divw`/`divwu` notes when a trial divides by zero);
- a call tree that reads the time base or a special register the runtime models (HID2, the GQRs), whose values differ from Unicorn's by design (653 functions reach one, through `OSGetTick` and the like). A trial whose lifted side doesn't finish in 20 s, when the original did, is a mismatch. Trials that loop for 400,000 instructions under Unicorn, or reach a special register or system call the runtime doesn't model, are skipped and counted.

**It found one lifter bug:** `ori`, `oris`, `xori` and `xoris` have no record bit (bit 31 belongs to the immediate), but the lifter updated CR0 when the immediate was odd. Fixed, with a regression test.

**Results on the game** (8 trials per function, seed 750, v0.8.0): **8,103 functions, 64,824 trials, 0 unexplained mismatches**, in 174 s with 16 workers. 63,267 trials agree exactly: 18,984 returned, 36,414 stopped at the same memory fault, 7,869 reached the same call to a function that isn't lifted yet. 901 are known differences (810 indirect jumps to non-functions, 40 time-base or special-register reads, 24 non-IEEE flushes, 15 cache operations, 7 divisions by zero, 4 `dcbz` fault addresses, 1 smashed return), and 656 were skipped (177 Unicorn exceptions, 217 runaway loops, 211 special registers or system calls, 51 floating-point cases Broadway's result isn't pinned down for).

v0.7.0 (slices 1 and 3): **7,549 functions, 60,392 trials, 0 unexplained mismatches**, in 163 s with 16 workers. 58,989 trials agree exactly: 17,767 returned, 32,342 stopped at the same memory fault, 8,880 reached the same call to a function that isn't lifted yet. 802 are the known differences (723 indirect jumps to non-functions, 35 time-base readers, 16 cache operations, 15 non-IEEE flushes, 7 divisions by zero, 5 `dcbz` fault addresses, 1 smashed return), and 601 were skipped (187 Unicorn exceptions, 186 runaway loops, 205 special registers or system calls, 23 floating-point cases Broadway's result isn't pinned down for). The 1,175 functions slice 3 added, alone: 9,400 trials, 9,281 agreeing (1,769 returned), 0 unexplained mismatches.

Previously (v0.6.0, slice 1 only): **6,374 functions, 50,992 trials, 0 unexplained mismatches.** 49,782 trials agree exactly: 15,446 returned, 23,370 stopped at the same memory fault (instruction and address), 10,966 reached the same call to a function that isn't lifted yet. 686 are the known differences above (629 indirect jumps to non-functions, 27 time-base readers, 22 cache operations, 5 `dcbz` fault addresses, 3 divisions by zero), and 524 were skipped (166 Unicorn exceptions such as `sc`, 149 runaway loops on random input, 209 special registers or system calls the test runtime doesn't model). The full run takes 136 s with 16 workers (Windows, MSVC 17.14 harness). At v0.5.0's 4 trials per function it takes 69 s; v0.5.0 took about 45 minutes on two cores. (v0.5.0's 25,496 trials also had 0 unexplained mismatches.)

The many trials that end in a memory fault come from random pointers; better-shaped inputs (real structures from a running game) will come with the runtime.

### `qilin` CLI (`recomp/src/main.cpp`)
`info` (JSON), `insns` (every word of the text sections: address, word, text), `disasm` (a raw file of big-endian words, for the sweep), `profile` (instruction mix of the text sections, with paired-single, floating-point and privileged counts), `cfg` (summary, `--json` for every function), `coverage` (exit status 3 while any word is unknown), `suspects` and `lift` (`--out <dir>`, `--range lo-hi`, `--only addr`; writes only files whose content changed). Exit status is non-zero on any error.

## Next
1. CR bits computed only where read (a branch, `mfcr`, a sync), the rest of slice 2's register work.
2. Broadway's open FP questions, from hardware or a clean source: frC rounding in single-precision and paired-single multiplies, the `fres`/`frsqrte`/`ps_res`/`ps_rsqrte` tables, `fctiwz`'s high word for negative inputs, non-IEEE compares and `fsel`.
3. The difftest under UBSan; then the hardware-access profile and the runtime.
