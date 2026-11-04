# The GOG build

GOG `3.5.0.26g`, looked at on 2026-11-04 after a player ported the mod to it on their own
(`interiors=2` crashes for them, `interiors=1` works). The mod does not support this build yet;
0.9.0 is Steam only, and the port is planned for a later version.

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

* **Every address differs.** The DLL holds about 90 fixed addresses and each one checks its
  code first, so on GOG they skip themselves. The fixes must be found again in the GOG binary.
* **ASLR.** The DLL assumes base `0x00400000`. With `DYNAMIC_BASE` the image can load elsewhere,
  so addresses need the real base added.
* **Saves.** The thumbnail fix and the fast save list go through `ISteamRemoteStorage`. GOG
  saves differently, so those two need their own work.
* **Script patches, maps, icons.** These are data and should carry over once the file hooks
  have the right import slots.

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

Two Steam addresses are not `#define`s and would be missed by the table: `0x00AA7E18`
(`VA_PART_REQUEST`) inside a naked stub in `enginefix.c`, and `0x00E30444` (a float the door
fade code reads) in the door fade patch.

## What a port changes in the DLL

* Build detection at start, a per-build address table (Steam = today's defines, GOG filled in as
  each site is proven, 0 = not ported: that feature logs it and stays off), and the relocation
  delta added to every address.
* Import slots found by name in the exe's import directory instead of fixed slots; that alone
  makes the script patches (they only hook file APIs), the cpu32 guard and the crash capture
  usable on GOG. The input fixes hook DirectInput objects and do not depend on the build. The
  interior map files are data.
* `window_mode` adds menu rows that call the window patches, so that script feature must follow
  the WF_ sites.
* Saves: the thumbnail fix and the fast save list go through `ISteamRemoteStorage`; GOG has its
  own save path, which needs its own research.

## `interiors=2` crash

Not explained yet. LAA is ruled out: the GOG exe has the flag, so preloading interiors has the
same 4 GB as on Steam. The player's log and crash dump would show more.
