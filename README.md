[![Support me on Ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/masterroshihehe)

# Extended Sound Control

A standalone sound mod for The Sims 4 (PC). It goes past the game's own volume sliders: boost
the master volume and each sound category beyond 100 %, bring up ambient sound that has no
slider in the game, and change how sound is muffled and how it fades with distance.

> **This mod is 100% vibe coded.** Every line of code was written by Claude (Anthropic's AI).
> I ([MasterRoshiHehe](https://ko-fi.com/masterroshihehe)) act as the director: I decide what
> the mod should do, test every build and report what works and what doesn't.
> I don't write the code myself. Expect bugs, and read the code before you trust it.

**Status: early development.** Things change and break often.

## Features

- Master boost above 100 % that still goes through the game's own limiter.
- Separate boosts for music, sound effects, voices, UI and menu music.
- Ambient sound level (the game has no slider for it).
- Clarity: how much walls and distance muffle sound.
- Works on its own. With [Sims4VR](https://github.com/MasterRoshiHehe/Sims4VR) installed, its
  settings also show up in the VR debug menu.

## Settings

Open the settings menu in game with the hotkey (default: `.` on the keyboard, changeable in the
menu). The first page has the everyday settings; the rest is under **Advanced**.
All settings are also in the mod's `.ini` file.

## Requirements

- The Sims 4 on PC. Supported game version: 1.128.90.1030.
- Script mods enabled in the game (Options > Game Options > Other).

## Install

1. There is no release yet: the mod is in early development. When there is one, download the zip from the [Releases](../../releases) page. Until then, build it from source (below).
2. Unzip it into `Documents/Electronic Arts/The Sims 4/Mods/`.

## Building from source

Cross-compiled on Linux with MinGW-w64 and CMake; the script part needs Python 3.7 for its `.pyc` files.

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=tools/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## License

MIT, see [LICENSE](LICENSE). Third-party libraries keep their own licenses.

The Sims 4 is a trademark of Electronic Arts Inc. This is a fan-made mod and is not affiliated
with or endorsed by Electronic Arts.
