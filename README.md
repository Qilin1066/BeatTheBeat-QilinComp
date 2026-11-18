# BeatTheBeat-QilinComp

Ahead-of-time recompilation of the Europe/Australia release of *Beat the Beat: Rhythm Paradise*, disc ID `SOMP01`, revision 0. The [native port](docs/NATIVE-PORT.md) builds the translated executable as C++23 and stops when it reaches a game operation without a native implementation. Its Vulkan 1.3 target matrix covers Windows, Linux, Raspberry Pi 4, Intel and Apple Silicon Macs, and iPadOS. The native startup probe is currently headless.

## Native startup probe

Build the probe from your local PAL WBFS image or extracted data partition. The build verifies the executable and translates it locally. See the [native build and validation instructions](docs/NATIVE-PORT.md).

## Earlier preview

The separate `0.1.0-alpha.1` preview uses a C++23 host, Vulkan 1.3 and a recompiled module with Wii hardware services and CPU fallback paths. It reached save-slot selection on Windows x64. See the [Windows preview build guide](docs/BUILD.md) and [validation](docs/VALIDATION.md).

This repository tracks build scripts, patches, and documentation. Your disc image, extracted files, generated code, native module, and save data live in your local build directory.

See the [changelog](CHANGELOG.md) for release history and [source provenance](docs/PROVENANCE.md) for upstream revisions and licenses.
