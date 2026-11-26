# Extended Sound Control (formerly Claude Sound)

Standalone Sims 4 sound settings mod (game 1.128.90.1030, DX11), renamed 2026-10-17 (user). Works with or
without Sims4VR: without it, its own menu (script/extendedsoundcontrol/menu.py, game pickers) opens with a
hotkey ("." by default); with it, the VR debug menu shows the settings through `esc_get` / `esc_set`
(rows spec: docs/VR-ROWS.md; `claudesound_get/set` kept as aliases). Settings carry over from claudesound_user.ini.

- `src/sound.cpp`: the DLL (also the Python extension module `_extendedsoundcontrol`). Exe stamp gate 0x6aa08454.
- `src/audio_classes.h`: EA::Audio::Core vtables of 1.128 (generated from sims4vr/re/exe/6aa08454/classes.tsv;
  regenerate after a game patch).
- `script/extendedsoundcontrol/`: loads the DLL, menu + hotkey, console `esc.menu`, `esc.status`, `esc.probe`...
- `config/extendedsoundcontrol.ini`: settings ([sound]); changes in game go to extendedsoundcontrol_user.ini.
- Design and plan: DESIGN.md; research: FEASIBILITY.md; tests: docs/NEEDS-TESTING.md.

0.1.0 = probe only: scans the game's memory (ReadProcessMemory, so it can't crash) for audio plug-in objects
by vtable and writes their bytes, links between them and what changes over 3 s to claudesound_probe.txt.
Changes no sound, hooks nothing.

Build (Linux): `cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=tools/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build`
(mingw posix variant); script: `python3.7 -I tools/build_script.py <out>`.
Package: `extendedsoundcontrol/` with the .dll, .ts4script, .ini and empty .log + _probe.txt (+ empty
extendedsoundcontrol_user.ini on the FIRST install only, so MO2 keeps writes in the mod folder; the DLL then still
carries Claude Sound settings over) -> the user's mod folder (currently `D:\Modding\Sims 4\mods\Claude Sound Mod\`). Sources -> `D:\Modding\Sims 4\mods\Claude Sound Workspace\`.
Releases in release/<version>. Never hook a function sims4vr hooks (listener, ambience, PlaySound wrap).

Exports for other mods: `esc_quiet("music", gain)` (0.6.2, 2026-10-27, for Claude Bustin Out): holds the music
and menu-music buses at gain 0..1 on top of the user's settings until called again with 1; returns 1 when the
buses are found (named), 0 otherwise. Bustin' Out calls it via GetModuleHandle + GetProcAddress for a whole drive
so the game's map keeps its sound effects without its music.
