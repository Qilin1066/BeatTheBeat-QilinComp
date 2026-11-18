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

## First native startup check

The command-line probe is a Windows-tested development target. It verifies the PAL `SOMP01` revision 0 DOL checksum, loads its sections directly into memory, and invokes the statically linked C++23 code at the game entry point. The target links the generated code and fixed arithmetic and state helpers. Missing native operations stop the probe with exit code 3. Graphics, audio and playable startup remain development work.

From the repository root on Windows, with Git for Windows, Ninja, CMake and the x86-64 UCRT LLVM-MinGW toolchain on `PATH`, run the following. For WBFS input, add `dtk.exe` to `PATH`.

```powershell
.\scripts\Build-Windows.ps1 -NativeBootProbe -GamePath 'D:\path\to\Beat the Beat.wbfs'
.\build\native-boot-build\qilin-native-boot.exe '.\build\generated\SOMP01_generated\main.dol'
```

The CMake target under `native/` takes `QILIN_PREPARED_BUILD`, the directory containing the locally generated `SOMP01` chunks and the pinned helper sources. The probe’s diagnostic gives the game address and operation that need native support before startup can progress.

The Windows x86-64 run entered at `0x80004050` and stopped at `0x800041DC`, where the translation requests the Wii `dcbf` cache operation. A same-size input with a different SHA-256 was rejected before execution. The ARM64 loader and entry chunk pass a Clang syntax check. Device testing for the rest of the platform matrix remains open.
