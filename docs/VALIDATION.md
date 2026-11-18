# Validation

The Windows alpha used LLVM-MinGW Clang 23.1.5, CMake 4.5.0, Ninja 1.13.2, Python 3.13.9, and dtk 1.8.4. The locally built AOT module used Release mode, Clang interprocedural optimization, and module optimization level 1.

The published PowerShell script completed with the PAL WBFS as input. It extracted and validated the disc, regenerated and linked the AOT module, and staged the Windows runner. This run reused the upstream checkouts and host build cache. A build from a fresh checkout remains pending.

| Check | Observed result |
| --- | --- |
| Disc identification | `SOMP01`, disc revision 0, data partition extracted |
| Recompiler suite | 20 of 20 tests passed |
| Ahead-of-time module | 126 translated C chunks, module loaded as Windows x64, Wii entry point `0x80004050` |
| Renderer | Vulkan 1.3 selected on a GeForce GTX 1080 Ti with a Vulkan 1.4.309 loader |
| Windowed integration | A scripted 24-frame A+B press reached save-slot selection. The run recorded 151,924,355 native dispatches |
| Rebuilt module and saves | Scripted Wii Remote buttons selected a save slot, chose a name, and confirmed the save. The title wrote save data in the isolated profile and displayed the opening dialogue |
| Audio data | The rebuilt module generated 22.11 seconds of stereo 32 kHz DSP audio with nonzero samples. The host was muted during capture |

The interpreter-step counter excludes JIT and hook fallback. The smoke test establishes boot and input flow through save creation. Save reload, sustained frame pacing, audible output, physical input latency and gameplay accuracy remain open. Linux x64, Linux ARM64 and Apple Silicon have pending build and device validation.

## First gameplay milestone

Run the first tutorial to completion in the Windows build and in Dolphin with the same PAL in-game settings, input device and audio output. Compare beat-to-button timing and gameplay results. Record frame times, audio underruns, and timestamped input relative to audible cues across repeated runs. Include a save and reload, then use the same procedure for Linux x64, Raspberry Pi 5 and Apple Silicon.

The locally delivered Windows preview archive SHA-256 is `8741b26924448aa368507dfe6e45db1f55ca76722f62675b089d1c07d625df4e`.
