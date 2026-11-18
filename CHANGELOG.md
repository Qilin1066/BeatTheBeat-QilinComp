# Changelog

## 0.1.0-alpha.1 — 2026-11-18

- Added the initial build for PAL `SOMP01` revision 0 with a C++23 host and a Windows x64 ahead-of-time module.
- Required Vulkan 1.3 in the renderer and added device enumeration and extension handling for MoltenVK. Added a macOS build option for a supplied MoltenVK dynamic library.
- Verified Windows Vulkan startup, native dispatch, and the A+B transition from the title screen to save-slot selection.
- Recorded the exact source revisions, local build procedure, and the Windows gameplay validation milestone.
