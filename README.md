# Witcher 2 Engine Fix

Seamless interiors, draw-distance, save-preview, interior-map and quality-of-life fixes for **The Witcher 2: Assassins of Kings - Enhanced Edition**. A small `dinput8.dll` that the game loads by itself and that adjusts specific engine values and bugs in memory while the game runs. `witcher2.exe`, the game's data files and your saves are never modified.

Sibling of [Witcher Engine Fix](https://github.com/MariusTBjercke/witcher-enginefix) for The Witcher 1, built the same way.

## What it does

| | What you notice | Default |
|---|---|---|
| **Missing save previews** | New saves showed a purple/blue gradient instead of a screenshot in the Load menu. The game asks for the screenshot and saves in the same frame, and the finished image arrives about a frame too late, so the game drops it. The mod writes that image next to the save under the name the game expects, and makes the menu reload a QuickSave's picture right away instead of after a restart. Old saves cannot get their picture back. | on |
| **Save list memory leak** | Each time the Load/Save menu built its list, the game read every save's preview again and leaked it when the menu already had that picture: ~0.9 MB per save per menu open, kept until you quit. With many saves that adds up to hundreds of MB and is a likely cause of crashes once saves pile up. The mod frees it. Nothing is deleted and saves are not limited. | on |
| **Graphics presets** | One hotkey, **Ctrl+F10**, cycles baseline / high / ultra / extreme. Each level sets mesh LOD distance, foliage distance, the hard mesh visibility cutoff and the mesh-shadow distance together, with an on-screen readout. Starts at `extreme`. The same levels are a **Draw distance** row in Options > Graphics (no on-screen text there), which the game remembers between sessions. | on (extreme) |
| **LOD values that stick** | The game's `MeshDistanceScale`/`FoliageDistanceScale` are held at the preset values, also after the options menu rewrites them, and the two "gameplay LOD downscale" flags are kept off. | on |
| **Black map in interiors** | In 25 interior map entries (temples, inns, Loredo's house, towers and other houses) the map screen (M) was black with one huge pin, because the game has no big map for them. The zip ships loose files that give each of these areas a 2048x2048 upscale of its own minimap, so the map shows and zooms the right way. | on (files) |
| **Seamless interiors** | Walk into houses, shops and inns without a fade. The game unloaded the outdoors around you at every door and loaded it again on the way out, behind ~2.2 s of black. Now the outdoors stays loaded (visible through the windows), the area's interiors load with the area so open doors show the real rooms, and a house door takes a few hundredths of a second. Doors that change area (the Flotsam gate) keep a short fade. `[loading] interiors` (2 preload, 1 load on first visit, 0 the game's doors), `door_fade`, `area_fade`. | on |
| **Faster save list** | To show the Load/Save list (and at game start) the game unpacked every save in full to read a few header values. The mod unpacks only the parts the list reads, in memory: about 2.5x faster with 120 saves, same list. Loading a save is unchanged. `[saves] fast_list`. | on |
| **Crash guard** | A physics ray cast that meets an object that was just unloaded (seen once when walking out of the Flotsam inn) misses it instead of closing the game. `[stability] raycast_guard`. | on |
| **Clean quit** | Quit in the pause menu (or any quit with an area loaded) crashed the game while it shut down: a script recorded a quest fact after the world was gone. Nobody saw it, but each quit left a crash dump. The mod gives that fact time 0 instead. `[stability] quit_guard`. | on |
| **32-thread CPUs** | On a CPU with 32 or more logical processors the game never showed a window: its own CPU count never ended. The mod skips that count, the same as taking a core away in Task Manager, but keeps the core. `[stability] cpu32_guard`. | on |
| **Crash capture** | The game's own crash handler closes the game without a word. The mod logs what crashed and writes a small dump next to the log, so a crash can be reported and analysed. `[diagnostics] crash_dump`. | on |
| **Skip the startup videos** | `[startup] skip_intro=1` starts at the main menu without the logo videos and the intro movie. The mod presses "skip" for the game on each startup movie (the same path as Esc), so a short black moment remains. Works with or without an intro removal mod. | off |
| **Ctrl+F9/F10 do not quick-load** | F9 (quick load) and F10 cannot be rebound in this game. While Ctrl or Alt is held, the mod hides F9/F10 from the game, so the mod's hotkeys never trigger a quick load. Plain F9 still quick-loads. | on |
| **Snipping Tool and Windows keys** | The game blocks every Windows key combination while it is in front, so Win+Shift+S and Print Screen (Snipping Tool) did nothing. Now they work, as do Win+D and the rest; a Windows key tapped on its own still does not open the Start menu. `[hotkeys] windows_keys`. | on |
| **Book read markers** | Books show `Read`, `Nothing new` or `Unread` in the inventory, trade and storage lists, with a tooltip describing what reading them would still teach you. A tick marks books you have read; at merchants and in storage it also keeps the game's carried-copy meaning. `Nothing new` is text only, since the book itself is still unread. Reading a book refreshes the inventory immediately. Looted books show the read tick too; the loot window shows `(Nothing new)` after the name where applicable. In 12 languages. | on |
| **Continue** | The main menu gets a Continue button (the game's own translated label) that loads your newest save. | on |
| **Quit in the pause menu** | The pause menu gets the game's Quit button, with a confirmation, next to "Exit to main menu". | on |
| **Medallion on the move** | The medallion key works at once: Geralt keeps walking, running or fighting instead of stopping for the animation, and the cooldown is 1 second instead of 2. Same pulse, sound and highlights. `[scriptpatches] medallion`. | on |
| **Display mode in game** | Options > Graphics gets a **Display mode** row: full screen, borderless window or windowed, switched at once without a restart. Borderless fills the screen at the desktop resolution without a border or title bar, while Direct3D stays windowed, so alt-tab does not recreate the device; the process is made DPI-aware so the picture stays 1:1 on a scaled desktop. | on (borderless off) |

Each DLL part can be switched off in `witcher2_enginefix.ini`; the interior maps are removed by deleting their files (see below).

### Script patches without touching the game files

The last four change the game's scripts and UI screens. The DLL does that at game start: the first start builds patched copies of the files your game uses (your `base_scripts.dzip`, also when a mod like Project Mersey replaced it; the trade/storage screens, also when an HD UI mod replaced them; the read-book icons) into `bin\witcher2_enginefix_cache\`, and the game reads those instead of the originals. Later starts reuse them until a game file changes. A screen another UI mod changed in a way the patch does not recognize is left alone, and a part that cannot be applied is skipped and logged rather than half-applied.

**Upgrading from 0.4/0.5 with the old script patches installed:** extract the new version over the old one. The first start puts the original files back once from `Witcher2EngineFix_Optional\ScriptPatches\backup` (only files still exactly as the old patcher left them) and keeps that folder as `backup.restored-by-dll`. After that start the `Witcher2EngineFix_Optional` folder can be deleted.

Details: [dll-script-patches.md](docs/fixes/dll-script-patches.md), [shop-book-read-marker.md](docs/fixes/shop-book-read-marker.md), [menu-buttons.md](docs/fixes/menu-buttons.md), [borderless-window.md](docs/fixes/borderless-window.md). Details, addresses and how each part was verified are in [`docs/fixes/`](docs/fixes/).

### Preset values

| Preset | Mesh LOD | Foliage | Mesh visibility | Mesh shadow |
|---|---:|---:|---:|---:|
| baseline (the game's own) | 1 | 1 | 1 | 1 |
| high | 2 | 2 | 1.5 | 1.5 |
| ultra | 4 | 3 | 2 | 2 |
| extreme | 8 | 5 | 3 | 3 |

The higher levels cost GPU time. They do not change how far away the game spawns characters (that is a separate system, see [docs/fixes/npc-streaming-sun-shadows.md](docs/fixes/npc-streaming-sun-shadows.md)).

## Requirements

- The Witcher 2: Assassins of Kings - Enhanced Edition, **3.5.0.26 on Steam**. Tested on Windows 11, with both DXVK and the game's native Direct3D 9.
- **Steam only for now.** The GOG version (3.5.0.26g) is a separate build from 2023: its code was recompiled, so the engine fixes do not find what they look for and skip themselves, and the save fixes need Steam storage. GOG support is being worked on for a later version; see [docs/fixes/gog-build.md](docs/fixes/gog-build.md).
- The save-preview fix works when saves go through Steam storage (the default for the Steam version). The engine hooks check the exact game code before they patch anything; on another build they skip themselves and say so in the log.

## Installation

1. Download `Witcher2EngineFix-<version>.zip` from [Nexus Mods](https://www.nexusmods.com/witcher2/mods/1200) or [GitHub Releases](https://github.com/MariusTBjercke/witcher2-enginefix/releases).
2. Extract it into the game folder (the one that holds `bin\` and `CookedPC\`), so that `dinput8.dll` ends up next to `witcher2.exe` in `bin\`, for example
   `...\steamapps\common\the witcher 2\bin\dinput8.dll`
3. Start the game as usual (through Steam and the launcher).

To check that it works, open `bin\witcher2_enginefix.log` after starting the game. It should begin with `witcher2_enginefix x.y.z loaded` (the version you installed) and later show `distance: hooks installed` and `saves: ISteamRemoteStorage ... FileWrite hooked; thumbnail fix=1`. After a new save, the log says `thumbnail: wrote ..._640x360.bmp`.

| File | Purpose |
|---|---|
| `bin\dinput8.dll` | The engine fix. The game loads it automatically and it forwards everything else to Windows' own `dinput8.dll` |
| `bin\witcher2_enginefix.ini` | Settings |
| `CookedPC\globals\map_scales.csv` | The game's map table with the missing interior maps filled in |
| `CookedPC\globals\gui\maps\*_2048x2048.dds` | 25 upscaled interior maps (new names, nothing of the game's is replaced) |
| `bin\witcher2_enginefix_cache\` | Created at the first start: the patched copies the game reads for the script patches |

**Uninstall:** delete `bin\dinput8.dll`, `bin\witcher2_enginefix.ini`, the folder `bin\witcher2_enginefix_cache\`, `CookedPC\globals\map_scales.csv` and the `*_2048x2048.dds` files the zip put in `CookedPC\globals\gui\maps\` (the game's own maps live inside `pack0.dzip`, not in that folder).

## Configuration

`bin\witcher2_enginefix.ini` is read at startup and never written by the mod. The most useful keys:

| Section / key | Default | Meaning |
|---|---|---|
| `[general] enabled` | 1 | 0 turns off everything the DLL does (input still forwarded); the map files are removed separately |
| `[general] log` | 1 | 0 stops writing `witcher2_enginefix.log` |
| `[presets] default` | extreme | Startup level: baseline, high, ultra or extreme |
| `[presets] cycle` | Ctrl+F10 | Hotkey that cycles the levels (F9/F10 with Ctrl or Alt) |
| `[preset.<name>]` | see table | Your own values per level |
| `[saves] fix_thumbnails` | 1 | 0 turns the save-preview fix off |
| `[saves] fix_list_leak` | 1 | 0 turns the save-list leak fix off |
| `[saves] fast_list` | 1 | 0 lets the game unpack every save in full for the Load/Save list |
| `[loading] interiors` | 2 | 1 loads each interior on its first visit (short fade then), 0 restores the game's doors |
| `[loading] fast_doors`, `door_fade`, `area_fade` | 1, 0.05, 0.25 | fade lengths for house doors and area changes; `fast_doors=0` and 0.5 restore the game's timing |
| `[loading] door_light_blend` | 1.0 | seconds the lighting takes to change at a house door without a fade; 0 switches at once as the game does |
| `[stability] raycast_guard` | 1 | 0 turns the ray-cast crash guard off |
| `[stability] cpu32_guard` | 1 | 0 lets the game run its own CPU count (hangs on 32 or more logical processors) |
| `[stability] quit_guard` | 1 | 0 lets the shutdown after Quit crash again (it only leaves a dump) |
| `[startup] skip_intro` | 0 | 1 starts at the main menu, without logo videos and intro |
| `[window] borderless` | 0 | 1 starts borderless (Full screen off, desktop resolution). After the first use, Options > Graphics owns the setting; it is stored in the game's User.ini |
| `[window] enabled` | 1 | 0 turns the window patches, the live switch and the menu channel off |
| `[window] dpi_aware` | 1 | Make the process DPI-aware when borderless starts on |
| `[scriptpatches] book_marker`, `continue`, `pause_quit`, `window_mode`, `medallion` | 1, 1, 1, 1, 1 | 0 turns that script patch off (the cache is rebuilt); `window_mode` is the Display mode and Draw distance rows |
| `[osd] enabled`, `size`, `ms` | 1, 34, 2500 | On-screen readout; 34 px suits 4K, 22 px 1080p |
| `[diagnostics] save_trace` | 0 | 1 logs the save-preview path (Steam writes, screenshot encoding) |
| `[diagnostics] savelist_trace` | 0 | 1 logs each save-list build (time, Steam reads, memory) |
| `[diagnostics] load_trace` | 0 | 1 measures every loading screen and house door |
| `[diagnostics] crash_dump` | 1 | 1 logs a crash and writes a minidump, 2 a full dump, 0 off |
| `[diagnostics] exit_trace` | 1 | 1 logs which code ended the game (one line at exit), 0 off |

The ini file itself documents every key, including the legacy separate hotkeys (`[presets] enabled=0`).

## Compatibility

The pause-menu Quit confirmation currently needs one click before it
responds to hover. The button still works; this is a known minor issue.

- **DXVK, strongly recommended at 4K**: the game is a 32-bit program, so it has 4 GB of address space at most. With the game's own Direct3D 9, every texture also takes room there. At 4K on the launcher's highest settings, loading a save inside the Flotsam inn took 1.7 GB at the main menu and 2.6 GB after the load, and the game ran out of memory within a minute. That happened even though this mod loaded nothing extra (its preload stops below 1280 MB free), and whatever the texture budget or draw distance. With DXVK, which keeps textures on the graphics card, the same load stays around 1.3 GB. Lower resolutions leave more room (not measured yet). One owner setup (DXVK 3.1.1, RTX 5070 Ti) hit a GPU reset (Vulkan device lost) in the same place, with or without this mod; if the game hangs that way, set `dxvk.enableDescriptorHeap = False` in `dxvk.conf` (DXVK then uses its descriptor-buffer binding; one run of about 8 minutes with 8 doors at the inn had no GPU reset, where every run before hung within minutes) or try another DXVK version. See [docs/fixes/door-transitions.md](docs/fixes/door-transitions.md), "Memory". The on-screen readout is a small topmost window, so it shows in borderless and DXVK full screen but not in true exclusive full screen.
- **Borderless window**: only in windowed mode (Full screen off) and best at the desktop resolution; at a lower resolution it is a centered frameless window at that size, and it always sits on the primary monitor. Windowed mode may lose the device gamma ramp (Brightness/Gamma), and the game may behave differently in the background than in full screen; both are properties of the game's windowed mode, not of the patch.
- **Other `dinput8.dll` mods**: only one `dinput8.dll` can sit in `bin\`. This mod loads the system `dinput8.dll` itself.
- **Script mods**: a mod that replaces `base_scripts.dzip` is fine; the patches go into whatever copy is there. Mods installed as user content (`Documents\Witcher 2\UserContent`, listed in `Config\UserContent.ini`) can carry their own scripts, which the game uses instead of `base_scripts.dzip`'s; the patches then go into those copies. If a mod's copy of a file is too different for a patch, that one feature stays off and the game starts without it (`witcher2_enginefix.log` says which).
- **Other map mods**: a mod that ships its own `CookedPC\globals\map_scales.csv` conflicts with the interior maps; whichever was copied last wins. UI mods that replace `ui_nav.swf` (A Better UI, HD UI upscales) are fine.
- **Steam Cloud**: no effect on the fix; saves and previews are written through the game's own Steam storage interface.
- **Antivirus**: some scanners flag the release as suspicious. This is a false positive: `dinput8.dll` is an unsigned proxy DLL that reads hotkey state and patches values in the game's memory, which is what heuristics look for. Since 0.6.0 the release holds no `.bat` or PowerShell script (0.5.x shipped a PowerShell patcher, which most of the warnings were about). Nothing is downloaded or sent anywhere; build the DLL yourself from this source if you prefer (see below).

## Building from source

Needs Visual Studio 2022 or newer with the **Desktop development with C++** workload (x86 tools). Nothing else; the one third-party piece, [tinf](https://github.com/jibsen/tinf) 1.2.1 (zlib license), is vendored in `src/third_party/tinf`.

```bat
build.bat
```

This produces `build\dinput8.dll`. If Visual Studio is not in the default location, set `VCVARS` to the full path of `vcvars32.bat` first. To build the release zip, first generate the interior maps from your own copy of the game (Python 3 with Pillow 11.2 or newer; the files are game assets and are not in this repository):

```powershell
python tools\build-interior-maps.py "<game>\CookedPC\pack0.dzip"
powershell -ExecutionPolicy Bypass -File tools\package-release.ps1
```

which writes `dist\Witcher2EngineFix-<version>.zip` (version from `CHANGELOG.md`, checked against `ENGINEFIX_VERSION` in `src\version.h`).

Research tooling (unpacking the game exe for Ghidra, probes) is described in [AGENTS.md](AGENTS.md#research-setup). None of it is needed to build the mod.

## License

[MIT](LICENSE). The Witcher 2 is a trademark of CD PROJEKT S.A.; this project is not affiliated with or endorsed by CD PROJEKT RED.
