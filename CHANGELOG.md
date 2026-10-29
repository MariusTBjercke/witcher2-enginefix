# Changelog

## 0.2.0

- Interior maps: the map screen (M) no longer shows a black local map with one huge pin in temples, inns, Loredo's house and 22 other interiors. The game's `globals\map_scales.csv` has no big map for these areas, so the panel loaded an empty image name. The zip now ships a filled-in `map_scales.csv` and 2048x2048 upscales of each area's own minimap as loose files under `CookedPC\globals\`. Upscaled because the map panel's zoom runs backwards on images smaller than the panel. Data only; the DLL is unchanged apart from its version. Generated from the game's own `pack0.dzip` by `tools\build-interior-maps.py`. See `docs/fixes/interior-maps.md`.

## 0.1.0

First public release, for The Witcher 2: Assassins of Kings - Enhanced Edition 3.5.0.26 (Steam).

- Save previews: new saves (manual saves and QuickSave) get their screenshot in the Load menu again instead of a purple/blue gradient. The save tick (`009B63DE`) queues an in-memory 640x360 capture and saves in the same frame; the capture finishes about a frame later, after `009B7149` already found no image. The DLL keeps that finished BMP and writes it as `<save>_640x360.bmp` through the game's own Steam storage interface. If the game wrote the image itself, nothing is added. `[saves] fix_thumbnails=0` turns it off. The Load menu also reloads previews that were rewritten during the session (QuickSave, reused AutoSave slots); it kept one picture per file name until a restart (`00B28657`), so a new QuickSave showed the previous one. Existing saves keep their missing previews.
- Graphics presets: Ctrl+F10 cycles baseline / high / ultra / extreme, each setting mesh LOD distance, foliage distance, the mesh visibility cutoff and the mesh-shadow distance together, with an on-screen readout. `[presets] default` picks the startup level (extreme).
- Mesh visibility and mesh-shadow distance: two fingerprinted renderer hooks (RVA `0x005BB09F`, `0x005BB465`) scale the game's hard cutoffs; they install only when the exact game code is found.
- LOD values held: `MeshDistanceScale` and `FoliageDistanceScale` stay at the preset values when the options menu rewrites them, and the gameplay LOD downscale flags are kept off.
- Ctrl/Alt+F9/F10 no longer reach the game, so the mod's hotkeys never trigger a quick load; plain F9 is unchanged.
- Diagnostics: `[diagnostics] save_trace=1` logs the save-preview path (Steam writes, screenshot encoding, preview file calls).
