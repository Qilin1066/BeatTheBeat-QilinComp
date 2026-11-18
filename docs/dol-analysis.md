# main.dol: analysis (roadmap step 1)

Last updated 2026-11-18 (v0.2.0: the tools exist; the PAL `main.dol` hasn't been run through them yet).

## Getting main.dol

The tools read `main.dol` from the game's data partition. Nothing from the game goes into git (`.gitignore` covers it). To extract it with Dolphin: right-click the game → **Properties** → **Filesystem** → right-click the data partition → **Extract System Data**. That writes `sys/main.dol` (and `sys/boot.bin`, `sys/apploader.img`, `sys/fst.bin`). **Extract Entire Disc** gives the `files/` the runtime will read later.

```sh
build/qilin info    path/to/main.dol            # SHA-256, supported build, entry point, sections, BSS (JSON)
build/qilin profile path/to/main.dol            # instruction profile of the text sections
build/qilin insns   path/to/main.dol -o main.insns.txt
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol
```

## The supported build

| | |
|---|---|
| Disc | `SOMP01`, revision 0: *Beat the Beat: Rhythm Paradise*, Europe/Australia |
| `main.dol` SHA-256 | `2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe` |
| `main.dol` size | 3,302,048 bytes |
| Entry point | `0x80004050` (`__start`; the same address as in the US build) |

The hash, size and entry point were recorded in the 0.1 phase (`prior-art.md` §1), whose build script checked the hash against the user's copy before translating it. `qilin info` will confirm them; until then, `known_build()` in `recomp/src/dol.cpp` carries the recorded hash.

## Start-up, as the 0.1 probe saw it (to re-check)

The 0.1 phase's native probe ran the start-up code of this build and recorded these PAL addresses. They are observations from that run, with the names ChatGPT gave them, not yet checked against our own analysis or against the decomp's US symbols ported to PAL.

| Address | What the probe took it to be |
|---|---|
| `0x80004050` | `__start`, the entry point |
| `0x80004210` | `__init_registers` (stack pointer, small-data bases `r2`/`r13`); `__start` resumes at `0x80004058` |
| `0x800DBC90` | `__OSPSInit`: enables paired singles (`HID2` LSQE and PSE), clears the GQRs. Its caller resumes at `0x80004374` |
| `0x800DD390` | The cold-boot cache set-up, called so as to return to `0x8000437C`; it also registers the DMA error handler `0x800DD240` with `OSSetErrorHandler` (`0x800DDF10`) for error 1 |
| `0x800DA760` | A performance-monitor register write. `OSInit` clears two control registers and four counters in one sequence from `0x800DB414` to `0x800DB43C` |
| `0x800DA8AC` | The `mtspr HID4, r3` (`0x7C73FBA6`) in `PPCMthid4`, which `OSInit` calls with `0x83900000` |

Other observations:
- `__init_data`'s ROM copy table has ten entries whose source and destination are the same (the DOL loader has already put the sections in place), and its BSS table has three ranges, which the loader leaves zeroed.
- The HID4 value `0x83900000` sets, among others, the bit that enables the second set of BAT registers (BAT4–7), which the SDK uses to map MEM2. Our runtime maps MEM1 and MEM2 flat (`roadmap-and-design-targets.md`), so the replacement for `PPCMthid4` has nothing to configure; the analysis should confirm that nothing else reads HID4 back.

## To do (needs main.dol)

1. `qilin info`: confirm the hash, the sections and BSS; record the section map here.
2. The decoder cross-check over every word of the text sections, and `qilin profile`: the instruction mix, how much is paired-single code, and every privileged instruction (expected only in the SDK's OS, which is replaced).
3. Function discovery and CFG; port the decomp's US function names and library splits to PAL addresses; record the library boundaries (start and end of each library's code) as `code-analysis.md` does for Ghoulies.
4. Hardware access: every store to the write-gather pipe (`0xCC008000`) and every other access to `0xCC000000`–`0xCDFFFFFF` or an uncached mirror from code we'll lift.
5. C++ exception tables (`extab`, `extabindex`): does the game throw?
6. The timing model: what drives the rhythm engine (frame count, audio position, or the time base) and how it handles 50 Hz and 60 Hz.
