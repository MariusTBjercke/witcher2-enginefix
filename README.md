# Witcher 2 Engine Fix

Draw-distance, save-preview, interior-map and quality-of-life fixes for **The Witcher 2: Assassins of Kings - Enhanced Edition**. A small `dinput8.dll` that the game loads by itself and that adjusts specific engine values and bugs in memory while the game runs. `witcher2.exe` and your saves are never modified.

Sibling of [Witcher Engine Fix](https://github.com/MariusTBjercke/witcher-enginefix) for The Witcher 1, built the same way.

## What it does

| | What you notice | Default |
|---|---|---|
| **Missing save previews** | New saves showed a purple/blue gradient instead of a screenshot in the Load menu. The game asks for the screenshot and saves in the same frame, and the finished image arrives about a frame too late, so the game drops it. The mod writes that image next to the save under the name the game expects, and makes the menu reload a QuickSave's picture right away instead of after a restart. Old saves cannot get their picture back. | on |
| **Save list memory leak** | Each time the Load/Save menu built its list, the game read every save's preview again and leaked it when the menu already had that picture: ~0.9 MB per save per menu open, kept until you quit. With many saves that adds up to hundreds of MB and is a likely cause of crashes once saves pile up. The mod frees it. Nothing is deleted and saves are not limited. | on |
| **Graphics presets** | One hotkey, **Ctrl+F10**, cycles baseline / high / ultra / extreme. Each level sets mesh LOD distance, foliage distance, the hard mesh visibility cutoff and the mesh-shadow distance together, with an on-screen readout. Starts at `extreme`; the hotkey lasts until you quit. | on (extreme) |
| **LOD values that stick** | The game's `MeshDistanceScale`/`FoliageDistanceScale` are held at the preset values, also after the options menu rewrites them, and the two "gameplay LOD downscale" flags are kept off. | on |
| **Black map in interiors** | In 25 interior map entries (temples, inns, Loredo's house, towers and other houses) the map screen (M) was black with one huge pin, because the game has no big map for them. The zip ships loose files that give each of these areas a 2048x2048 upscale of its own minimap, so the map shows and zooms the right way. | on (files) |
| **Skip the startup videos** | `[startup] skip_intro=1` starts at the main menu without the logo videos and the intro movie. The mod presses "skip" for the game on each startup movie (the same path as Esc), so a short black moment remains. Works with or without an intro removal mod. | off |
| **Ctrl+F9/F10 do not quick-load** | F9 (quick load) and F10 cannot be rebound in this game. While Ctrl or Alt is held, the mod hides F9/F10 from the game, so the mod's hotkeys never trigger a quick load. Plain F9 still quick-loads. | on |

Each DLL part can be switched off in `witcher2_enginefix.ini`; the interior maps are removed by deleting their files (see below).

### Optional script patches (off until you install them)

`Witcher2EngineFix_Optional\ScriptPatches\Install.bat` asks for each of these and patches the game files you have (your `base_scripts.dzip`, also when a mod like Project Mersey replaced it, and for the book marker the trade/storage screens, also when an HD UI mod replaced them). Originals go to its `backup\` folder; `Uninstall.bat` restores them. Run `Install.bat` again to change the choice.

| | What you notice |
|---|---|
| **Book read markers** | Books show `Read`, `Nothing new` or `Unread` in the inventory, trade and storage lists, with a tooltip describing what reading them would still teach you. A tick marks books you have read; at merchants and in storage it also keeps the game's carried-copy meaning. `Nothing new` is text only, since the book itself is still unread. Reading a book refreshes the inventory immediately. Looted books show the read tick too; the loot window shows `(Nothing new)` after the name where applicable. In 12 languages. |
| **Continue** | The main menu gets a Continue button (the game's own translated label) that loads your newest save. |
| **Quit in the pause menu** | The pause menu gets the game's Quit button, with a confirmation, next to "Exit to main menu". |

Details: [shop-book-read-marker.md](docs/fixes/shop-book-read-marker.md), [menu-buttons.md](docs/fixes/menu-buttons.md). Details, addresses and how each part was verified are in [`docs/fixes/`](docs/fixes/).

### Preset values

| Preset | Mesh LOD | Foliage | Mesh visibility | Mesh shadow |
|---|---:|---:|---:|---:|
| baseline (the game's own) | 1 | 1 | 1 | 1 |
| high | 2 | 2 | 1.5 | 1.5 |
| ultra | 4 | 3 | 2 | 2 |
| extreme | 8 | 5 | 3 | 3 |

The higher levels cost GPU time. They do not change how far away the game spawns characters (that is a separate system, see [docs/fixes/npc-streaming-sun-shadows.md](docs/fixes/npc-streaming-sun-shadows.md)).

## Requirements

- The Witcher 2: Assassins of Kings - Enhanced Edition, **3.5.0.26**. Tested on **Steam** (Windows 11), with both DXVK and the game's native Direct3D 9.
- The save-preview fix works when saves go through Steam storage (the default for the Steam version). GOG has not been tested. The renderer hooks check the exact game code before they patch anything; on another build they skip themselves and say so in the log.

## Installation

1. Download `Witcher2EngineFix-<version>.zip` from [Nexus Mods](https://www.nexusmods.com/witcher2/mods/1200) or [GitHub Releases](https://github.com/MariusTBjercke/witcher2-enginefix/releases).
2. Extract it into the game folder (the one that holds `bin\` and `CookedPC\`), so that `dinput8.dll` ends up next to `witcher2.exe` in `bin\`, for example
   `...\steamapps\common\the witcher 2\bin\dinput8.dll`
3. Start the game as usual (through Steam and the launcher).

To check that it works, open `bin\witcher2_enginefix.log` after starting the game. It should begin with `witcher2_enginefix 0.5.0 loaded` and later show `distance: hooks installed` and `saves: ISteamRemoteStorage ... FileWrite hooked; thumbnail fix=1`. After a new save, the log says `thumbnail: wrote ..._640x360.bmp`.

| File | Purpose |
|---|---|
| `bin\dinput8.dll` | The engine fix. The game loads it automatically and it forwards everything else to Windows' own `dinput8.dll` |
| `bin\witcher2_enginefix.ini` | Settings |
| `CookedPC\globals\map_scales.csv` | The game's map table with the missing interior maps filled in |
| `CookedPC\globals\gui\maps\*_2048x2048.dds` | 25 upscaled interior maps (new names, nothing of the game's is replaced) |
| `Witcher2EngineFix_Optional\ScriptPatches\` | The optional script patches; nothing changes until you run `Install.bat` |

**Uninstall:** run `Witcher2EngineFix_Optional\ScriptPatches\Uninstall.bat` if you installed script patches, then delete `bin\dinput8.dll`, `bin\witcher2_enginefix.ini`, `CookedPC\globals\map_scales.csv` and the `*_2048x2048.dds` files the zip put in `CookedPC\globals\gui\maps\` (the game's own maps live inside `pack0.dzip`, not in that folder).

## Configuration

`bin\witcher2_enginefix.ini` is read at startup and never written by the mod. The most useful keys:

| Section / key | Default | Meaning |
|---|---|---|
| `[general] enabled` | 1 | 0 turns off DLL patches (input still forwarded); map files and installed script patches are removed separately |
| `[general] log` | 1 | 0 stops writing `witcher2_enginefix.log` |
| `[presets] default` | extreme | Startup level: baseline, high, ultra or extreme |
| `[presets] cycle` | Ctrl+F10 | Hotkey that cycles the levels (F9/F10 with Ctrl or Alt) |
| `[preset.<name>]` | see table | Your own values per level |
| `[saves] fix_thumbnails` | 1 | 0 turns the save-preview fix off |
| `[saves] fix_list_leak` | 1 | 0 turns the save-list leak fix off |
| `[startup] skip_intro` | 0 | 1 starts at the main menu, without logo videos and intro |
| `[osd] enabled`, `size`, `ms` | 1, 34, 2500 | On-screen readout; 34 px suits 4K, 22 px 1080p |
| `[diagnostics] save_trace` | 0 | 1 logs the save-preview path (Steam writes, screenshot encoding) |
| `[diagnostics] savelist_trace` | 0 | 1 logs each save-list build (time, Steam reads, memory) |

The ini file itself documents every key, including the legacy separate hotkeys (`[presets] enabled=0`).

## Compatibility

The optional pause-menu Quit confirmation currently needs one click before it
responds to hover. The button still works; this is a known minor issue.

- **DXVK**: works with and without it. The on-screen readout is a small topmost window, so it shows in borderless and DXVK full screen but not in true exclusive full screen.
- **Other `dinput8.dll` mods**: only one `dinput8.dll` can sit in `bin\`. This mod loads the system `dinput8.dll` itself.
- **Other map mods**: a mod that ships its own `CookedPC\globals\map_scales.csv` conflicts with the interior maps; whichever was copied last wins. UI mods that replace `ui_nav.swf` (A Better UI, HD UI upscales) are fine.
- **Steam Cloud**: no effect on the fix; saves and previews are written through the game's own Steam storage interface.

## Building from source

Needs Visual Studio 2022 or newer with the **Desktop development with C++** workload (x86 tools). Nothing else.

```bat
build.bat
```

This produces `build\dinput8.dll`. If Visual Studio is not in the default location, set `VCVARS` to the full path of `vcvars32.bat` first. To build the release zip, first generate the interior maps from your own copy of the game (Python 3 with Pillow 11.2 or newer; the files are game assets and are not in this repository):

```powershell
python tools\build-interior-maps.py "<game>\CookedPC\pack0.dzip"
powershell -ExecutionPolicy Bypass -File tools\package-release.ps1
```

which writes `dist\Witcher2EngineFix-<version>.zip` (version from `CHANGELOG.md`, checked against `ENGINEFIX_VERSION` in `src\enginefix.c`).

Research tooling (unpacking the game exe for Ghidra, probes) is described in [AGENTS.md](AGENTS.md#research-setup). None of it is needed to build the mod.

## License

[MIT](LICENSE). The Witcher 2 is a trademark of CD PROJEKT S.A.; this project is not affiliated with or endorsed by CD PROJEKT RED.
