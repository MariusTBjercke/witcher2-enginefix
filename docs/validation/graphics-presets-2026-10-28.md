# Graphics preset validation, 2026-10-28

## Automated evidence

- Before implementation, the real config/handler preset probe compiled and
  reported 17 expected runtime failures. Existing 27 native and 9 legacy checks
  still passed.
- After implementation, `tools/distanceprobe/build-and-run.bat` freshly compiled
  and passed 27 native, 9 legacy handler and 21 preset checks (57 total, exit0).
- `build.bat` succeeded. Only the four existing LNK4104 COM-export warnings remain;
  no new C compiler warnings.
- Independent reviewer compared against `build/before-presets/enginefix.c`,
  freshly rebuilt/reran all 57 checks and reported no blockers. Reviewed startup,
  override order, validation, physical/focus edges, legacy fallback and no ini
  persistence. DirectInput filtering unchanged; native bridges unchanged.

## Installation evidence

Game process was absent. Copied built DLL and distributed ini into
`D:/SteamLibrary/steamapps/common/the witcher 2/bin` with previous files backed up
under `build/before-presets/`. Hashes of installed and repo files matched:

| Artifact | SHA256 |
|---|---|
| dinput8.dll | 1F4535B5A7CF2315644BB2EE9DBFF1083376A8F657CBD587A609403E02798819 |
| witcher2_enginefix.ini | DCD8EACF0F0EE18D32B4249B17D095F6B673EBA6C42CDB06C51BD92E5D8CC31A |
| game witcher2.exe (unchanged) | CB9FBC9C1DC7AE0AC73C5768E55724CC8E7B78999299F0A2E6E5AB7D74A55FE8 |

`Documents/Witcher 2/Config/User.ini` and `Rendering.ini` remain read-only.
UberSampling and DXVK settings were not edited.

## Remaining human check

Start via Steam/Launcher. Confirm the game starts with `extreme` and Ctrl+F10
cycles baseline/high/ultra/extreme with the named OSD, without quick load.
Restart should return to ini `default=`. The owner previously confirmed the
individual four-control build's visual improvement; this new combined interface
has not yet been verified in game. No claim that presets fix NPC streaming or
the sun-shadow quality boundary.
