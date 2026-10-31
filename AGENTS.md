# witcher2-enginefix - agent notes

Runtime fixes for **The Witcher 2: Assassins of Kings - Enhanced Edition** (Steam,
appid 20920, 3.5.0.26), shipped as a `dinput8.dll` proxy in the game's `bin\`.

Sibling project with the same architecture:
[witcher-enginefix](https://github.com/MariusTBjercke/witcher-enginefix) (The Witcher 1).

## Reading order for a fresh session

1. This file.
2. `docs/research.md` - binary, packing, config system, settings object, pitfalls.
3. The `docs/fixes/*.md` entry for the area you touch (`save-previews.md`,
   `graphics-presets.md`, `render-distance-experiment.md`, ...).
4. `docs/plans/progress.json` for open and deferred items.

## Project at a glance

| What | Where |
|---|---|
| All runtime code | `src/enginefix.c` (proxy, LOD, presets, input filter, save thumbnails) |
| Renderer distance hooks | `src/distance_patch.h` |
| Shipped settings | `dist-files/witcher2_enginefix.ini` |
| Interior maps (data, not DLL) | `tools/build-interior-maps.py <pack0.dzip>` -> `build/interior-maps/` (game assets, never committed; `package-release.ps1` requires it) |
| Version | `ENGINEFIX_VERSION` (+ `_BIN`) in `src/version.h` = top entry of `CHANGELOG.md` |
| Release zip | `tools/package-release.ps1` -> `dist/Witcher2EngineFix-<version>.zip` |
| Validation evidence | `docs/validation/` |

## Paths

Machine-specific; nothing in the build depends on them.

| What | Where |
|---|---|
| Game install | `<game>` = e.g. `D:\SteamLibrary\steamapps\common\the witcher 2` |
| Installed DLL + ini | `<game>\bin\dinput8.dll`, `<game>\bin\witcher2_enginefix.ini` |
| Runtime log | `<game>\bin\witcher2_enginefix.log` (held open exclusively while the game runs) |
| User config (**outside** the game folder) | `%USERPROFILE%\Documents\Witcher 2\Config\` |
| Steam saves | `C:\Program Files (x86)\Steam\userdata\<steamid>\20920\remote\` |
| Unpacked analysis copy | `tools\ghidra-inputs\witcher2.exe.unpacked.exe` (git-ignored) |
| Ghidra programs | `/witcher2/witcher2.exe` (packed), `/witcher2/witcher2.exe.unpacked.exe` |
| MSVC x86 | `C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat` (`build.bat` also tries VS 2022; or set `VCVARS`) |

## Build, install, release

```powershell
# build -> build\dinput8.dll
& .\build.bat

# install (game and launcher closed - the DLL is locked while the game runs)
$game = "D:\SteamLibrary\steamapps\common\the witcher 2"
Copy-Item build\dinput8.dll "$game\bin" -Force
Copy-Item dist-files\witcher2_enginefix.ini "$game\bin" -Force

# verify the DirectInput hook without launching the game (cwd must be the game's bin)
& tools\inputprobe\probe.exe

# release zip (bump CHANGELOG.md and ENGINEFIX_VERSION together first)
powershell -ExecutionPolicy Bypass -File tools\package-release.ps1
gh release create v<version> dist\Witcher2EngineFix-<version>.zip --title "Witcher 2 Engine Fix <version>" --notes-file <notes.md>
```

`tools/inputprobe/probe.exe` is git-ignored; build it once with
`cl /nologo /O2 /MT /guard:cf tools\inputprobe\probe.c dinput8.lib dxguid.lib user32.lib /Fe:tools\inputprobe\probe.exe`
from an x86 developer prompt. Note that it injects real F9/F10 key presses:
never run it while the game is in the foreground.

No CI: the build needs a local MSVC, and every behaviour test needs the game.

## Research setup

Only needed for reverse engineering, not for building.

1. **Steamless** (SteamStub unpacker), v3.1.0.5 was used: download from
   <https://github.com/atom0s/Steamless/releases> and extract to `tools\steamless\`
   (git-ignored; third-party, not redistributed here).
2. Copy the game's `bin\witcher2.exe` to `tools\ghidra-inputs\` and unpack the **copy**:
   `tools\steamless\Steamless.CLI.exe tools\ghidra-inputs\witcher2.exe`
   -> `witcher2.exe.unpacked.exe` (SteamStub Variant 2.1, OEP `0x0066F59D`). Check
   the original's SHA-256 against `docs/research.md` §1 first. Game binaries are
   git-ignored and must never be committed.
3. **Ghidra** with the GhidraMCP bridge: import both into project folder `/witcher2/`,
   analyze the unpacked one (39 339 functions). The bridge's HTTP API
   (`http://127.0.0.1:8089/decompile_function?address=...&program=/witcher2/witcher2.exe.unpacked.exe`)
   is usable from a shell when the MCP tools do not load.
4. Optional: Windows SDK Debugging Tools (x86 `cdb.exe`) for **offline** dump
   inspection only - see the save-preview incident below.

## Rules

* Never modify or replace `<game>\bin\witcher2.exe`. Unpack a **copy** for analysis.
* `Documents\Witcher 2\Config\User.ini` and `Rendering.ini` are **read-only by design**
  on the owner's machine (hand-tuned; the launcher and the in-game options menu would
  rewrite them). Unlock, edit, re-lock:
  `(Get-Item "$env:USERPROFILE\Documents\Witcher 2\Config\User.ini").IsReadOnly = $false`
* The game cannot be started from the command line - Steam launches `Launcher.exe` and
  a human has to press *Launch game*. Assume a human is needed for an in-game test.
* After changing anything under `[hotkeys] suppress_mod_keys` or any vtable/IAT hook
  signature, ask for a start test before doing more work: a wrong COM signature
  kills the process silently, with no WER entry (see `docs/research.md` §6).
* No live software breakpoints in the game process (see below). Prefer passive
  in-process logging (`[diagnostics] save_trace` is the pattern).
* Keep machine paths and Steam IDs out of tracked files (`%USERPROFILE%`, `<steamid>`, `<game>`).

## Verified facts (Steam 3.5.0.26, unpacked; re-verify before contradicting)

* No ASLR, image base `0x00400000`: absolute addresses are stable at runtime.
* Config keys are **UTF-16** strings in `.rdata`. ASCII scans miss every one of them.
* Section `VirtualAddress` values are **relative to the image base**:
  `VA = 0x400000 + SectionVA + (fileOffset - RawPtr)`. Forgetting `0x400000` still
  produces plausible-looking pointers into `.text`.
* The config/`CName` name pool has **no static xrefs**. Reach key behaviour through the
  registration code (`FUN_00b07714`), not by chasing the strings.
* `IDirectInput8::CreateDevice` has **3 parameters plus `this`** - not 4 plus `this`.
* The shipped exe is SteamStub Variant 2.1 (`.bind`); the packed import shows ~400
  functions, the unpacked one 39 339. At runtime the stub fills the original IAT, so
  IAT slots from the unpacked file are valid in the running game
  (e.g. `D3DXSaveSurfaceToFileInMemory` at `00CAD4AC`, `SteamRemoteStorage` at `00CAD624`).
* Settings singleton `DAT_02580E28`: `+0x34/+0x38` gameplay LOD downscale flags,
  `+0x68` FoliageDistanceScale, `+0x6C` MeshDistanceScale.
* Steam storage: `steam_api.dll` exposes `STEAMREMOTESTORAGE_INTERFACE_VERSION004`;
  `FileWrite` is vtable slot 0, thiscall, 3 stack args (call site `0052AD91..ADA5`).
  The game ignores its return value.
* Save previews: `009B63DE` queues the in-memory 640x360 capture (`009B6BC5`) and
  saves (`009B79AD` -> `009B7149`) in the same tick; the capture lands ~25 ms later.
  That race is the whole missing-preview bug. `[FileSystem] CloudStorage` in User.ini is
  overridden at startup when Steam already has saves (`00AC9152`).
* The live CDB save-preview trace is **disabled**: the game crashed on its first
  software breakpoint at VA `009B6BC5` with `STATUS_WX86_BREAKPOINT` (`4000001F`).
  Do not repeat that workflow; see `docs/fixes/save-previews.md`.

## Conventions

See `docs/conventions.md` (commit style, versioning, releases, language).
