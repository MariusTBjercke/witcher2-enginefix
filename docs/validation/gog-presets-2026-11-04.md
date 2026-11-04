# LOD, presets and distance hooks on GOG (0.10.0), 2026-11-04

Owner's test on GOG 3.5.0.26g (vanilla scripts plus A Better UI, the shipped ini: presets on,
`default=extreme`) after porting `RVA_SETTINGS`, `EF_RANGE_RVA` and `EF_SHADOW_RVA`
(`docs/fixes/gog-build.md`, "Ported addresses"). Load a save, wait for the OSD, Ctrl+F10 through
the levels twice, walk for a while, quit from the main menu.

```text
witcher2_enginefix 0.10.0 loaded (image base 0x00D30000)
build: GOG 3.5.0.26g (TimeDateStamp 644689F9, .text 008FC18D), delta 0x00930000 from 0x00400000; import slots 23/24 found by name, not imported: SteamRemoteStorage
build: 10 feature group(s) not ported to GOG yet (listed above); the rest runs as configured
settings object 0x02F1AF08 found: mesh=4.000 foliage=2.000
osd: Graphics: extreme  (LOD only)
lod enforced: mesh=8.000 foliage=5.000
distance: hooks installed at RVA 0060C120 / 0060C4F6; visibility=3.00x shadow=3.00x
osd: Graphics: extreme  (4/4)
distance stats: mesh=831553 shadow=354109 baseline_reject=173379 admitted=136582 (3.00x/3.00x)
lod enforced: mesh=1.000 foliage=1.000
preset: baseline mesh=1.00 foliage=1.00 visibility=1.00x shadow=1.00x; distance_active=1
preset: high mesh=2.00 foliage=2.00 visibility=1.50x shadow=1.50x; distance_active=1
preset: ultra mesh=4.00 foliage=3.00 visibility=2.00x shadow=2.00x; distance_active=1
preset: extreme mesh=8.00 foliage=5.00 visibility=3.00x shadow=3.00x; distance_active=1
distance stats: mesh=1129222 shadow=402728 baseline_reject=294684 admitted=217973 (3.00x/3.00x)
```

* The settings object sits at `0x025EAF08` + the delta, as the static proof said; the values
  read before enforcing are the shared User.ini's (`MeshDistanceScale` 4, `FoliageDistanceScale`
  2).
* Both distance sites took the GOG bytes and bridges. `baseline_reject` and `admitted` are
  non-zero, so `ef_range_bridge_gog` compares with the stack distance as intended (with Steam's
  bridge they would have stayed 0 against GOG's zero XMM6).
* Ctrl+F10 cycled all four levels twice, each with its LOD write. The level only comes from
  `[presets] default` on GOG, because the Options row (`window_mode`) is not ported.
* No dump.

## Shutdown without `unloaded` (open)

As in run 2 of `gog-phase0-2026-11-04.md`, the log ends right after the CRT's filter change
during shutdown (`crash: filter armed, chaining to 0x01077DE4 (witcher2.EXE+0x347DE4)`), with no
`unloaded` line, although this build only tries the log lock at process exit. Both GOG runs with
a save loaded ended like this; the GOG run that stayed in the menu and every Steam run wrote
`unloaded`. No dump, no Windows error event, and Galaxy logged a normal close. So the game
exits as asked, but either the DLL's detach is not reached after a play session on GOG or the
log lock is held at that moment. Not explained yet.
