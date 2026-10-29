# Interior maps: black local map in temples, inns and houses

Status: shipped in 0.2.0. Data-only fix (loose files under `CookedPC\globals\`);
no DLL code involved. Target: Steam Enhanced Edition 3.5.0.26.

## Symptom

In some interiors (seen first in the area named *Temple*) the map screen (M) shows a
black local map with one huge pin in the top-left corner. It cannot be dragged.
Zooming out to the continent map works; zooming back in returns to black. Save/reload
and a game restart do not help; it clears when the player leaves the area.

## Cause

The local map is a 2D DDS image, not a render of the level.

1. A `CMapArea` trigger (`game\gameplay\maparea.ws`) sets the player's current map id
   on entry and restores the act map on exit.
2. `CGuiMapBig.FillMap` (`game\gui\guimap.ws`) calls `theHud.GetMapInfo` for that id and
   sets `LoNavSheet = "img://globals/gui/maps/" + mapFile + ".dds"` on the Scaleform
   panel `ui_nav.swf`.
3. `GetMapInfo` reads `globals\map_scales.csv` (string `globals\map_scales.csv` at file
   offset `0x9C345C` in the unpacked exe; columns `Map Index;Map name;Minimap name;MinX;MaxX;MinY;MaxY;FOW file;FOW grid`).

In the shipped csv, 26 rows have an empty *Map name*; 25 of them have a minimap. For
these the panel loads `img://globals/gui/maps/.dds`, gets no image, and the
`fxImage` has zero size: black, no dragging, pins scaled against nothing.

| Id | Minimap | Area (from name) |
|---:|---|---|
| 4, 50, 51 | `swiatyniaminimapa_512x512`, `swiatynia1/2_1024x1024` | temple (*świątynia*) |
| 5, 6 | `samotniaminimapapoziaom1/2_256x256` | hermitage |
| 10-13 | `minimapadomloredo*_512x512` | Loredo's house |
| 18 | `elfiespaminimapa_512x512` | elven bath |
| 20-22 | `karczmaminima*_512x512` | inn |
| 32 | `houseofhangemenminimapa_512x512` | House of the Hanged Man |
| 38, 39, 40 | `detmoldtowerminimapa`, `fillipahouseminimapa`, `wierzaobl` | towers / houses |
| 47, 48, 49 | `tower0/1minimapa`, `korytarzzcmentarza_256x256` | towers, cemetery passage |
| 52-54, 58, 65 | `28domkow*`, `domkiflotsam`, `domekobokvergen`, `minimapaarena` | houses, arena |

Row 26 has neither map nor minimap and is left alone.

The scripts are not touched by any installed mod: `maparea.ws` and `guimap.ws` in the
modded `base_scripts.dzip` are byte-identical to the original.

## Why the maps are upscaled

Pointing *Map name* at the minimap alone makes the map appear, but zoom runs backwards.
`ui_nav.swf` (`scrollTo`, identical ActionScript in the original, A Better UI and the HD
UI upscale build) computes

```
fit  = max(boxSizeX / fxImage._width, boxSizeY / fxImage._height)
zoom = fit + slider * (1 - fit)
```

For a 2048 image `fit` < 1 and the slider zooms in; for a 512 or 1024 image `fit` > 1
and the same slider shrinks the picture. A 2048x2048 copy restores the normal direction.

## The fix

`tools\build-interior-maps.py <pack0.dzip>` extracts the csv and the 25 minimaps from the
game's own archive, upscales each minimap to 2048x2048 (Lanczos, DXT1, no mipmaps, same
header layout as `worldmap_2048x2048.dds`), names it `<minimap base>_2048x2048.dds`
(no name exists in `pack0.dzip`), and fills the empty *Map name* fields. All other csv
bytes, including CDPR's undecodable junk rows at the end, are kept. The release zip
carries the result under `CookedPC\globals\` (loose files override the archives, as
the existing `globals\video_start.csv` mods do).

Pin positions come from the csv bounds; the owner saw them in the right place with both
the minimap-size and the 2048 images.

## Not changed

The map opens at the image centre at minimum zoom (`scrollTo(0)` after load), not on the
player. With a wide panel and square images the picture fills the width and crops
top and bottom, so northern or southern pins need scrolling. Centring on Geralt would
need a patch to `ui_nav.swf` or `guimap.ws`, both of which other mods replace.

## Verification

Owner, 2026-10-29, in the *Temple* area: csv-only variant showed the map with reversed
zoom; the 2048 variant shows the map, pins in place, zoom in the normal direction.
Generator output is byte-identical to the files tested in game.

## Turning it off

Delete `CookedPC\globals\map_scales.csv` and the `*_2048x2048.dds` files listed in this
document from `CookedPC\globals\gui\maps\`. Another mod that ships its own
`globals\map_scales.csv` conflicts; the last one copied wins.
