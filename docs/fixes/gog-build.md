# The GOG build

GOG `3.5.0.26g`, looked at on 2026-11-04 after a player ported the mod to it on their own
(`interiors=2` crashes for them, `interiors=1` works). 0.9.0 is Steam only. From 0.10.0 the DLL
detects the build and runs the parts that need no game address on GOG; the engine fixes follow
one area at a time.

## How it differs from Steam

| | Steam 3.5.0.26 | GOG 3.5.0.26g |
|---|---|---|
| Linked | 2013-05 | 2023-04 (rebuilt, not the same binary) |
| Packing | SteamStub | none |
| `.text` | `0x8A94AD` | `0x8FC18D` (about 340 KB more) |
| LAA | yes | yes |
| ASLR | no (`DllCharacteristics 0x8100`) | **yes** (`0x8140`) |
| Platform | `steam_api.dll` | `Galaxy.dll` |
| `dinput8.dll` | imported | imported, so the proxy loads |
| `base_scripts.dzip` | | vanilla here; every script edit applies (`sptest test --game`) |

## What a port needs

* **Every address differs.** The DLL holds about 70 fixed game addresses, and each fix has to be
  found again in the GOG binary. Up to 0.9.0 the DLL tried the Steam addresses everywhere and
  relied on its byte checks; since 0.10.0 a feature whose addresses are not known on the running
  build stays off (see below).
* **ASLR.** The GOG exe can load anywhere, so every address gets the real base added. 0.10.0
  does that for all of them (`ga()`).
* **Saves.** The thumbnail fix and the fast save list go through `ISteamRemoteStorage`. GOG
  saves differently, so those two need their own work.
* **Script patches, maps, icons.** These are data. The script patches need only the file hooks,
  which 0.10.0 finds by import name. The map files are built from `pack0.dzip`, and GOG's has
  the same `globals\map_scales.csv` and the same 127 textures under `globals\gui\maps` as
  Steam's, byte for byte (compared 2026-11-04; both archives are 10 515 200 574 bytes with
  29 450 entries).

## How far the Steam addresses carry over (2026-11-04)

`tools/port-probe.py <steam unpacked exe> <gog exe>` turns 16 or 32 bytes at each Steam code
address in `docs/addresses.md` into a pattern (absolute addresses and rel32 targets masked) and
searches GOG's `.text`:

| | Count |
|---|---|
| Import slots (resolve by name at runtime, no GOG table needed) | 24 |
| Data, vtables and `.rdata` (follow from the code that uses them) | 25 |
| Code with a unique hit at 16 or 32 bytes (candidates, unverified) | about 18 |
| Code with no hit at all | about 28 |

The GOG build was recompiled, not shifted: most functions have other prologues and frame
layouts, so most sites need RE from strings, callers and vtables, and every original-byte check
and struct offset must be checked again. Candidates from the probe (Steam -> GOG):

| Name | Steam | GOG candidate |
|---|---|---|
| `VA_FADE_OUT_EXEC` | `009DC316` | `00A2D7E1` |
| `VA_GC` | `00ABD770` | `00B11BA4` |
| `VA_IMAGES_REMOVE` | `00B285C6` | `00B5DF9E` |
| `VA_INDOOR_CMP` | `009C0223` | `00A11286` |
| `VA_INDOOR_IMM` | `009C0232` | `00A11295` |
| `VA_MAP_FIND` | `004A4A03` | `004BDE35` |
| `VA_MEM_SERIALIZE` | `00ABF380` | `00B138EA` |
| `VA_MEM_SIZE` | `00ABF379` | `00B138E3` |
| `VA_RAY_HIT` | `0084C750` | `0091F110` |
| `VA_UNPACK` | `0068F3B0` | `007614F0` |
| `VA_UNPACK_CALL` | `0068E8E7` | `007618C7` |
| `WF_GAME_WNDPROC` | `009D237E` | `00A2362D` |
| `WF_RESET_COPY` | `0052D4FE` | `004CE64E` |
| `WF_VP_TICK` | `009E146E` | `00A32999` |
| `WK_WNDPROC` | `009D4BD9` | `00A25ED3` |
| `VA_DOOR_TIMER` | `0095DB00` | `005AF658` (16 bytes only) |
| `VA_PART_SWITCH` | `00A91BF9` | `00AE626C` (16 bytes only) |
| `WF_STYLE_SITE` | `009D45CA` | `00A258C0` (16 bytes only) |

These are file VAs at GOG's preferred base `0x00400000`; with ASLR the runtime address is
`GetModuleHandle(NULL) + (va - 0x00400000)`.

Up to 0.9.0 three naked stubs in `enginefix.c` held Steam addresses in their instructions
(`0x00AA7E18` = `VA_PART_REQUEST` in `ko_part_request`, `VA_STARTVIDEO_LIST` and
`VA_STARTVIDEO_STOP` in the skip-intro stubs), and the door fade patch compared with `0x00E30444`,
a float that had no name. Since 0.10.0 the stubs load the address from a variable set before the
patch, and the float is `VA_DOOR_FADE_LEN`.

## How the DLL tells the builds apart (0.10.0)

DllMain reads the loaded exe's PE header. The SteamStub leaves the header and the section table
as they are, so the packed Steam exe reads like the unpacked one:

| | TimeDateStamp | `.text` VirtualSize | Load address |
|---|---|---|---|
| Steam 3.5.0.26 | `518B881C` | `008A94AD` | `0x00400000` (no `DYNAMIC_BASE`; only forced ASLR in Windows would move it, and `ga()` adds the delta then too) |
| GOG 3.5.0.26g | `644689F9` | `008FC18D` | anywhere (ASLR) |

Anything else counts as unknown: only what needs no game address runs there (crash capture,
the OSD, the DirectInput keyboard filter, and `cpu32_guard` and `file_trace` when that exe imports
their functions); the script patches stay off. The log's `build:` line names the build, the
delta from `0x00400000` and how many import slots were found.

* **Addresses.** The `#define`s stay the Steam values. Every use goes through `ga(VA_...)`
  (absolute) or `ga_rva(EF_.../RVA_...)`: on Steam the define plus the delta (0), on GOG the
  value in `k_gog` (`src/enginefix.c`) plus the delta, and 0 when `k_gog` has 0. Hot hooks keep
  the result in a variable set before the hook goes in. Code that needs an address checks it
  for 0 or is behind `build_gate`.
* **Gate.** `build_gate()` runs in DllMain before any feature thread: each feature group whose
  addresses are not all known on the running build has its ini switches set off, with one line
  `<area>: not on this build (GOG 3.5.0.26g) - <feature> off`. On Steam it changes nothing.
* **Import slots** are found by DLL and function name in the exe's import directory
  (`k_imports`, `imports_find`). On Steam they are exactly the old fixed slots (the packed exe's
  directory is the original one); on GOG all but `steam_api.dll!SteamRemoteStorage` are there.
* `window_mode` adds Options rows that call the window patches, so it is gated with the `WF_`
  sites. `pause_quit` adds the pause menu's Quit, which crashes during shutdown without
  `quit_guard` (`quit-crash.md`), so it waits for `VA_FACTS_TIME`.

What runs on GOG with 0.10.0: the script patches `book_marker`, `continue` and `medallion` (they
only hook file APIs; `build\sptest.exe test --game "<gog>"` passes), the F9/F10 keyboard filter
(DirectInput objects, no game address), `cpu32_guard` (an import; GOG's own CPU count code is
not re-checked), crash capture and the interior map files. Ported since (below): LOD, the presets
with Ctrl+F10 and the distance hooks; then `quit_guard` (and with it `pause_quit`),
`raycast_guard`, `skip_intro`, the window patches with the Options rows (`window_mode`) and the
Windows keys.

## Ported addresses

| Steam | GOG | Proof |
|---|---|---|
| `RVA_SETTINGS` `02180E28` | `021EAEB8` | getter `004024EF` (`A1 B8 AE 5E 02`), creator `00402517` stores the static object `025EAF08`, exit handler clears it at `004025EB`; loader `00B40DCD` registers the same 50 keys at the same offsets (`graphics-presets.md`, `research.md` §6) |
| `EF_RANGE_RVA` `005BB09F` | `0060C120` | same function and bytes, distance in XMM7 (`render-distance-experiment.md`) |
| `EF_SHADOW_RVA` `005BB465` | `0060C4F6` | same, compare with XMM7 |
| `VA_GAME` `01404950` | `01475740` | written at `00AFB1DC` after `L"Initializing Game..."`; `activeWorld` registered at +0x40 and `player` at +0x250 as on Steam |
| `VA_FACTS_TIME` `0099871E` | `0050B0A9` | in FactsAdd `0050B044` (string `00D441D4`, registration `0050B0FF`); bytes per build (`quit-crash.md`) |
| `VA_RAY_SLOT` `00D41A38` | `00E08740` | `hkpSimpleWorldRayCaster` RTTI, vtable `00E0873C` slot 1 |
| `VA_RAY_HIT` `0084C750` | `0091F110` | identical 0x1A5-byte body (`crash-raycast-stale-body.md`) |
| `VA_STARTVIDEO_LIST` `009A2190` | `00509DF5` | `video_start.csv` handle, `Release` column; thiscall (`skip-intro.md`) |
| `VA_STARTVIDEO_CALL` `009B588F` | `00509EF7` | the only call to the builder, in the start wrapper `00509EEB` |
| `VA_STARTVIDEO_TICK` `009A211D` | `0050A01E` | `CVideoInstance` vtable `00D43F3C` + 0x74 |
| `VA_STARTVIDEO_STOP` `009A1F2E` | `00509D26` | pushes `StopVideo`; thiscall |
| `VA_STARTVIDEO_VSLOT` `00ECAF78` | `00D43FB0` | the vtable slot above |
| `WF_*` (8) and `WF_CONFIG` | see `borderless-window.md`, "GOG 3.5.0.26g" | the window creation, the render interface and the device code are the same functions; the config writer is `__thiscall` on GOG |
| `WK_WNDPROC_SLOT` `00EC6560` / `WK_WNDPROC` `009D4BD9` | `00F68FA0` / `00A25ED3` | `CViewportWindow` RTTI, the same procedure (`windows-keys.md`) |

Seen in game on GOG on 2026-11-04 (`docs/validation/gog-presets-2026-11-04.md`): the settings
object found at its static address plus the delta, LOD enforced, both distance hooks installed,
non-zero `baseline_reject`/`admitted`, Ctrl+F10 through all four levels.

## Open

* On GOG the log usually ends at the CRT's shutdown filter change without `unloaded`, even with
  the detach only trying the log lock: three of four GOG runs on 2026-11-04, with and without a
  save loaded; the first, with only the phase-0 features, did write it. Every Steam run ends with
  `unloaded`. The game closes normally (Galaxy logs a normal close, no dump, no error event);
  whether the DLL's detach runs at all there is not known yet. Next step: log the exe's
  `ExitProcess` call (its import slot by name) and, when the log lock is busy at detach, write a
  marker through the raw file handle.

Start tests on 2026-11-04 (owner, the same 0.10.0 build on both):

* Steam: the log matched 0.9.0's line for line apart from the new `build:` line (every hook
  installed, `import slots 24/24`, LOD, distance, doors, preload, window, saves, skip intro,
  quit guard catching 2 `FactsAdd` calls at Quit); no dump.
* GOG (vanilla scripts, A Better UI): `build: GOG 3.5.0.26g`, loaded at `0x00D30000` (delta
  `0x00930000`), `import slots 23/24` (no `SteamRemoteStorage`), 11 groups `not on this build`,
  `cpu32: hooked`, the script hooks 3/3, overlay built (`ui_trade.swf`/`ui_storage.swf` patched
  inside `abetterui.dzip`), crash filter armed; Continue in the main menu, no Display mode rows;
  no dump. Only the menu was visited, and DirectInput's keyboard is created when play starts
  (input mode 3), so the keyboard filter was not reached in that run. Other modules in the GOG
  process call `DirectInput8Create` too (4 calls before play; the exe has a single call site,
  in the input mode switch), which is why each hooked DirectInput vtable now keeps its own
  original.

`tools\buildprobe\build-and-run.bat "<steam exe>" "<gog exe>"` maps both real exes and checks the
detection, every import slot against the values read from their import tables, `ga()` with a
non-zero delta, and which switches the gate turns off on each build.

## Porting an address

1. Prove the GOG site (`w2-game-re`): the same function by strings, callers or vtables, its
   contract, its bytes. A probe candidate from the table above is not proof.
2. Put the GOG value, at the preferred base `0x00400000` (an RVA for `EF_`/`RVA_` names), into
   `k_gog` and run `python -I tools/address-table.py`.
3. Make the byte checks hold on GOG. They are Steam's: most will differ on the recompiled code,
   and some embed absolute addresses that the loader relocates under ASLR, so a fixed pattern
   cannot match there: `VA_STREAM_DTOR` (`c7 06 2c 2c f0 00`, the stream vtable),
   `WF_RESET_COPY` (`be a0 67 62 01`, `WF_PP`), `WF_WRITER_SITE` (`a1 50 86 60 01`, a global),
   GOG's `VA_STARTVIDEO_STOP` (`68 18 3e d4`, a string). Build such bytes at run time from
   `ga()`, as `door_fade_site` does for `VA_DOOR_FADE_LEN`, or check only their low 16 bits,
   which survive the 64 KB load steps. Bytes that differ per build go through `by_build()`.
4. Re-check struct offsets the feature reads, and the calling convention: the builds were
   compiled apart, and a function can keep its bytes and still take `this` in another register
   (the config writer, the skip-intro builder and stop).
5. When a feature group has all its addresses, `build_gate` lets it run; test it in game on GOG.

## Still to do

* Doors and interiors (`[loading]`: the partition switch, the door timer and start, the fades,
  the indoor draw check), the largest set; a player saw `interiors=2` crash on their own port.
* Saves: the thumbnail fix and the fast save list go through `ISteamRemoteStorage`; GOG has its
  own save path, which needs its own research.

## `interiors=2` crash

Not explained yet. LAA is ruled out: the GOG exe has the flag, so preloading interiors has the
same 4 GB as on Steam. The player's log and crash dump would show more.
