# Combined graphics presets

Added 2026-10-28 at the owner's request, following Witcher 1 enginefix's pattern:
the ini chooses a startup level and one hotkey cycles levels for the current
session. Further NPC streaming and sun-shadow changes are deferred by the owner.

## Use

`bin/witcher2_enginefix.ini`:

```ini
[presets]
enabled=1
default=extreme
cycle=Ctrl+F10
```

| Preset | Mesh LOD | Foliage | Mesh visibility | Mesh shadow gate |
|---|---:|---:|---:|---:|
| baseline | 1 | 1 | 1 | 1 |
| high | 2 | 2 | 1.5 | 1.5 |
| ultra | 4 | 3 | 2 | 2 |
| extreme | 8 | 5 | 3 | 3 |

`Ctrl+F10` advances to the next row and wraps from extreme to baseline. An OSD
shows the name and position. The game must be foreground; only a new physical
function-key press triggers a change. Holding the key, changing modifiers while
holding it, or returning focus with it held cannot advance the level.

`extreme` is the startup default and uses the top factors of the existing tested
controls. These names do not represent measured performance targets. The owner
confirmed a large improvement with the individual distance controls; the new
combined interface still needs the normal human game startup/OSD check.

Override each row in `[preset.baseline]`, `[preset.high]`, `[preset.ultra]` or
`[preset.extreme]`, using `mesh_scale`, `foliage_scale`, `visibility_scale` and
`shadow_scale`. LOD factors must be finite, greater than 0 and less than 100;
distance factors must be finite and in [1,4]. Missing or invalid fields use that
row's built-in defaults. Invalid preset names use extreme. Names ignore case.
The supported preset bindings are F9/F10 with Ctrl and/or Alt, optionally Shift;
other bindings fall back to Ctrl+F10 because only these keys have input filtering.

Hotkeys never write an ini file or a save. Ini edits are read at startup.

### Options > Graphics > Draw distance (0.7.1)

The same four levels are a selector row under Display mode (default, high, ultra, extreme,
in the game's language; "default" is `baseline`, the game's own values). It belongs to the
`[scriptpatches] window_mode` script feature and the window section's config-writer hook
(`docs/fixes/borderless-window.md`):

* the row writes `[Rendering] EngineFixDrawDistance` (0-3) with the game's own
  `WriteConfigParamFloat`; the writer hook passes it to the worker, which selects the preset
  like Ctrl+F10 but shows no OSD (the row shows the choice) and logs
  `preset: <name> ... (options menu)`;
* the other way round, whenever the level changes elsewhere (start, Ctrl+F10) the viewport tick
  hook writes the live level into that key through the writer's trampoline, on the game's main
  thread (checked against the thread `DllMain` ran on), so the row always opens on the level in
  use. The trampoline skips the hook, so that write is not taken for a menu choice;
* the engine saves the key in `Documents\Witcher 2\Config\User.ini` like its other options.
  At start the stored level, when present, wins over `default=` (log
  `presets: startup=<name> (options menu)`). With a read-only User.ini (the owner's machine)
  nothing is stored and `default=` applies at every start.

Needs `[presets] enabled=1` and `[window] enabled=1` (the writer and viewport hooks); otherwise
the row is shown but has no effect. The script cache layout is `OV_CACHE_LAYOUT` 5.

Verified 2026-11-02. Offline: `build.bat` clean; `sptest test` 0 failures; `sptest build` puts
the selector after Display mode in `FillOptionsGraphics` and the `9202` handler in
`InputOptionsGraphics`; `check_texts.py` ok for the nine window texts in all twelve languages;
`windowprobe` and `distanceprobe` 0 failures. In game (owner, 0.7.1): the scripts compiled, the
row showed the live level, menu choices changed the distances at once without the OSD, and the
row followed Ctrl+F10. `[hotkeys] enabled=0` disables live switching while
retaining the startup selection. `[general] enabled=0` stops the runtime worker.

## What changes in memory

1. The DLL reads its own ini at startup. In preset mode it selects all four
   factors together and enables LOD enforcement and the existing distance hooks.
   The individual `[lod]` scale/enforce and `[distance]` scale/enabled settings
   apply in legacy mode. The downscale-flag option remains independent.
2. Mesh and foliage are settings-object floats at `+0x6C` and `+0x68`, written
   every ~120 ms and immediately when a preset changes. This holds them through
   an options-menu rewrite. This is a live settings update, with no game-ini edit.
3. Visibility and shadow distance use the already implemented, fingerprinted
   instruction hooks at RVA `0x005BB09F` and `0x005BB465`. Hooks install once, after
   settings initialization. A preset change updates the DLL's squared factors,
   which the same bridge code reads on each evaluation. It does not install or
   rewrite another instruction patch on every press.
4. Before distance hooks succeed, the OSD explicitly says `LOD only`. Successful
   installation replaces that with the ordinary preset name/position. Refused
   installation remains fail-closed and is recorded in the runtime log.

Baseline resets only these four factors to 1; it does not uninstall hooks or
disable gameplay downscale enforcement. Presets do not change UberSampling,
NPC streaming, sun cascade lengths/resolution, DXVK, save previews, the game exe
on disk, or `Documents/Witcher 2/Config/User.ini` and `Rendering.ini`.

## Compatibility, tests and rollback

Set `[presets] enabled=0` and restart to restore Ctrl+F9 mesh, Ctrl+F10 foliage,
Ctrl+Shift+F9 visibility and Ctrl+Shift+F10 shadows. All four separate hotkeys
are inactive in preset mode. Existing ini files without `[presets]` retain their
previous behavior. To omit distance code patches entirely, disable both presets
and `[distance] enabled`, then restart.

The DirectInput COM hooks, key suppression and `suppress_mod_keys` setting were
not changed. No additional native engine addresses or binary hooks were added.

`tools/distanceprobe/build-and-run.bat` exercises the real DLL config/dispatch:
21 preset checks cover startup, all factors/squares, cycle/wrap, physical edges,
focus, suppressed legacy actions, custom rows, invalid fields/names/bindings,
old-ini behavior, the shipped ini and no ini persistence. Existing 27 native hook checks and 9
legacy hotkey checks remain in the same run. Preset expectations failed before
implementation (17 failures); all 57 checks pass with the implementation.

Build with `build.bat`. The pre-preset DLL, source and both ini variants are saved
under `build/before-presets/`. Restore `dinput8.dll` and `installed.ini` (renaming
the latter to `witcher2_enginefix.ini`) into the game bin, while the game is closed,
to return exactly to the previous installed build.

Installation and independent review are recorded in
[validation](../validation/graphics-presets-2026-10-28.md). Human startup/OSD
verification of the combined interface remains pending.

Deferred research with verified addresses, evidence and remaining uncertainty:
[NPC streaming / sun shadows](npc-streaming-sun-shadows.md). Existing hook bytes,
ABI and scope: [render distance experiment](render-distance-experiment.md).
