# Naming

The same rule as the other Qilin recomps: **Qilin** is the author's name and goes on the *tools*, the things that do the recompiling. Anything a player sees is named after the game.

| Thing | Name | Why |
|---|---|---|
| The repo / project | BeatTheBeat-QilinComp | The game plus the toolchain that ports it |
| The recompiler CLI | `qilin` | The author's tool |
| C++ namespaces | `qilin`, `qilin::rt`, `qilin::gen` | The project's namespace |
| Build options, macros, CMake targets, environment variables | `QILIN_*`, `qilin_*` | Same prefix as the namespace |
| The window's title, SDL's app name | Beat the Beat: Rhythm Paradise | Players launch the game, not a tool |
| The game's data folder | `<per-user data>/qilin/beatthebeat` | As the other recomps keep theirs (`qilin/<game>`); saves in `nand/`, the pipeline cache |
| The settings file | `beatthebeat.ini`, in the data folder | After the game |
| The settings menu | F1 or Esc (controller: Back + Start); Controls, Display and Game pages; pauses the game; saves on close | The other recomps' menu (Dear ImGui), kept in step with them |
| Lifted functions | `fn_<address>` | By their address in the original DOL |

The game executable is still `qilin_run` while the runtime is brought up, as in the sister projects; it takes the game's name with the first player release.

The data folder moved from `<per-user data>/BeatTheBeat-QilinComp` (up to v0.16.0) to `<per-user data>/qilin/beatthebeat`: the runtime moves an old folder across on its first run.
