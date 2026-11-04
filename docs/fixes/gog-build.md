# The GOG build

GOG `3.5.0.26g`, looked at on 2026-11-04 after a player ported the mod to it on their own
(`interiors=2` crashes for them, `interiors=1` works). The mod does not support this build yet.

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

## `interiors=2` crash

Not explained yet. LAA is ruled out: the GOG exe has the flag, so preloading interiors has the
same 4 GB as on Steam. The player's log and crash dump would show more.
