# Native port

The game’s PowerPC code is translated during the local build and compiled as C++23 for the target architecture. The shipped native engine must load the user’s game data directly and implement the graphics, audio, input and save services that the game uses. The native execution path treats an uncovered instruction, address or service as an error. Dolphin can supply reference captures during development.

## Target platforms

| Target | Architecture | Graphics API path |
| --- | --- | --- |
| Windows | x86-64 and ARM64 | System Vulkan driver |
| Linux | x86-64 and ARM64 | System Vulkan driver |
| Raspberry Pi 4 | ARM64 with a 64-bit OS | Mesa V3DV exposing Vulkan 1.3 |
| macOS | Intel x86-64 and Apple Silicon ARM64 | MoltenVK on a supported Metal device |
| iPadOS | ARM64 | MoltenVK XCFramework in a signed native app |

The renderer targets Vulkan 1.3 with the feature subset supported by the target devices. It will check the physical device’s API version and the features it uses before allocating resources. The Pi 4 should be used to check frame pacing and input-to-audio timing early. Mesa 24.3 introduced Vulkan 1.3 exposure for Pi 4 and Pi 5 via V3DV. MoltenVK added Vulkan 1.3 in its 1.3.0 release. [Vulkan device properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceProperties.html) · [Mesa 24.3 release notes](https://docs.mesa3d.org/relnotes/24.3.0.html#new-features) · [MoltenVK 1.3.0 release](https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.3.0) · [MoltenVK integration guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md#installing-moltenvk-in-your-vulkan-application)

The iPadOS target will link its precompiled game code into the app during the build. MoltenVK’s Metal layer handles graphics on Apple platforms. The app loads disc data supplied by the user.

## Native startup probe

The command-line probe is a Windows-tested development target. It verifies the PAL `SOMP01` revision 0 DOL checksum, loads its sections directly into memory, and runs the statically linked C++23 startup code. The target links the generated code and fixed arithmetic and state helpers. Missing native operations stop the probe with exit code 3. Graphics, audio and playable startup remain development work.

From the repository root on Windows, with Git for Windows, Ninja, CMake and the x86-64 UCRT LLVM-MinGW toolchain on `PATH`, run the following. For WBFS input, add `dtk.exe` to `PATH`.

```powershell
.\scripts\Build-Windows.ps1 -NativeBootProbe -GamePath 'D:\path\to\Beat the Beat.wbfs'
.\build\native-boot-build\qilin-native-boot.exe '.\build\generated\SOMP01_generated\main.dol'
```

The CMake target under `native/` takes `QILIN_PREPARED_BUILD`, the directory containing the locally generated `SOMP01` chunks and the pinned helper sources. The probe’s diagnostic gives the game address and operation that need native support before startup can progress.

The probe lowers `dcbf` and `dcbst` at compile time. It checks that the addressed 32-byte block is mapped. Compiled loads and stores share a single RAM backing, so the block’s contents are already in memory when the cache operation runs. The generated `sync` and `isync` operations retain their host fences. Instruction-cache invalidation is still unsupported. [PowerPC `dcbf`](https://www.ibm.com/docs/en/aix/7.2.0?topic=set-dcbf-data-cache-block-flush-instruction) · [PowerPC `dcbst`](https://www.ibm.com/docs/en/aix/7.1.0?topic=set-dcbst-data-cache-block-store-instruction)

On this revision, the SDK data initialization table has ten copies whose source and destination are identical. Its three uninitialized data ranges are already zeroed by the loader. The probe starts with the original compiled register setup at `0x80004210` and resumes after the SDK data setup call. It initializes paired-single state and the quantization registers directly, preserving the memory fence before calling the compiled floating-point register setup. At the cold-boot cache setup call, the host provides coherent memory and the compiled `OSSetErrorHandler` still registers the DMA error handler.

In `OSInit`, the SDK clears two performance-monitor control registers and four counters. For this verified PAL call site, the native probe starts with profiling inactive and resumes after those six writes. The PAL executable's other accesses to those registers belong to the debugger's register save and restore routines. Those paths still stop the probe with a diagnostic.

The Windows x86-64 WBFS build passed the performance-monitor setup and stopped on the HID4 write in `PPCMthid4` at `0x800DA8AC` (`0x7C73FBA6`) with configuration `0x83900000`. The next step is to account for that configuration in native address mapping and paired-single stores. [IBM Broadway User’s Manual, HID4](https://pokeacer.xyz/wii/pdf/BroadwayUserManual.pdf#page=65)

Focused checks covered register setup, paired-single state, DMA error-handler registration, and rejection of a counter write from a different caller or with a nonzero value. A same-size input with a different SHA-256 failed before execution. The updated Windows ARM64 loader compiled to an object file. Running the game on Windows ARM64 and the other target platforms remains open.
