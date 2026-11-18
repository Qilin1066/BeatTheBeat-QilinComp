# Windows x64 build

Put Git for Windows, CMake, Ninja, x86-64 UCRT LLVM-MinGW, and Python 3 on `PATH`. For a direct WBFS build, also put [decomp-toolkit](https://github.com/encounter/decomp-toolkit/releases/tag/v1.8.4) on `PATH` as `dtk.exe`. Use a Vulkan 1.3-capable Windows driver. The tool versions used for the original preview are recorded in [validation](VALIDATION.md).

From the repository root, run one of the following in 64-bit PowerShell. Use a short build path and leave enough space for the sources, compiler output, and extracted assets.

```powershell
.\scripts\Build-Windows.ps1 -GamePath 'D:\path\to\Beat the Beat.wbfs' -BuildDirectory 'C:\work\QilinBuild'
```

```powershell
.\scripts\Build-Windows.ps1 -GamePath 'D:\path\to\extracted-pal-data' -BuildDirectory 'C:\work\QilinBuild'
```

An extracted data partition contains `sys/boot.bin`, `sys/main.dol`, and `files/`. Use this form for split WBFS dumps. The script fetches the pinned source revisions and submodules, validates the PAL executable, translates it, and builds the native module and C++23 host. It stages `Launch.cmd` in the build directory’s `run` folder. An existing build directory can be reused for incremental builds.

Run `run/Launch.cmd`. It uses the selected disc data and creates settings and saves in `run/user`. The initial mapping is **X** for Wii Remote A, **Z** for B, **Enter** for Plus, and the arrow keys for the D-pad.

## Other hosts

Linux x64 and ARM64 builds need a 64-bit toolchain and a Vulkan 1.3 hardware driver. Mesa 24.3 introduced Vulkan 1.3 exposure on Raspberry Pi 4 and 5. On Apple Silicon, the patched ModernGekko build takes `-DMODERNGEKKO_MOLTENVK_LIBRARY=/absolute/path/to/libMoltenVK.dylib`. Use a pinned arm64 or universal build of MoltenVK 1.3.0 or newer. The Windows build script targets Windows x64. [Mesa release notes](https://docs.mesa3d.org/relnotes/24.3.0.html) · [MoltenVK 1.3.0 release](https://github.com/KhronosGroup/MoltenVK/releases/tag/v1.3.0)
