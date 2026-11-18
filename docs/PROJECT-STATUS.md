# Project status and decisions

## Conversation record

- The project began with a Europe/Australia WBFS dump of *Beat the Beat: Rhythm Paradise*. The requested result was a fast and accurate C++23 port using Vulkan 1.3 on 64-bit devices.
- The user clarified “i want no emulation, or on the fly translation either” and “NATIVE!” They named Windows, Linux, Raspberry Pi 4, Intel and Apple Silicon Macs, and Apple Silicon iPads, with MoltenVK on Apple platforms.
- The user identified their *Grabbed by the Ghoulies* recompilation as a reference for the same efficiency, accuracy, stability, and portability goals. They authorized a private, source-only GitHub repository and chose `BeatTheBeat-QilinComp`, with version history and documentation.
- The first Windows preview reached save-slot selection. After the native requirement was clarified, development moved to the `native-boot` branch. The previous preview remains documented under version `0.1.0-alpha.1`. [Preview validation](VALIDATION.md)

## Implementation decisions

| Area | Decision |
| --- | --- |
| Input and distribution | Verify PAL `SOMP01` revision 0 by the DOL SHA-256. Extract and generate code locally. Git tracks source, scripts, patches, and documentation. The WBFS, extracted assets, generated code, and binaries stay local. |
| Execution | Translate before launch and compile the generated code as C++23 for each target architecture. The runtime executes only precompiled host code. Unsupported operations stop with an address and diagnostic. Native startup uses compile-time lowering and scoped replacements for verified SDK calls. |
| Startup fidelity | Load game sections at their final addresses, clear uninitialized data, preserve register initialization and memory ordering, and keep the SDK’s DMA error-handler registration. The startup performance-monitor reset is specific to the verified call site. Debugger monitor access still raises a diagnostic. |
| Graphics and platforms | Vulkan 1.3 is the renderer baseline. Target system Vulkan on x86-64 and ARM64 Windows and Linux, Mesa V3DV on 64-bit Raspberry Pi 4, and MoltenVK on Intel and Apple Silicon macOS and ARM64 iPadOS. Check the required device features and measure timing on the target hardware. |

## Latest verified checkpoint

The Windows x64 `-NativeBootProbe` build from the PAL WBFS reaches `OSInit`, passes its performance-monitor reset, and stops at `PPCMthid4` at `0x800DA8AC` with instruction `0x7C73FBA6` and configuration `0x83900000`. Repeated runs reached the same diagnostic. A same-size DOL with the wrong hash was rejected, and out-of-scope monitor accesses stopped as intended. The updated Windows ARM64 loader compiled to an object file. The probe is headless. Playable startup, native audio and rendering, and device runs on the remaining platforms are pending. [Implementation and build notes](NATIVE-PORT.md) · [Verified code checkpoint](https://github.com/Tecboy1066/BeatTheBeat-QilinComp/commit/4305ba406b75647c90488d55a6bdd909b3f5f8d9)

## Next steps

1. Implement the native behavior required by the PAL HID4 configuration. Verify address mapping and paired-single stores, including values around the normal and subnormal boundary. Rerun the Windows x64 WBFS probe and record the next unsupported operation.
2. Continue through `OSInit` toward the game’s `main` by implementing the SDK and platform services it calls. Add checks for each new native service and verify startup against the PAL game.
3. Connect the native graphics, audio, input, and save services. Validate Vulkan 1.3 device feature checks, reach the title screen, and verify timing during playable sections. Measure frame pacing and input-to-audio latency on the Pi 4 as well as desktop hardware.
4. Verify startup and gameplay on the supported 64-bit platform matrix, including MoltenVK on macOS and iPadOS. Record the tested hardware, driver, and build in the repository before tagging a native release.
