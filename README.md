# BeatTheBeat-QilinComp

Experimental ahead-of-time recompilation of the Europe/Australia release of *Beat the Beat: Rhythm Paradise*. The first tested revision is `SOMP01`, disc revision 0. It uses a C++23 host, a Vulkan 1.3 graphics backend, and a native module built from the game’s Wii PowerPC executable. The recompiler emits C for that module. The host retains Wii hardware services and CPU fallback paths.

Version `0.1.0-alpha.1` reached save-slot selection on Windows x64 after a scripted Wii Remote A+B press. Linux x64, Linux ARM64 and Apple Silicon are target platforms. The source includes the MoltenVK integration for macOS. See [validation and next milestone](docs/VALIDATION.md) for the measured results and remaining coverage.

The [native port](docs/NATIVE-PORT.md) has a separate C++23 startup probe and a target matrix for Windows, Linux, Raspberry Pi 4, Intel and Apple Silicon Macs, and iPadOS.

## Build and run

Use a local PAL WBFS image or its extracted data partition. The build validates the disc ID, revision and executable hash before translating it. Follow the [Windows build guide](docs/BUILD.md). The launcher maps **X** to Wii Remote A and **Z** to B. Press **X + Z** at the title screen.

This repository tracks build scripts, patches, and documentation. Your disc image, extracted files, generated code, native module, and save data live in your local build directory.

See the [changelog](CHANGELOG.md) for release history and [source provenance](docs/PROVENANCE.md) for upstream revisions and licenses.
