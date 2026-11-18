# Prior art: Wii and PowerPC recompilation, and this game (researched 2026-11-18)

What exists, what each project chose, and what it means for this one (C++23, Vulkan 1.3, cross-platform including low-end ARM boards, efficiency and reliability first, the same targets as GBTG-Qilincomp).

## 1. This project's 0.1 phase (ChatGPT, 2026-11-18)
The repository started with work by ChatGPT, in two parts. Both are retired; the history is in git (`v0.1.0-alpha.1` is commit `751b042`, the native probe ends at `b1c602c`).
- **0.1.0-alpha.1, a Windows preview:** a PowerShell script fetched DolRecomp, ModernGekko and RecompCore (§2) at pinned commits, patched them to require Vulkan 1.3 and to package MoltenVK, translated `main.dol` to 126 C chunks and built ModernGekko's runner around them. Its notes report save-slot selection and a new save on Windows x64 (GeForce GTX 1080 Ti), with 22 s of DSP audio captured. Graphics, audio and the rest of the hardware ran on Dolphin's code, and the CPU could fall back to an interpreter, so this was emulation with recompiled CPU code.
- **The native boot probe (`native-boot` branch):** a 200-line headless C++23 program that linked DolRecomp's generated C into an executable without the interpreter, loaded the DOL itself and stopped at the first operation it couldn't run natively. It got through the register set-up, `__init_data`, the cache set-up and `OSInit`'s performance-monitor reset (by skipping verified call sites), and stopped on the HID4 write in `PPCMthid4`.
- **Why it was retired:** generated C instead of C++, a context-struct register model (`cpu->gpr[n]`), an interpreter fallback path in the generated code, and a Dolphin-based runtime are each the opposite of a target in `roadmap-and-design-targets.md`.
- **What carried over:** the PAL build's identity (`SOMP01` revision 0 and the `main.dol` SHA-256), and the start-up observations, recorded in `dol-analysis.md` to be checked again with our own tools.

## 2. DolRecomp, ModernGekko, RecompCore (ExpansionPak)
- **DolRecomp:** a static recompiler for GameCube and Wii DOLs and RELs (and early Wii U), with C and LLVM back ends. It handles the CPU side only; the README leaves graphics, audio, input and the platform to the host.
- **ModernGekko:** "a runtime for GameCube/Wii recomps", crediting the Dolphin team as its foundation, with a code-mod ABI. **RecompCore** is the Dolphin-derived runtime underneath it (per-file GPL-2.0-or-later). Games in its hall of fame include Super Mario Sunshine, Brawl, Melee, Luigi's Mansion and Kirby Wii.

**Takeaways:** proof that CodeWarrior-compiled Wii code recompiles statically. The model (C output, guest registers in a context struct, an interpreter for whatever the translator doesn't cover, Dolphin's hardware emulation as the runtime) is what we're avoiding, for the same reasons Ghoulies avoided xboxrecomp's.

## 3. Rhythm Heaven Fever decompilation (`conhlee/rhf`) — the same game, US build
- A work-in-progress matching decompilation of the US release (`SOME01`, `main.dol` SHA-1 `d96d2c7e50b0c713cb8fcb18835258351ada5576`), built with decomp-toolkit and the original CodeWarrior compilers. **Licence: CC0 1.0.** Checked at commit `02b140c9e7007e2a4ddd61c80ac3f2eb35251ed7`.
- `config/SOME01/symbols.txt` has 22,221 symbols, **8,103 of them functions**, with sizes; `splits.txt` assigns code to translation units. The libraries, by number of units: Revolution SDK (`bte` 68, `hbm` 41, `os` 34, `gx` 14, `ax` 10, `dvd` 8, `wpad` 7, `nand` 6, `mem` 5, `mtx` 4, `ipc` 4, …), NW4R (`snd` 62, `ut` 18, `lyt` 16, `g3d` 5), MSL 53, MetroTRK 25, the EABI runtime 11, and the game.
- US addresses seen so far: `__start` at `0x80004050` (the PAL entry point too), `OSInit` at `0x800D9AB0`, `PPCMthid4` at `0x800D8FA0`.
- An older disassembly of the US build (`EstexNT/rhf`) and the RHModding community's documentation of Fever's tickflow bytecode (the scripts that drive each rhythm game) also exist.

**Takeaways:** the best naming source we could have, and CC0, so it can be vendored like Ghoulies vendors nxdk's kernel export table. The PAL build is a different binary, so names are ported by aligning function sizes (§ *Decisions*). The library split confirms the boundary in `roadmap-and-design-targets.md`: the Bluetooth stack (`bte`), the Home Button Menu (`hbm`) and MetroTRK (the debugger stub) are large and are replaced or dropped, not lifted.

## 4. CastawayRecomp and BlackKnightRecomp (yunophilia) — Wii, static, C++
- *The Sims 2: Castaway* statically recompiled to C++ for Windows x64 and Linux x64; the recompiler and most of the runtime come from the same author's BlackKnightRecomp. Function names and starts come from a linker ELF the disc happens to ship.
- The recompiler decodes every Gekko/Broadway instruction, paired singles and quantised loads and stores included, and emits one C++ function per guest function.
- **The game's own Revolution OS runs recompiled; guest threads are host fibers.** VI, PI, CP/PE, DSP/AI, EXI, SI and IPC are emulated at a high level, IOS's `/dev/di` and `/dev/fs` are served from the installed disc files and a `nand/` folder. GX renders through Direct3D 11 (DXVK on Linux); audio goes through an AX HLE to SDL3. GPL-2.0-only, with Dolphin-derived runtime code.
- Testing: an end-to-end TAS script that plays from first boot, saves, quits and reloads, checking game memory at each step; headless on Linux.

**Takeaways:** a working Wii static recompilation with the same CPU decisions as ours. Its system boundary is lower than ours (the SDK runs recompiled on emulated hardware registers); ours replaces the SDK's hardware-facing libraries, as Ghoulies replaces the kernel and D3D, so no hardware register emulation is needed beyond the write-gather pipe. Fibers for guest threads are a data point for the scheduler decision. The TAS-style end-to-end test is worth copying at step 5.

## 5. NWiiRecomp (BlackLineInteractive) and Mario Kart Wiicompiled (patchzy)
- **NWiiRecomp:** a static recompiler and runtime for GameCube and Wii, PowerPC → C++, stating no instruction-level emulation and no Dolphin code; it implements the time base and decrementer, and paired singles with GQR-based quantisation.
- **Mario Kart Wiicompiled:** announced in July 2026 as the first static recompilation of a Wii game, in public beta since August, with Windows and x86-64 builds and, since September, an aarch64 one. It was built with AI coding tools, which drew criticism.

**Takeaways:** Wii static recompilation is now practical and done more than one way; none of these targets Vulkan 1.3 with an ARM board as the low-end reference.

## 6. XenonRecomp and ReXGlue (Xbox 360) — the most mature PowerPC → C++ lifters
- **XenonRecomp** (hedge-dev; Unleashed Recompiled) converts Xbox 360 executables into C++ that compiles for any platform; jump tables and other details come from configuration. Exceptions aren't supported. **ReXGlue** builds on it with a Xenia-based runtime and C++23 output, tracks which registers it can keep as locals, models CR fields for branches, and implements the MSR's interrupt-enable bit as a global lock.

**Takeaways:** a different CPU (64-bit, VMX, no paired singles) but the same instruction-set family; their CR handling and register-as-local tracking are the nearest prior art for our lifter's register model.

## 7. Dolphin
- The reference emulator, GPL-2.0-or-later. Its CPU interpreter documents Broadway's corner cases from hardware tests (paired-single rounding, the estimate tables, quantisation), and its GX and AX code define what correct output looks like.

**Takeaways:** ground truth for behaviour; we don't copy its code. Where we need a hardware fact that only Dolphin's source records (the `fres`/`frsqrte` tables, for instance), we derive it from hardware documentation or measurement and check it against Dolphin's results, and note the source here.

## 8. GNU binutils' `broadway` disassembler
- `objdump -M broadway,raw` decodes the 750CL's instructions with the paired singles, without simplified mnemonics. It is our decoder's cross-check (`tools/check/verify_decoder.py`). It differs from Broadway in reviewed ways: it leaves out `mftb`, accepts `fsqrt`, `fsqrts`, `tlbia`, `tlbld` and `tlbli` (not on the 750CL), accepts later ISAs' hint fields that are reserved on Broadway, and doesn't check several invalid forms. The full list is in the script.

## Decisions changed or added
1. **Decoder:** our own (no Zydis-like library exists for Broadway), cross-checked against binutils on every word, in CI over a sweep of the encoding space.
2. **Names:** the CC0 Rhythm Heaven Fever decomp, vendored (`third_party/rhf`) and ported from US to PAL by aligning function size sequences (v0.4.0; without the US executable there are no US bytes to sign).
3. **System boundary:** replace the SDK's hardware-facing libraries at their API (no hardware register emulation beyond the write-gather pipe); lift the C/C++ runtime, the pure-software SDK libraries and NW4R. Confirm per library in analysis.
4. **References:** Dolphin for behaviour and timing; the decomp for meaning and names; CastawayRecomp's TAS test as the model for our end-to-end test.
