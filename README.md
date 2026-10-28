# Witcher 2 Engine Fix

Draw-distance and save-preview fixes for **The Witcher 2: Assassins of Kings - Enhanced Edition**. A small `dinput8.dll` that the game loads by itself and that adjusts specific engine values and bugs in memory while the game runs. `witcher2.exe` and your saves are never modified.

Sibling of [Witcher Engine Fix](https://github.com/MariusTBjercke/witcher-enginefix) for The Witcher 1, built the same way.

## What it does

| | What you notice | Default |
|---|---|---|
| **Missing save previews** | New saves showed a purple/blue gradient instead of a screenshot in the Load menu. The game asks for the screenshot and saves in the same frame, and the finished image arrives about a frame too late, so the game drops it. The mod writes that image next to the save under the name the game expects, and makes the menu reload a QuickSave's picture right away instead of after a restart. Old saves cannot get their picture back. | on |
| **Graphics presets** | One hotkey, **Ctrl+F10**, cycles baseline / high / ultra / extreme. Each level sets mesh LOD distance, foliage distance, the hard mesh visibility cutoff and the mesh-shadow distance together, with an on-screen readout. Starts at `extreme`; the hotkey lasts until you quit. | on (extreme) |
| **LOD values that stick** | The game's `MeshDistanceScale`/`FoliageDistanceScale` are held at the preset values, also after the options menu rewrites them, and the two "gameplay LOD downscale" flags are kept off. | on |
| **Ctrl+F9/F10 do not quick-load** | F9 (quick load) and F10 cannot be rebound in this game. While Ctrl or Alt is held, the mod hides F9/F10 from the game, so the mod's hotkeys never trigger a quick load. Plain F9 still quick-loads. | on |

Each part can be switched off in `witcher2_enginefix.ini`. Details, addresses and how each part was verified are in [`docs/fixes/`](docs/fixes/).

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

1. Download `Witcher2EngineFix-<version>.zip` from [Releases](https://github.com/MariusTBjercke/witcher2-enginefix/releases).
2. Extract it into the game folder, so that `dinput8.dll` ends up next to `witcher2.exe` in `bin\`, for example
   `...\steamapps\common\the witcher 2\bin\dinput8.dll`
3. Start the game as usual (through Steam and the launcher).

To check that it works, open `bin\witcher2_enginefix.log` after starting the game. It should begin with `witcher2_enginefix 0.1.0 loaded` and later show `distance: hooks installed` and `saves: ISteamRemoteStorage ... FileWrite hooked; thumbnail fix=1`. After a new save, the log says `thumbnail: wrote ..._640x360.bmp`.

| File | Purpose |
|---|---|
| `bin\dinput8.dll` | The engine fix. The game loads it automatically and it forwards everything else to Windows' own `dinput8.dll` |
| `bin\witcher2_enginefix.ini` | Settings |

**Uninstall:** delete `bin\dinput8.dll` and `bin\witcher2_enginefix.ini`.

## Configuration

`bin\witcher2_enginefix.ini` is read at startup and never written by the mod. The most useful keys:

| Section / key | Default | Meaning |
|---|---|---|
| `[general] enabled` | 1 | 0 turns every patch off (input still forwarded) |
| `[general] log` | 1 | 0 stops writing `witcher2_enginefix.log` |
| `[presets] default` | extreme | Startup level: baseline, high, ultra or extreme |
| `[presets] cycle` | Ctrl+F10 | Hotkey that cycles the levels (F9/F10 with Ctrl or Alt) |
| `[preset.<name>]` | see table | Your own values per level |
| `[saves] fix_thumbnails` | 1 | 0 turns the save-preview fix off |
| `[osd] enabled`, `size`, `ms` | 1, 34, 2500 | On-screen readout; 34 px suits 4K, 22 px 1080p |
| `[diagnostics] save_trace` | 0 | 1 logs the save-preview path (Steam writes, screenshot encoding) |

The ini file itself documents every key, including the legacy separate hotkeys (`[presets] enabled=0`).

## Compatibility

- **DXVK**: works with and without it. The on-screen readout is a small topmost window, so it shows in borderless and DXVK full screen but not in true exclusive full screen.
- **Other `dinput8.dll` mods**: only one `dinput8.dll` can sit in `bin\`. This mod loads the system `dinput8.dll` itself.
- **Steam Cloud**: no effect on the fix; saves and previews are written through the game's own Steam storage interface.

## Building from source

Needs Visual Studio 2022 or newer with the **Desktop development with C++** workload (x86 tools). Nothing else.

```bat
build.bat
```

This produces `build\dinput8.dll`. If Visual Studio is not in the default location, set `VCVARS` to the full path of `vcvars32.bat` first. To build the release zip:

```powershell
powershell -ExecutionPolicy Bypass -File tools\package-release.ps1
```

which writes `dist\Witcher2EngineFix-<version>.zip` (version from `CHANGELOG.md`, checked against `ENGINEFIX_VERSION` in `src\enginefix.c`).

Research tooling (unpacking the game exe for Ghidra, probes) is described in [AGENTS.md](AGENTS.md#research-setup). None of it is needed to build the mod.

## License

[MIT](LICENSE). The Witcher 2 is a trademark of CD PROJEKT S.A.; this project is not affiliated with or endorsed by CD PROJEKT RED.
