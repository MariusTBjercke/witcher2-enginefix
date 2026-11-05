# Layer storage leak fix, first run (Steam, 2026-11-06 00:18)

Build: 0.11.0 plus `[stability] layer_storage_leak` (`docs/fixes/layer-storage-leak.md`), DLL only;
the installed ini predates the key, so the default (1) applied. Steam 3.5.0.26, DXVK. Owner: a save
in Flotsam loaded from the main menu, then saved and loaded again four times (five loads in all).
No crash; the game kept running.

## Log

```
00:18:05.596 stability: layer storage leak fix: array clear hooked, single removal hooked
00:18:21.115 savelist: after the switch private=1374 MB ... free=2098 largest_free=2012 MB   (load 1, from the menu)
00:18:47.824 stability: layer storage: 49 saved entity state(s) freed so far (0 MB) ...
00:18:52.208 savelist: after the switch private=1437 MB ... free=2021 largest_free=1677 MB   (load 2)
00:19:08.845 savelist: after the switch private=1450 MB ... free=2003 largest_free=1612 MB   (load 3)
00:19:17.857 stability: layer storage: 13145 saved entity state(s) freed so far (40 MB) ...
00:19:30.537 savelist: after the switch private=1463 MB ... free=2002 largest_free=1731 MB   (load 4)
00:19:47.892 stability: layer storage: 19719 saved entity state(s) freed so far (61 MB) ...
00:19:56.383 savelist: after the switch private=1489 MB ... free=1975 largest_free=1687 MB   (load 5)
00:20:17.926 stability: layer storage: 26292 saved entity state(s) freed so far (81 MB) ...
```

The first 49 were single removals (spawned entities) before the first reload. Each later load
freed ~6 570 states, ~20 MB, the store's full content each time (26 292 - 49 over four reloads).
Every preload stayed complete (62 of 62) and every door afterwards kept the outdoors.

## The process afterwards

Same read-only scans as the night before (`VirtualQueryEx`/`ReadProcessMemory` from outside):

| | 2026-11-05 23:54, without the fix | 2026-11-06 00:21, with it |
|---|---|---|
| Session | ~5 h, many loads | 5 loads |
| `CGameDataStorage` objects | 132 774 | 6 573 |
| pointed at by nothing | 122 699 (370 MB) | **0** |
| 64 KB pages holding state (`BLCK`) | 6 408 | 321 |
| Havok animation pages | 3 304 | 3 268 |

6 573 matches the number freed per reload: the store holds one copy of the world's states and
the old copy now goes. The Havok pages are the same count after 5 loads as after 5 hours, so they
are a cache that fills early, not growth.

## Is anything else leaking per load? (00:26-00:27, same session)

Five more loads of the same quicksave, ~16 s apart, no saving. The `savelist: before the switch`
line is taken with the old world unloaded (`interiors: before the switch - ... 0 loaded`), so it
is the best baseline:

| | Load 2 | 3 | 4 | 5 | (6 min of play) | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Private, empty world (MB) | 1 391 | 1 403 | 1 394 | 1 423 | | **1 392** | 1 417 | 1 424 | 1 430 | 1 435 |

Back-to-back loads raise the empty-world baseline by ~5-7 MB each, but a few minutes of normal
play brought it back to where it started (1 392 MB). So this part is released later, not leaked.
Per-class object counts (RTTI census before load 6 and after load 10) moved by a few hundred
objects at most (`DataBuffer` +712, Havok motions +201, `CPersistentEntityHandleData` +159), and
the large allocations (16-22 MB) are freed and made again on every load. Loading with the fix in
leaves no growth that this run could show.

## Not covered

* The rest of the growth on 2026-11-05 (~1.1 GB over five hours, including +415 MB in two
  minutes in Binduga with no load) came from playing, not from loads, and was not reproduced here.
  Finding it needs a census over a long normal session.
* NPC state across loads (an NPC moved, killed or looted before the save) was not checked
  specifically. GOG not run.
