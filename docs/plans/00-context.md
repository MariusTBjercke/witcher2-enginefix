# Context

## Goal

Make The Witcher 2 (Steam 3.5.0.26) behave like a modern, high-resolution game on the
owner's hardware — draw distance, shadows, characters, pacing — without touching
`witcher2.exe` on disk and without changing content. Secondary: quality-of-life UI
additions (Continue / exit from the pause menu), currently **deferred**.

## Hardware

RTX 5070 Ti (16 GB), Intel Core Ultra 7 265KF, 64 GB RAM, 3840x2160 @ 74 Hz.

## Environment that already exists (installed 2026-10-28, verified)

**Rendering**
* DXVK v3.1.1 (x86) at `<game>\bin\d3d9.dll`, tuned `dxvk.conf` in the same folder
  (`d3d9.maxFrameRate=72`, `maxFrameLatency=1`, `samplerAnisotropy=16`,
  `maxAvailableMemory=4096`).
* `Documents\Witcher 2\Config\User.ini` — read-only, hand tuned:
  `TextureMemoryBudget=1024`, `TextureTimeBudget=10`, `MaxTextureSize=4096`,
  `AllowSharpen=0`, shadows `2048/2048`, `ShadowedLights=4`,
  `MeshDistanceScale=4.0`, `FoliageDistanceScale=2.0`,
  `MeshLodGameplayDownscale=0`, `CharacterLodGameplayDownscale=0`, `UberSampling=0`.
* `Documents\Witcher 2\Config\Rendering.ini` — read-only, `MaxResidentMipMap=11` and
  `MaxSize=4096` on the seven diffuse groups.
* `witcher2.exe` is already Large-Address-Aware and has **ASLR disabled**.

**Mods in `<game>\CookedPC`** (Nexus, installed and hash-verified)
* Project Mersey Fix Pack 1.3 — replaces `base_scripts.dzip`, adds `ZZ_UPatch.dzip`,
  `.w2strings`
* Blick's Cutscene Fixes 1.2 — `zzzz_fixed_cutscenes.dzip`
* AI Upscale Textures X2 (characters / environment / weapons) — ~1 900 files
* Extreme Vegetation Rendering Distance — 300 meshes
* Witcher 2 HD UI AI Upscale — 3 899 files under `globals\gui`
* Intro logo removal — `globals\video_start.csv`

Backups and a precise revert path: `<game>\_backup_premods\`
(`added_files.txt`, `restore-original.ps1`). Steam's *Verify integrity* restores the
one replaced file that has no local backup (`globals\video_start.csv`).

**This DLL** — `<game>\bin\dinput8.dll` + `witcher2_enginefix.ini`, log at
`<game>\bin\witcher2_enginefix.log`.

## What the DLL does today

| Feature | State |
|---|---|
| Proxy for system `dinput8.dll` (all 6 exports forwarded) | works |
| Finds settings singleton `DAT_02580e28` and enforces `MeshDistanceScale` / `FoliageDistanceScale` | works, verified in game |
| Forces both `*LodGameplayDownscale` flags to 0 | works |
| On-screen readout on each change (topmost layered window) | works, verified in game |
| Combined startup/live graphics presets | implemented; Ctrl+F10 cycles baseline/high/ultra/extreme; ini default=extreme; awaiting human interface check |
| Separate mesh / foliage / visibility / shadow hotkeys | retained with [presets] enabled=0; owner confirmed previous four-control build in game |
| Keyboard filter that clears F9/F10 in the polled state while Ctrl/Alt is held | implemented, keyboard-device only, verified by `tools\inputprobe` that it does not crash |
| Visibility / mesh shadow-caster range hooks | owner confirms substantial improvement; reused by presets; 27 native, 9 legacy and 21 preset checks pass |

## Source of truth

* `docs/fixes/save-previews.md` — current thumbnail investigation, attempted methods/results, retired debugger workflow, backups and remaining uncertainty
* `docs/research.md` — binary, packing, config system, settings object, pitfalls
* `docs/fixes/graphics-presets.md` — combined preset config, memory mechanism, compatibility, verification and rollback
* `docs/fixes/render-distance-experiment.md` — underlying code hooks, exact bytes, original controls and verification
* `docs/fixes/npc-streaming-sun-shadows.md` — remaining NPC streaming candidates, saved radii, three fixed sun-shadow cascades and 1024 atlas allocation
* `docs/fixes/lod-distance.md` — what was tried, with observations
* `docs/fixes/modern-hardware-options.md` — remaining candidate improvements
* `docs/fixes/roadmap-shadows-characters-menus.md` — the three larger asks
* `AGENTS.md` — paths, build/verify commands, rules, time-wasting traps
