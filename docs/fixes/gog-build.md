# The GOG build

GOG `3.5.0.26g`, looked at on 2026-11-04 after a player ported the mod to it on their own
(`interiors=2` crashes for them, `interiors=1` works). 0.9.0 is Steam only. 0.10.0 detects the
build and supports GOG with every feature: each fixed address has a GOG value, and each hook
whose calling convention differs there has its own stub (below).

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
* **Saves.** On Steam the thumbnail fix and the fast save list go through
  `ISteamRemoteStorage`. GOG has no storage provider and saves to local files, so both have
  their own GOG path (`save-previews.md` and `save-bloat.md`, "GOG").
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
Windows keys; then the doors and interiors (`[loading]`) and `load_trace`; then the saves:
`fix_thumbnails`, `fast_list` and `savelist_trace` (`fix_list_leak` is not needed there).

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
| `VA_PART_SWITCH` `00A91BF9` | `00AE626C` | pushes L"Streaming PH: " (one copy); the same offsets (`+0x30C`, `+0x310`, `+0x358`), a 0x58-byte job, 3 callers as on Steam; the same 12 bytes |
| `VA_PART_SWITCH_A` / `_B` | `00AFE3F0` / `00AFF2DC` | its two calls: the request queue `00AFE36B` and `00AFF209` |
| `VA_PART_RUN` `00A91952` / `VA_PART_RUN_CALL` `00A91DAD` | `00AE5FCA` / `00AE6418` | the stage strings ("Flushing...", "GC...", "Attaching entities..."); **the job in EDI**, no stack arguments, plain `RET` (Steam: stdcall, `RET 4`); both callers load EDI first (`mov edi,esi; call`, and the async tick) |
| `VA_PART_GC_CALL` `00A91AD7` / `VA_GC` `00ABD770` | `00AE614D` / `00B11BA4` | after "GC..."; the GC's 14 instructions as on Steam, stdcall `RET 4`, its rel32 differs |
| `VA_PART_REQUEST` `00AA7E18` | `00AFCFA6` | the StreamWorldPartition (mode 0) and LoadWorldPartition (mode 1) natives and the door task (mode 4) call it, 10 callers on each; **`__thiscall`, the game in ECX** (Steam: EAX); `RET 0xC` |
| `VA_EMPTY_STRING` `0160FBA0` | `01680110` | pushed by every request caller |
| `VA_DOOR_TIMER_SLOT` / `VA_DOOR_TIMER` | `00D7A4CC` / `005AF658` | `CStreamingDoor` RTTI, vtable `00D7A394` (154 slots on both), slot 78; L"StreamingDoorTimer"; thiscall `RET 8` |
| `VA_DOOR_START_SLOT` / `VA_DOOR_START` | `00D7A5F8` / `005AF6D9` | slot 153; "Overlapped door streaming task on door '%s'"; thiscall `RET 4` |
| `VA_DOOR_TELEPORT_VT` / `VA_DOOR_FADE_EVENT` | `00D7A318` / `005B0276` | `CDoorStreamingBlackScreen` RTTI, slot 2; thiscall `RET 8`, reads only the event's CName at +4 |
| `VA_DOOR_INTERIOR_VT` | `00D7A36C` | `CDoorStreamingWaitOnKnob` RTTI |
| `VA_CNAME_FROM_STR` `00B28D76` | `00B5E7AC` | the door handler builds its own `fade_out` id with it (`005B0293`); **`__thiscall`, `out` in ECX, `RET 4`** (Steam: stdcall, `RET 8`) |
| `VA_STR_FADE_OUT` | `00D7A328` | L"fade_out", the only copy |
| `VA_DOOR_FADE_OUT` / `_IN` / `_LEN` | `005B01DF` / `005B02ED` / `00D269BC` | the two loads of the fade length in the BlackScreen constructor and handler: **`fld dword [00D269BC]`** (`D9 05`, disp32 at +2; Steam `movss xmm0`, `F3 0F 10 05`, disp32 at +4). `00D269BC` is a 0.5 shared by 324 sites, so only the two instructions are repointed |
| `VA_FADE_OUT_EXEC(_SLOT)` / `VA_FADE_IN_EXEC(_SLOT)` | `00A2D7E1` (`00F1F3B8`) / `00A2D789` (`00F1F3CC`) | `CRenderCommand_ScreenFadeOut`/`In` RTTI, slot 3; length at +0x0C / +0x08 as on Steam |
| `VA_INDOOR_CMP` / `_IMM` | `00A11286` / `00A11295` | in the frame setup `00A11181` (the same 160 instructions); the same 18 bytes, once in `.text` |
| `VA_RENDER_THREAD` | `0167DE94` | `CRenderThread` RTTI, its loop (slot 1, `00A23145`) stores itself there; +0x268/+0x26C as on Steam |
| `VA_UNPACK` / `VA_UNPACK_CALL` | `007614F0` / `007618C7` | the FastLZ decompressor, byte-identical; its call in the dzip block reader `00761870` |
| `VA_LAYER_LOAD` / `_CALL` `00A5AC6C` / `00A91709` | `00AAE95E` / `00AE5D66` | the per-layer load and its one call in the bucket loader `00AE5C5A` ("Loading layer '%s'..."); **stdcall (info, ctx), `RET 8`** (Steam: info in EDI, `RET 4`); the memory guard (`door-transitions.md`) |
| `VA_SPINNER_EXEC` / `_SLOT` `009DC3F1` / `00E7C088` | `00A2D9F9` / `00F1F0C0` | `CRenderCommand_ToggleLoadingIndicator` RTTI, vtable `00F1F0B4` slot 3; show byte +0x08; the same bytes apart from the rel32; created by the same door task (ctor, `fade_out` event, dtor) and partition job (build, end) |
| `VA_ENV_ADD` / `_CALL` `00A75014` / `00A367CB` | `00AC9EED` / `00A89F59` | the area environment activation (stdcall, 9 arguments, `RET 0x24`, the same body and argument order) and its one call, in the component's activate `00A89ED3` (`__thiscall` on GOG; Steam ESI) |
| `VA_ENV_STAB` / `_CALL` `00A74FBF` / `0095DC5E` | `00AC8CDF` / `005AF54B` | Stabilize, **`__thiscall`** (ECX = the manager; Steam EAX), plain `RET`; on GOG the door finish `005AF51F` reaches it with a **tail `JMP`** at `005AF54B`, so the hook keeps the `E9` (`patch_rel32`); the 8 Stabilize sites map one to one, and the door's is the only `JMP` |
| `VA_CAPTURE_PENDING` / `_QUEUED` | `01169160` / `01169161` | cleared by the save writer `004E721B` (`88 1D` + address, relocated); never read on GOG (`save-previews.md`, "GOG") |
| `VA_SAVELIST` / `_CALL_A` / `_CALL_B` | `005072D1` / `00507969` / `0050794A` | the only code with `L"*.TheWitcherSave"`; **`__thiscall`, ECX = panel, plain `RET`**; its calls from the startup and the `RefreshSaveData` native (`save-bloat.md`, "GOG") |
| `VA_MEM_STREAM` / `_SERIALIZE` / `_SIZE` | `00F1A248` / `00B138EA` / `00B138E3` | `CMemoryFileReader` RTTI, slots 1 and 3, the same code |
| `VA_DZIP_READER` / `_CALL` / `_VT` `00A4E42E` / `00A980BC` / `00E8BA80` | `00AA1DFC` / `00AEBF37` / `00F2E990` | the `CDZipFileReader` constructor (**this in ESI**, Steam EDI), its one call in the save open `00AEBEDA`, its vtable; GOG's `fast_list` hooks this call |

The world, job, layer group, layer info and partition node offsets the door code reads are all
the same on GOG (`world+0x30C/+0x310/+0x50/+0x358`, `game+0xE4/+0xE8`, job `+0x0C..+0x2C`,
layer info `+0x38/+0x5C/+0x68/+0x78`, node `+0x04/+0x28/+0x60..+0x78`, door `+0x228`, task
`+0x14`, entity position `+0xA0`), each found in the same instruction of the matching
function. GOG also has a second FastLZ caller (`0054D84B`, beside the Arena server code, no
static reference); the load trace does not count it.

Seen in game on GOG on 2026-11-04 (`docs/validation/gog-doors-2026-11-04.md`):
- Every door and partition hook went in.
- 64 interiors were preloaded with Flotsam.
- House doors in Flotsam and Binduga went without a fade. Each job ran in 17-38 ms with the GC
  skipped, and the `fade_out` handler took the DLL's event (`state 2`).
- The occluders were loaded through the request function once Geralt was out.
- The Flotsam-Binduga gate kept its 0.25 s fade.
- The load trace's loading-screen and door windows were logged.

Seen in game on GOG on 2026-11-04 (`docs/validation/gog-presets-2026-11-04.md`): the settings
object found at its static address plus the delta, LOD enforced, both distance hooks installed,
non-zero `baseline_reject`/`admitted`, Ctrl+F10 through all four levels.

## Open

* On GOG the log usually ends at the CRT's shutdown filter change without `unloaded` (three of
  four GOG runs on 2026-11-04; every Steam run ends with `unloaded`). Explained on 2026-11-04
  (`docs/validation/gog-shutdown-2026-11-04.md`): the game's CRT gives the exit to the .NET
  runtime's `CorExitProcess` whenever `mscoree.dll` is in the process (`__crtCorExitProcess`, GOG
  `00743F1F`, Steam `00672346`). On GOG it is, most likely brought by Galaxy's overlay:
  `mscoreei.dll` joins the filter chain as the overlay's input proxy starts, which is
  suggestive but not proof. The runtime then ends the process without the DLL detach. The
  first GOG run, which only visited the menu, did write `unloaded`. Either .NET was not loaded
  yet, or its `CorExitProcess` returned; the trace now logs that case too. A diagnostic run
  logged no `ExitProcess` or `TerminateProcess` from the exe or `Galaxy.dll` and no detach at all.
  The Steam process has no .NET runtime, so its CRT calls `ExitProcess`. Since then the exe's
  `GetProcAddress` hands out a wrapper for `CorExitProcess` that logs the end first
  (`[diagnostics] exit_trace=1`, 0 = off). On GOG the
  log now ends with `exit: CorExitProcess(0) from witcher2.EXE+0x343F48` (the CRT's `call eax`,
  `docs/validation/gog-doors-2026-11-04.md`). The game itself closes normally either way.

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
   (the config writer, the skip-intro builder and stop, the partition request, the CName
   lookup, the save list builder and Stabilize are `__thiscall` on GOG; the partition run takes
   its job in EDI, the save reader its `this` in ESI). A call can also become a tail `JMP`
   (the door's Stabilize), which needs the opcode kept when it is redirected. A float can be
   loaded by another instruction too (the door fades: `fld` on GOG, `movss` on Steam). The
   first bytes usually tell the conventions apart, so check them per build before calling.
5. When a feature group has all its addresses, `build_gate` lets it run; test it in game on GOG.

## Still to do

Every feature of 0.10.0 now has its GOG addresses. The 11 addresses without a GOG value
(`docs/addresses.md`) are Steam's storage provider and the image registry the Steam save list
uses. GOG has neither in that path.

Not yet seen in game on GOG:
- the preview fix's late write (the game won the race on every save so far);
- the memory guard skipping a layer.

Seen on GOG on 2026-11-04 (native D3D9, 4K):
- `door_light_blend` and the hidden spinner at the houses and the inn;
- a door left to the game for memory (930 MB free at the inn's harbour door). That run is where
  such a door was found to need the game's own timing (`door-transitions.md`, "Memory").

## `interiors=2` crash

Not seen with 0.10.0 yet. The GOG exe has the LAA flag, so preloading interiors has the same
4 GB address space as on Steam. There are two likely causes:

- **Running out of memory with native Direct3D 9.** Most GOG players run without DXVK. Native D3D9
  keeps its managed textures inside the 32-bit process. On the owner's Steam install, with DXVK
  turned off, at 4K and the extreme preset (2026-11-04, 15:18):
  - loading a save made inside the inn with the interiors and the street preloaded left 2807 MB
    private, with 572 MB the largest free block;
  - two seconds later the game read through a NULL that its allocator had returned, at 3405 MB.
    The minidump could not be written (`0x80070008`).

  The same save took 1072 MB at the menu with DXVK and 1738 MB without it. The memory guard
  (`[loading] preload_memory_mb`, `door_memory_mb`; `door-transitions.md`, "Memory") holds the
  preload to the free address space, also on GOG. Even so, at 4K with native D3D9 the inn load ran
  out with nothing preloaded, so DXVK is the advice there.
- **Calling the GOG functions with Steam's conventions.** This applies to a port that only moves
  the Steam addresses. Two of the functions the interiors call changed convention on GOG:
  - The partition run takes its job in EDI. A stdcall hook passes the job on the stack, so the
    game runs a job from whatever EDI holds.
  - The partition request is `__thiscall`. A call with the game in EAX leaves ECX undefined.

The player's log and crash dump would tell which.
