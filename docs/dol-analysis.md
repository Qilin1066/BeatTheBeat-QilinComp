# main.dol: analysis (roadmap step 1)

Last updated 2026-11-18 (v0.3.0: the PAL `main.dol` run through every tool: hash, sections, instruction profile, decoder cross-check, functions and coverage).

## Getting main.dol

The tools read `main.dol` from the game's data partition. Nothing from the game goes into git (`.gitignore` covers it). To extract it with Dolphin: right-click the game → **Properties** → **Filesystem** → right-click the data partition → **Extract System Data**. That writes `sys/main.dol` (and `sys/boot.bin`, `sys/apploader.img`, `sys/fst.bin`). **Extract Entire Disc** gives the `files/` the runtime will read later.

```sh
build/qilin info    path/to/main.dol            # SHA-256, supported build, entry point, sections, BSS (JSON)
build/qilin profile path/to/main.dol            # instruction profile of the text sections
build/qilin insns   path/to/main.dol -o main.insns.txt
build/qilin cfg     path/to/main.dol --json cfg.json   # functions, jump tables, summary
build/qilin coverage path/to/main.dol --list           # every word: code, padding, dead or unknown
build/qilin suspects path/to/main.dol
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol
```

## The supported build

| | |
|---|---|
| Disc | `SOMP01`, revision 0: *Beat the Beat: Rhythm Paradise*, Europe/Australia (`sys/boot.bin` starts `SOMP01`, revision byte 0) |
| `main.dol` SHA-256 | `2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe` ✅ confirmed on the user's copy (v0.3.0) |
| `main.dol` size | 3,302,048 bytes |
| Entry point | `0x80004050` (`__start`; the same address as in the US build) |

## Sections

From `qilin info`. The names are CodeWarrior's usual layout, confirmed by `__init_data`'s own tables (below): its ROM copy table lists exactly these ten sections, in this order, and its BSS table the three BSS ranges.

| Slot | Address | Size (bytes) | Section |
|---|---|---|---|
| text0 | `0x80004000` | 10,016 | `.init`: start-up code, the debugger stub's entry, `memcpy`/`memset`, `__init_data`'s tables |
| data0 | `0x80006720` | 832 | `extab` (C++ exception actions) |
| data1 | `0x80006A60` | 1,184 | `extabindex` (function → exception table) |
| text1 | `0x80006F00` | 2,041,760 | `.text` |
| data2 | `0x801F96A0` | 160 | `.ctors` (static initialisers) |
| data3 | `0x801F9740` | 32 | `.dtors` |
| data4 | `0x801F9760` | 976,096 | `.rodata` |
| data5 | `0x802E7C40` | 231,616 | `.data` |
| data6 | `0x80320500` | 4,832 | `.sdata` (small data, addressed from `r13`) |
| BSS | `0x803217E0` | 3,716 | `.sbss` |
| data7 | `0x80322680` | 35,264 | `.sdata2` (small constants, addressed from `r2`) |
| BSS | `0x8032B040` | 52 | `.sbss2` |
| BSS | `0x8032B080` | 705,544 | `.bss`, up to `0x803D7488` |

The header's BSS range is `0x803217E0`, 744,616 bytes: it spans `.sbss`, `.sdata2`, `.sbss2` and `.bss`, and the loader keeps `.sdata2`'s bytes inside it.

## Instructions

`qilin profile`: 512,944 words in the text sections, 162 distinct instructions. The most common are `addi` (17.1%), `lwz` (12.0%), `b` (10.7%), `bc` (7.2%) and `stw` (7.2%). 53,489 floating-point instructions, of which 3,766 are paired singles (`psq_l`/`psq_st` 3,533, the rest arithmetic: `ps_madd`, `ps_madds0/1` …): far fewer than in a 3D game, as you'd expect from a 2D rhythm game. 153 privileged instructions (`mfmsr` 56, `rfi` 39, `mtmsr` 23, …), all in the SDK's OS and the debugger stub, which are replaced. `mftb` appears 6 times, `sc` 3 times.

**Decoder cross-check** (`tools/check/verify_decoder.py --dol`): all 512,944 words agree with GNU objdump; 505,952 valid and identical, 6,935 invalid for both, and the only known-difference classes are `mfcr`'s printed field (51) and `mftb` (6). Of the 6,935 invalid words, 6,916 are zero padding; the other 19 are data in `.init` (15) and four `lmw r0,0(r3)`, an invalid form the debugger stub's hand-written register restore uses. The debugger stub isn't lifted.

## Functions and coverage

`qilin cfg` (1.2 s): **8,301 functions** (1 entry, 4,906 call targets, 231 reached only by tail calls, 681 only through `lis`/`addi` pointers, 2,127 only through pointers in data such as vtables, 355 found in gaps), 505,266 reachable instructions, **157 jump tables, all proven** by their bound check, 2,560 indirect calls, 219 indirect tail calls (calls through a vtable or callback that end the function, and the runtime's pointer-to-member call), 816 tail calls, and 29 functions that fall into the next one (the runtime's register save/restore chains). The decomp names 8,103 functions in the US build; the two counts will be compared when its names are ported.

**One suspect:** the debugger stub's register restore (`0x80116064`) reaches the `lmw` invalid form. **Five starts rejected** by validation, each data or a template: `__init_data`'s two tables, the SDK's `bla 0x60` template, a `nop` before the OS's context code, and the debugger stub's exception entry.

`qilin coverage`, by word:

| Section | Code | Padding | Dead | Unknown |
|---|---|---|---|---|
| `.init` (2,504 words) | 816 | 1,640 | 0 | 48 |
| `.text` (510,440 words) | 504,450 (98.83%) | 5,377 | 559 | 54 |

Padding is zero words and `nop`s that nothing reaches. Dead code is 559 words that the compiler left unreachable inside functions (typically a `b` right after another unconditional branch, or a switch case nothing selects); nothing branches to them and no pointer in data points into them. **The 102 unknown words are all identified, and none of them is code we lift:**

| Where | Words | What |
|---|---|---|
| `0x80004744` | 12 | The string "Metrowerks Target Resident Kernel for PowerPC" (the debugger stub) |
| `0x80006678` | 30 | `__init_data`'s ROM copy table: ten (source, destination, size) entries, source = destination |
| `0x800066FC` | 6 | `__init_data`'s BSS table: three (address, size) ranges |
| `0x800DBB50` | 1 | `bla 0x60`: an SDK template copied to low memory, not run in place |
| `0x80115FCC`, `0x801160BC` | 53 | The debugger stub's exception entry and register restore (`stmw r0`, `lmw r0`) |

## Start-up, as the 0.1 probe saw it (to re-check)

The 0.1 phase's native probe ran the start-up code of this build and recorded these PAL addresses. They are observations from that run, with the names ChatGPT gave them, not yet checked against the decomp's US symbols ported to PAL.

| Address | What the probe took it to be |
|---|---|
| `0x80004050` | `__start`, the entry point |
| `0x80004210` | `__init_registers` (stack pointer, small-data bases `r2`/`r13`); `__start` resumes at `0x80004058` |
| `0x800DBC90` | `__OSPSInit`: enables paired singles (`HID2` LSQE and PSE), clears the GQRs. Its caller resumes at `0x80004374` |
| `0x800DD390` | The cold-boot cache set-up, called so as to return to `0x8000437C`; it also registers the DMA error handler `0x800DD240` with `OSSetErrorHandler` (`0x800DDF10`) for error 1 |
| `0x800DA760` | A performance-monitor register write. `OSInit` clears two control registers and four counters in one sequence from `0x800DB414` to `0x800DB43C` |
| `0x800DA8AC` | The `mtspr HID4, r3` (`0x7C73FBA6`) in `PPCMthid4`, which `OSInit` calls with `0x83900000` |

Other observations:
- ✅ Confirmed (v0.3.0): `__init_data`'s ROM copy table has ten entries whose source and destination are the same (the DOL loader has already put the sections in place), and its BSS table has three ranges, which the loader leaves zeroed.
- The HID4 value `0x83900000` sets, among others, the bit that enables the second set of BAT registers (BAT4–7), which the SDK uses to map MEM2. Our runtime maps MEM1 and MEM2 flat (`roadmap-and-design-targets.md`), so the replacement for `PPCMthid4` has nothing to configure; the analysis should confirm that nothing else reads HID4 back.

## To do

1. Port the decomp's US function names and library splits to PAL addresses (masked-hash signatures), compare the function lists, and record the library boundaries (start and end of each library's code) as `code-analysis.md` does for Ghoulies.
2. Hardware access: every store to the write-gather pipe (`0xCC008000`) and every other access to `0xCC000000`–`0xCDFFFFFF` or an uncached mirror from code we'll lift.
3. C++ exception tables (`extab`, 832 bytes, and `extabindex`, 1,184 bytes): which functions have them, and does the game throw?
4. The timing model: what drives the rhythm engine (frame count, audio position, or the time base) and how it handles 50 Hz and 60 Hz.
