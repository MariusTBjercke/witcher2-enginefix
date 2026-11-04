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
not re-checked), crash capture and the interior map files.

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
   `WF_RESET_COPY` (`be a0 67 62 01`, `WF_PP`), `WF_WRITER_SITE` (`a1 50 86 60 01`, a global).
   Build such bytes at run time from `ga()`, as `door_fade_site` does for `VA_DOOR_FADE_LEN`.
4. Re-check struct offsets the feature reads.
5. When a feature group has all its addresses, `build_gate` lets it run; test it in game on GOG.

## Still to do

* The engine fixes, by area: presets/LOD/distance (`RVA_SETTINGS`, `EF_RANGE_RVA`, `EF_SHADOW_RVA`), then
  window mode, quit and raycast guards, skip intro, then doors and interiors.
* Saves: the thumbnail fix and the fast save list go through `ISteamRemoteStorage`; GOG has its
  own save path, which needs its own research.

## `interiors=2` crash

Not explained yet. LAA is ruled out: the GOG exe has the flag, so preloading interiors has the
same 4 GB as on Steam. The player's log and crash dump would show more.
