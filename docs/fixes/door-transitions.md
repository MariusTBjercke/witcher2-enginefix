# Door transitions: what changes at a house door, and how to make it seamless

Status 2026-11-04: researched; option 1 and the owner-test fixes are built (0.10.0 working tree, see
"Built" at the end), not yet run in game. Target: Steam Enhanced Edition 3.5.0.26, unpacked,
and the game's own level data (`CookedPC\pack0.dzip`). Builds on
[interior-preload.md](interior-preload.md) (partitions, layers, `[loading] interiors=2`) and
[load-times.md](load-times.md) (door tasks and fades).

## Symptom

* A player (Nexus, 0.8.x, default `interiors=2`): at a door there is a visible "flash": the
  outdoors (or the room) briefly turns into a "lower quality version", and leaving an inn the
  interior wall changes colour abruptly.
* Another player: a low-resolution ground texture circle around Geralt (outdoors, not tied to
  doors).

With `interiors=2` a house door in Act 1 loads and unloads almost nothing and runs without a
fade (`instant_doors=1`), so whatever the partition switch still changes happens in plain view.
The vanilla door hid it behind ~2 s of black.

## Short answer

1. **Low-poly at doors: no.** No mesh, stand-in or LOD level changes at an Act 1 door with
   `interiors=2`. What changes is the interior's **area environment** (a layer 13 of the 14 Flotsam
   interior partitions load). It switches the whole frame to the interior look: no sun light and
   no sun shadows, a camera far plane of 26-386 m instead of 1 024 m, stronger depth of field,
   other tone mapping, colours and fog. With the street still on screen, that reads as a
   lower-quality picture. Leaving, the room gets the outdoor look for a moment.
2. **Lighting: it can be blended, and the engine already blends.** Area environments fade in and
   out over `activationTime`/`deactivationTime` in the environment manager. Two things make the
   door a hard cut: CDPR set those times to 0 for most Act 1 and Act 2 interiors (the fade hid it), and every door
   task ends in `FUN_0095db81`, which re-evaluates the area environments and then calls
   **Stabilize** (`0095DC5E`), finishing every blend at once. Skipping that one call for kept doors
   and giving interior areas a minimum blend time turns the snap into a cross-fade. Preloading the
   environment layer alone does not help (it is small, and its areas reach 3-6 m into the street).
3. **Ground circle: no setting for the texture; a sharp line ahead of Geralt is a sun-shadow seam** (see
   "Ground texture circle"). The terrain detail fades by mip level in the
   terrain pixel shader, from screen-space derivatives; depth of field can look similar. See
   "Ground texture circle".

## What a kept door still changes (Act 1, 0.8.1, `interiors=2`)

From the partition definitions (`CWorldPartitions` inside `levels\l02-port\l02-port.w2w`, parsed
offline, see "Reading the level data" below) and the door traces of runs 13 and 17:

| Kind | Changes at the door? | Evidence |
|---|---|---|
| Building shells, street, other houses | No: `keep_outdoors` keeps every outdoor layer | run 13: inn in "kept 110 of 115, unloaded 5" |
| The house's own rooms | No: preloaded with the area (`preload_interiors`) | run 13: "0 interiors added", nothing `\interior\` loaded at a door |
| Low-poly stand-ins | No (see below) | partition tree, run 13 "skipped ... low-poly" |
| `fake` rooms | No: dropped for the whole area by preload | interior-preload.md, run 12 |
| Occluders | Yes, but deferred until Geralt is 5 m past the door (0.8.1) | runs 16-17 |
| Ambient sound, music, reverb | Yes (`sounds\...` layers, partition `soundAmbient`/`musicCue`) | `FUN_00aa95fa`, partition tree |
| Townspeople (`comm*`, `community`) | Yes (the inn's basement communities) | run 13 |
| Location type to the renderer | Not in Act 1: no `l02-port` partition is `LT_INDOOR` (`LT_CAVE` is the only value written; the rest are the default). The prologue's are; `indoor_outdoors` covers them | partition tree, run 15 |
| **Area environment (lighting, post-processing, camera far plane)** | **Yes: 13 of the 14 Flotsam interior partitions load `environments\interior_city`** | below |

### Low-poly stand-ins are not what changes

The only stand-in layers in Act 1 are under `levels\l02-port\city_building_lowpoly\`:

| Layer | Content | Loaded by |
|---|---|---|
| `lod.w2l` | 11 `port_lods_exteriors_*.w2mesh`: Flotsam's buildings as seen from outside the walls | `village-city-forest` (Binduga) and `loredo_residence` |
| `malena_craftsmen\malena_craftsmen.w2l` | 2 more exterior LOD meshes | not referenced by any partition |
| `ships_lowpoly\ships_lowpoly.w2l` | `ship01/02/04_low.w2mesh` | `inn_interior`, `village-city-forest`, `loredo_residence` |
| `roche_lowpoly\roche_lowpoly.w2l` | empty layer (78 bytes) | `inn_interior`, `loredo_residence` |

The per-building `...\buildings\<name>\lod\lod.w2l` layers in Flotsam are all empty (78 bytes).
Binduga's house partitions inherit `city_building_lowpoly` from their parent, so a Binduga door
never swaps it; the inn's ship stand-ins are dropped from the load list by `keep_outdoors`
(`LC_LOWPOLY`, since 0.8.0). So no mesh is replaced by a lower-detail one at any Act 1 door,
and no stand-in is drawn next to the real object. Mesh LOD and the draw distance presets do not
depend on the partition. The partition job's "Flushing..." stages (`FUN_00b0cea0`,
`FUN_00b05b13`, `FUN_00b05a52` in `FUN_00a91952`) wait for the async loader and walk the
layer tree; nothing there evicts textures or meshes.

## The environment switch is what the player sees

### Interiors carry their own area environment

`levels\l02-port\environments\interior_city\interior.w2l` (575 KB) is one layer with 14
entities, each holding one `CAreaEnvironmentComponent`: an area (a polygon `worldPoints` plus a
height range, `boundingBox`) with `priority`, `activationTime`, `deactivationTime` and a full
`CAreaEnvironmentParams` set (`m_globalLight`, `m_toneMapping`, `m_finalColorBalance`, `m_ssao`,
`m_bloom`, `m_colorModOpaque`, `m_globalFog`, `m_sky`, `m_depthOfField`, `m_shadows`,
`m_cameraPlanes`, ...). 13 of Flotsam's 14 interior partitions (`inn_interior`,
`shealas_quarter`, `03_clerk_marketplace`, the other craftsmen/marketplace houses,
`06_custom_house_pier`) list the group `environments\interior_city`; the `Flotsam` partition
does not, and neither does `04_einar_gausel_craftsmen` (Einar Gausel's house keeps the outdoor
environment inside). The outdoor environments (`environments\global_environments.w2l`, the forest, the
landing) are in the `Root` partition and always loaded.

So an interior's area environment exists only while its partition is current; the door task
turns it on and off (next sections).

| Area (entity) | Priority | Activation / deactivation time | Sun (`globalLightActivated`) | Camera far plane |
|---|---|---|---|---|
| `global_environment` (outdoors, `Root`) | 1 | 0.1 / 200 s | not set (class default) | 1 024 m |
| `house_01_inn` (inn ground floor) | 15 | **0 / 0** | **0 (off)** | **386 m** |
| `house_01_inn2` (inn upper floor) | 15 | 0 / 0 | 0 | 26 m |
| `house_01_inn_basement` | 16 | 1 / 1 s (class default) | 0 | 38 m |
| `taves_craftsmen`, `guards_craftsmen` | 15 | 0 / 0 | 0 | 26 m |
| `clerk_marketplace_0`, `barracks_marketplace_0` | 15 | 0 / 0 | not set | 69 m |
| village houses (5) | 15 | 0 / 0 | not set | 26 m |
| `06_custom_house_pier` | 15 | 0.5 / 3.0 s | not set | 1 202 m |

The inn's parameters also move depth of field in (far focus 8 m, full blur at 68 m against 9.5 m
/ 105 m outdoors), change SSAO, tone mapping, fog and the near/far "colour mod" sharpening.

### Why it looks like a lower-quality picture

An area environment applies to the whole frame, not to the room. Once the interior's area is
active, everything on screen, including the street seen through the door or a window, is drawn
with no sun light (and so no sun shadows), the interior's tone mapping and colour balance,
stronger depth of field and a far plane of 26-386 m. The street turns flat, blurred and cut
off: the "lower quality version". The reverse happens on the way out: the room is drawn with
the outdoor sun, fog and tone mapping for a moment, so the wall "changes colour".

### How the engine turns an area environment on and off

Static analysis of the unpacked exe:

* **Component** `CAreaEnvironmentComponent` (properties registered in `FUN_00450b88`, size
  `0x1D80`): `parameters` `+0x1C0` (`CAreaEnvironmentParams`, `0x1B94` bytes), `activationTime`
  `+0x1D58`, `deactivationTime` `+0x1D5C` (floats), `priority` `+0x1D60`, `globalEnvRandChance`
  `+0x1D64`, `isGlobal` `+0x1D6C`; manager entry id `+0x1D68` (-1 = none). Constructor
  `FUN_00a36683`: both times **1.0 s** (`movss xmm0,[00DC391C]` = 1.0 at `00A366AA`, stored at
  `00A366D9`/`00A366E1`), priority 10. There is no blend-distance property.
* **Manager** `CEnvironmentManager` at `world+0x2B8` (vtable `00E8390C`), ticked every frame
  (`00B1FDF3`: `push dt; push mgr; call FUN_00a78346`). Entries: array `mgr+0x1BB0`, count
  `+0x1BB4`, stride `0x1BCC`: priority `+0x00`, activation time `+0x04`, deactivation time `+0x08`,
  a copy of the parameters `+0x0C`, blend factor `+0x1BB0`, rate `+0x1BB4`. Priority groups at
  `mgr+0x1BC0` (count `+0x1BC4`, stride `0x34`; factor `+0x28`, rate `+0x2C`, current entry
  `+0x30`).
* **Activate** `FUN_00a36748` (ESI = component, push byte force, RET 4) -> `FUN_00a75014`
  (stdcall, 9 args, RET 0x24): rate = `1 / max(activationTime, 0.01)`. **Deactivate**
  `FUN_00a365ce` (ECX = component, push byte force, RET 4) -> `FUN_00a735d9`: rate =
  `-1 / max(entry+0x08, 0.01)` (the time copied at activation). Both do nothing while
  `mgr+0x1F48` (a cutscene's "no other environments") is set and force is 0.
* **The blend is time-based only.** `FUN_00a734a3(dt)` adds rate x dt to each factor (clamped
  0..1) and `FUN_00a7467a` lerps the entries per priority group by those factors.
* **What starts and stops one:** trigger enter/exit (component vtable `+0x19C`/`+0x1A0` =
  `FUN_00a3680e`/`FUN_00a3660b`) activate/deactivate non-global areas with the ramp. Attaching
  (`FUN_00a3672c`) activates nothing. Detaching (`FUN_00a36709`, i.e. unloading the layer)
  deactivates with the ramp; the entry keeps its own copy of the parameters, so it can fade out
  after the component is gone.
* **Every door snaps it.** `FUN_0095db81` runs when a door task finishes (callers `00502B51`
  type 1 tick in state 2, `00502E9F` type 2, `005029B4` type 0, `0095DE1A` type 3, `0095DCB2`
  door slot `00EF02E0`). It walks the world's components (`world+0x298`, next `+0xC8`), and for
  every non-global `CAreaEnvironmentComponent` tests the point `world+0x2E0` with
  `IsPointInsideArea` (`FUN_00a4914f`) and activates or deactivates it. Then, at **`0095DC5E`**,
  it calls **Stabilize** `FUN_00a74fbf` (EAX = manager, no stack arguments): advance every blend
  by the longest activation/deactivation time + 1 s and drop finished entries. Bytes:
  `A1 50 49 40 01 8B 40 40 8B 80 B8 02 00 00 E8 5C 73 11 00` (`0095DC50`, unique), the call
  is the last 5. `world+0x2E0` is copied every game tick at `00AAAE80` (`FUN_00a91ed0`) from
  the position (`+0xA0`) of the entity behind the handle at `game+0x58`; whether that is the
  camera or Geralt is not proven.
* `FUN_00a91952` (the partition job) never calls the environment manager.

Other Stabilize callers (script `AreaEnvironmentStabilize`, cutscene environment events
`FUN_00afee4c`/`FUN_00afedd7`, the deferred flag `mgr+0x1BE8`, the first weather pick in
`FUN_00a78346`) are not door-related. Script natives `AreaEnvironmentActivate(name)` /
`AreaEnvironmentDeactivate(name)` / `AreaEnvironmentStabilize()` (registered in
`FUN_00a76637`) use the component's own times; none takes a blend time.

### When it switches, and why that is the wrong moment

The areas are drawn generously around the buildings. From the player positions in the door
traces (run 13) and the polygons in `interior.w2l`:

| Door | Geralt outside the door, at the switch | Area edge beyond the door |
|---|---|---|
| inn, street door | inside `house_01_inn`, 2.4 m from its edge | ~3.3 m |
| clerk (`03_clerk_marketplace`) | inside `clerk_marketplace_0`, 4.9 m from its edge | ~5.7 m |
| Einar Gausel (partition loads no environment; polygon shown for size) | inside `taves_craftsmen` (one polygon over a whole block, 85 x 40 m) | ~6 m |

Timeline of an instant door (run 13, inn): the door task starts (Geralt 0.9 m outside the
door), the switch runs 8 ms later and loads `interior_city` (47 ms), the fast-doors tick sends
the synthetic `fade_out` 79 ms after the start, and the task ends ~95 ms after the start with
`FUN_0095db81`, which activates the inn's area if the tested point is inside it (Geralt is,
2.4 m from the edge; a camera a few metres behind him is near the edge) and Stabilizes. So the
interior look lands on the street about a tenth of a second after Geralt touches the door, as a
hard cut; if the tested point is the camera and still outside, it lands as soon as the area's
trigger fires, also with 0 s. On the way out the switch unloads `interior_city`
(detach -> deactivate, 0 s -> 10 ms) and the door's re-evaluation and Stabilize finish the
rest: the room gets the outdoor look while Geralt is still inside. Even an area with real blend
times (the pier: 0.5 / 3 s) is cut by Stabilize. The game's fade hid both.

The workaround `instant_doors=0` follows from this: the snap comes one door tick after the
fade-in starts, so with `door_fade=0.25` the screen is still ~90 % black when it happens; with
the shipped 0.05 s fade it would be about two-thirds black. Not tried in game.

## Lighting: can it be preloaded or blended?

* **Preloading** the interior lighting buys nothing by itself: `interior_city` is 575 KB of
  parameters and attaches within the ~50 ms job, and attaching activates nothing. Keeping it loaded
  all the time (treating it like a preloaded interior) would hand activation to the triggers:
  the polygons reach 3-6 m into the street and `taves_craftsmen` covers a whole block, so the street
  would get the sunless interior look whenever the camera is in them. Not recommended. The
  interiors' own lights (`...\interior\lights*.w2l`) are already preloaded with the area.
* **Blending** is built in (time-based, per priority group) and CDPR uses it outdoors (forest
  4 s, `podejcie` 10 s, rain 20/30 s) and in the prologue castle's interiors (0.3 s). For a door
  the DLL would:
  1. skip the Stabilize call at `0095DC5E` for doors `keep_outdoors` handles (the
     re-evaluation before it stays, so the right areas still get activated/deactivated), and
  2. give interior areas a minimum blend time, because their data says 0: raise the
     activation/deactivation times in the `{priority, activation, deactivation}` block passed to
     `FUN_00a75014` (arg 7) while a kept door is re-evaluating, or set the entry rate `+0x1BB4`
     and the group rate `+0x2C` to +-1/T after the calls. The fade-out uses the time copied at
     activation (entry `+0x08`), so it must be raised when the area is activated.
* **Timing it to the threshold** (better still): defer the `\environments\interior` layer on a
  kept door the way 0.8.1 defers occluders (`ko_frame` and a same-partition switch), loading it
  once Geralt is ~1.5-2 m inside and unloading it once he is ~2-3 m outside, then re-evaluate
  the areas without Stabilize. The cross-fade then happens as he passes the door, not when he
  touches it. The deferred switch is not a door task, so `FUN_0095db81` does not run there; the
  DLL would call the activate/deactivate step itself or rely on the trigger events (unproven).
* **Keeping the outdoor detail seen from inside** (through windows and the open door) would mean
  overriding the interior's far plane (26-386 m) and depth of field with the outdoor values while
  the outdoors is kept. Where the blended `m_cameraPlanes` (`+0x194C` in the parameters) and the
  DOF values reach the camera and post-processing is not traced. The sun stays off inside: an
  environment is per frame, so a sunlit street seen from a sunless room is not possible without
  changing the interior look itself.

## Ground texture circle

Not a door issue. **Correction, 2026-11-04:** this section covers the ground texture only, which has
no distance to raise. The owner then reproduced a sharp chevron-shaped line ahead of Geralt. It is the seam at the end of the
first sun-shadow cascade, and Shadow quality High and Ultra remove it. See
[the visible cutoff](npc-streaming-sun-shadows.md#the-visible-cutoff-past-the-last-cascade-2026-11-04-static).
Plants turning flat a few metres ahead are a third thing: see "Plant billboards" in
[graphics-presets.md](graphics-presets.md). Which of these a player means by "low-res circle" is not established.

* **No terrain distance setting.** The ini key table (UTF-16 at `00E44454`-`00E44FAC`,
  `[Viewport]`, `[Rendering]`, `[TextureStreaming]`, `[Engine]`) has no terrain key; no UTF-16
  string in the exe names a terrain detail distance, radius or clipmap. Texture keys that reach
  terrain: `MaxAtlasTextureSize` (settings `+0x1C`) and `AtlasTextureDownscale` (`+0x40`), set
  through `FUN_00b07714`.
* **One material at every distance.** A terrain tile gets its material once, when it attaches
  (`00B18F97`, only caller `00A4BA47`): `engine\materials\Terrain\TerrainDetailed.w2mg` when the
  tile has the diffuse atlas, normal atlas and mask, else the unlit fallback `TerrainStd.w2mg`.
  Parameters `DiffuseMap` (the tile's colour map), `DetailDiffuseAtlas`, `DetailNormalAtlas`,
  `AtlasMask` and `DetailScale` = 0.3 (`00B19044 movss xmm0,[00F112EC]`; 0.3 is the detail
  tiling, one repeat per 3.3 m, not a distance; `00F112EC` is shared by 48 references, so a patch
  would repoint the disp32 at `00B19048`). The terrain vertex constant `VSC_DetailLevelParams`
  (c36) is zero for every chunk (`009CDDD1`); chunk LOD 0-5 changes only the mesh.
* **The detail fades by mip level, in the shader.** The four terrain pixel shaders in
  `CookedPC\shader.cache` (file offsets `0x211660`, `0x4A815C` G-buffer; `0x9235C`, `0x633290`
  forward) output `DiffuseMap + detail - 0.5` and fetch the detail with `texldl` at an explicit
  level `clamp(0.875 * log2(max(|ddx|, |ddy|) in atlas texels), 0, c41.z)`. There is no camera
  distance term. Once the detail has blurred out, only the colour map is left: 1024 texels per
  512 m tile (0.5 m per texel), 256 on some tiles (2 m). Because the level follows the larger
  derivative, the ground blurs within a few metres at grazing angles, and driver or DXVK
  anisotropic filtering cannot help an explicit-level fetch (reasoning, not tested).
* **Not texture streaming.** Every terrain texture group is `Streamable=false` in
  `bin\config\Rendering.ini` (`TerrainSpecial`, `TerrainMaskMap`, `TerrainDiffuseAtlas`,
  `TerrainNormalAtlas`), so `TextureMemoryBudget`/`MaxResidentMipMap` should not touch it
  (the group parser `FUN_00b07438` reads those keys; that streaming honours them is inferred).
* **Depth of field can look the same.** The environments' DOF curves (outdoors: far focus
  ~9.5 m, full blur ~105 m) blur everything past the focus distance, ground included.
  `AllowDOF` (settings `+0x54`) turns gameplay DOF off.

How to tell them apart in game: with DOF, rocks, grass and NPCs past the edge blur too and
`AllowDOF=0` removes it; the detail mip falloff blurs only the ground, as a smooth gradient
centred on the camera that moves outward when looking down; streaming would show tile-aligned
square edges that sharpen after a moment; a smaller atlas (`MaxAtlasTextureSize` below 4096,
e.g. a lower preset) makes the near detail itself softer. `AllowDOF` is the options menu's
"Depth of Field - Gameplay".

Levers, none tried: `MaxAtlasTextureSize=4096` and `AtlasTextureDownscale=0` (`[Rendering]`
in User.ini; the atlases are 4096 natively); a lower 0.875 in `shader.cache` (data patch; an
integrity check is unknown); a negative mip bias (whether D3D9/DXVK apply it to `texldl` is
unproven). The first is a settings answer for the player; the others are research.

## Options, ranked

By visual gain against risk and effort. Option 1 is built (see "Built").

| # | Option | Visual gain | Risk | Effort |
|---|---|---|---|---|
| 1 | **Door blend**: on kept doors skip Stabilize at `0095DC5E` and floor interior areas' blend times (~0.75-1 s; the class default is 1 s, the prologue castle uses 0.3 s), with an ini switch and a log line per door listing the manager's entries (name, priority, factor, rate) | The flash becomes a ~1 s cross-fade starting at the door, both ways | Low: one call site and one hook, both on the game thread inside the door task; vanilla and faded doors untouched | Small |
| 2 | **Threshold timing** on top of 1: defer the `\environments\interior` layer until Geralt is through the door (both directions), then re-evaluate without Stabilize | The look changes as Geralt passes the door, blended | Medium: needs the activate/deactivate step outside a door task, and the tested point (`world+0x2E0`) proven | Medium (reuses the occluder deferral) |
| 3 | **Outdoor far plane/DOF from inside** while the outdoors is kept | The street through windows is no longer cut at 26 m and blurred | Medium-high: changes CDPR's interior look; frame cost of drawing the outdoors from inside | Medium, consumer of `m_cameraPlanes` not traced |
| 4 | Workaround only: `instant_doors=0` with `door_fade=0.25` | Hides the snap behind a short black fade (the vanilla approach) | None | Settings |
| - | Keep the interior environment layers loaded all the time | Negative: interior lighting on the street near buildings | High | Small |

Low-poly stand-ins need nothing: no Act 1 door swaps one, and `ko_class` already keeps the inn's
ship stand-ins out. Acts 2 and 3 have no stand-in layers by those names (only a quest layer,
`03_camp\quests\...\q209i_kaedwen_low_poly_before_main_gate`).

**Recommendation:** build option 1 first, with its log line, and test the Flotsam inn and one
house both ways. Add option 2 only if a ~1 s cross-fade that starts at the door still looks
wrong. Option 3 is research. For the ground circle, no build: ask the player for a screenshot
and to try `AllowDOF=0` (options: "Depth of Field - Gameplay") and
`MaxAtlasTextureSize=4096`/`AtlasTextureDownscale=0`.

### Side finding: `indoor_outdoors` also keeps the sun on indoors

The third frame flag the `indoor_outdoors` patch keeps on (`+0x278F`) is show flag 35
**SHOW_Lighting**, not "one more": the frame's show flags start at info `+0x275C` (69 bytes,
copied in `FUN_0040df86` at `0040E0DD`; enum registered in `FUN_0041b5b9`), so `+0x2775` is flag 9
SHOW_Terrain, `+0x2776` flag 10 SHOW_Foliage and `+0x278F` flag 35 SHOW_Lighting, which gates the
sun shadow cascades (`FUN_009bfad1`) and the sun/global-light passes (`FUN_00aec932`,
`FUN_00aed267`, `FUN_00b14859`, `FUN_009d7b67`). In the vanilla game an `LT_INDOOR` partition
(prologue interiors) has no sun light; with `indoor_outdoors=1` it does. Act 1 has no
`LT_INDOOR` partition, so Flotsam is not affected. Worth checking in the prologue's interiors:
sunlight or sun shadows where a roof should block them, and the frame rate. `renderer+0xAC` (the
other half of that check) is a game-running flag (render command `00E7C3C4`, slot 3
`009DC0AB`), 1 during play; `hasWindows` never reaches the renderer.

## Owner test 2026-11-04 12:26 (0.10.0 Steam build, defaults)

Save loaded inside the Flotsam inn, out through the street door, then Einar Gausel's door in, out
and in. Owner: the lighting change at the inn is clear both ways; Einar Gausel's door shows no
change, only the small loading spinner in the corner. That matches the partition data (his
partition loads no environment) and confirms the environment as the visible change.

The log showed two more door issues:

* **A save inside an interior leaves the street half-loaded.** The load ran into
  `inn_interior` (965 layers, 64 interiors preloaded). That partition inherits `outside`'s 181
  groups (walls, streets, building shells) but not the `Flotsam` partition's own 51 (city lights,
  rocks, bushes, grass, piers, Loredo's residence exterior, animals, occluders, town sound). The
  first way out loaded them on the spot: `doors: 'inn_interior' -> 'Flotsam' loaded 49 layer(s)
  not preloaded (391 ms)`, decided as "is loaded - no fade" at the door start, so the game
  froze for ~0.4 s in plain view.
* **One slow door keeps its fade for the session.** The same line ends `took 391 ms - it keeps
  its fade`: the pair goes into `g_koSlow`, which is only cleared when `preload_interiors` is off
  (`hook_PartRun`, `g_koPairCount = g_koSlowCount = 0` under `!cfg_preload_interiors`). Every
  later exit from the inn then gets `door_fade` (a 0.05 s blink) although it loads nothing.
* **The loading spinner** at kept doors is the door task's render command `00E7C07C(1)` (shows
  `00_loading.swf`, see interior-preload.md, run 10), sent by the type 1 task's constructor and
  cleared on `fade_out`.

Fixes (built, see "Built"):

1. **Load the street with the save.** On a load (mode 1) into an interior partition, append the
   street sibling's layers to the job's last bucket, the way `preload_interiors` appends
   interiors, without `fake`, sound, occluder, interior-environment and low-poly layers (what a
   kept exit and re-entry leaves loaded anyway). Costs the ~0.4 s on the loading screen instead of
   on the first door. Finding the street sibling: Act 1 marks no partition as outdoor; the sibling
   with the most own layer groups works for Flotsam (51 against 7-18) and Binduga, but is a
   heuristic.
2. **Check before going without a fade.** In `hook_DoorStart`, count the target partition's layer
   infos (its own and its parents' groups, as `kp_find`/`kp_in_groups` already resolve them) that
   are not loaded (`+0x38`) and not skipped classes; above a few, use `area_fade` (0.25 s) instead
   of no fade. This covers every unknown case (other acts, other entry points) and makes the
   `g_koSlow` list unnecessary; until then, a pair should leave it after a fast run.
3. **No spinner on instant doors.** Skip `00E7C07C(1)` the way the fade commands are skipped,
   only for doors already set to `KO_NO_FADE`.

## Memory: native Direct3D 9 and the GPU fault runs (2026-11-04)

Owner tests that afternoon, Steam, 4K, extreme preset, `TextureMemoryBudget=1024`:

* **The hangs were not the mod.** Five runs hung a few minutes after loading a save inside the
  Flotsam inn and walking in and out. In each, NVIDIA's driver logged event 153 (a GPU
  reset), DXVK 3.1.1 logged `VK_ERROR_DEVICE_LOST`, and the Task Manager dumps showed the main
  and render threads spinning in DXVK's lock (`d3d9+0x298dd1`, `lock cmpxchg`/`pause`). Each
  step turned off more: `door_light_blend=0`, `door_spinner=1`, `preload_street=0`,
  `exit_trace=0`, then the released 0.9.0 DLL, then no Engine Fix at all. The fault came every
  time, so it lies in DXVK 3.1.1 or the driver. The corrupted ceiling beams of the run before
  were most likely the same fault. The owner switched DXVK off.
* **Without DXVK the 32-bit address space runs out.** The system's Direct3D 9 keeps a copy of
  every managed texture in the process. The same inn save:

  | | At the menu | After the load | Then |
  |---|---|---|---|
  | DXVK, 0.9.0 (no street preload) | 1 072 MB | 1 326 MB | |
  | Native D3D9, this build (interiors and street preloaded) | 1 738 MB | 2 807 MB, 572 MB largest free | 3 405 MB, crash |

  The crash: the renderer's allocator (`00AC85F0`) returned NULL to `009CB578`, read at
  `009CB57D`; the dump could not be written (`0x80070008`). 0.8.0's memory figures were all
  taken with DXVK. Most players use the system's Direct3D 9, so `interiors=2` can do this to
  them, which fits the unexplained `interiors=2` crash a GOG player reported.

### The memory guard (built, not yet run in game)

* **Preloaded layers stop at `[loading] preload_memory_mb` (1280) of free address space.**
  An area load puts the preloaded layers (interiors, a house's street) at the end of the
  job's last bucket, after the area's own. The per-layer load `FUN_00a5ac6c` (EDI = layer info,
  one stack argument, `RET 4`; its only call is `00A91709` in `FUN_00a91600`, fingerprint
  `00A916FB: 8B 7D EC 8D 45 D4 50 89 5D D4 C6 45 D8 01 E8 5E 95 FC FF`) goes through
  `layer_load_stub`. It checks `GlobalMemoryStatusEx` before each preloaded layer that the job
  did not list itself, and skips it below the floor. Those houses load on their first visit,
  with a fade (`ks_missing`). Log: `preload: N of M preloaded layers loaded, K left for their
  first visit (free address space at the lowest X MB, floor 1280 MB)`.
* **Doors go back to the game's own at `[loading] door_memory_mb` (1024).** A door that starts
  with less free than that runs the game's job (`g_koMemLow`): the outdoors and the other
  interiors unload and the GC runs, behind the 0.25 s area fade. Log: `doors: ... with only N
  MB of address space free - the game's door`. Its occluder layers wait, as on a kept door,
  until Geralt is through (`partition: memory door kept ...`, then `doors: Geralt is through the
  door - loading the occluders`). So `fast_doors` brings its picture back as soon as the job is
  done, behind the 0.25 s fades.

  Why: in the first build, such a job loaded the town's occluders at once. With the picture back
  after 172 ms, the town showed see-through in front of Geralt until he was out of the
  building's occluder block. This is the 0.8.1 bug at the inn's harbour door (GOG, native D3D9,
  4K, 2026-11-04: 930 MB free at the exit).

  When the occluders cannot wait (no viewport tick, or the request function not recognised), the
  door keeps the game's timing instead (`g_koMemLowTask`): the picture comes back on the door
  animation's `fade_out` event.
* **Without the per-layer hook** (a build it is not ported to) nothing is preloaded when less
  than 1280 + 1024 MB is free before the load. On GOG the per-layer load is `00AAE95E`
  (stdcall `(info, ctx)`, `RET 8`), called once at `00AE5D66`; it has its own stub.
* 0 turns either guard off. With DXVK there is room to spare, so neither triggers.
* Not handled: with fewer interiors preloaded, the open doors of houses that were skipped show
  nothing behind them, because the area's `fake` rooms are still dropped. The texture budget
  (`TextureMemoryBudget` in User.ini) sets most of the growth after a load. On native D3D9 a
  lower value (600-800) leaves more room.

### Native Direct3D 9 at 4K: runs with the guard (owner, 2026-11-04 15:29-15:45)

Same inn save, loaded from the main menu, Engine Fix with the memory guard, no DXVK, 3840x2160:

| Settings | Menu | After the load | Preloaded | Then |
|---|---|---|---|---|
| Hand-tuned User.ini (`TextureMemoryBudget=1024`, 4096 textures), draw distance extreme | 1 739 MB | 2 619 MB (821 MB free) | 0 of 111 | out of memory after 14 s, walking in the inn |
| Launcher maximum (`TextureMemoryBudget=600`, 2048 textures, `MeshDistanceScale=1.2`), extreme | 1 746 MB | 2 625 MB (819 MB free) | 0 of 111 | 60 MB free a minute later at the door, crash |
| Launcher maximum, extreme (2nd run) | 1 738 MB | 2 618 MB | 0 of 111 | 59 MB free 30 s later at the door, crash |
| Launcher maximum, draw distance baseline | 1 740 MB | 2 620 MB (818 MB free) | 0 of 111 | 58 MB free 28 s later at the door, crash |

* The guard did what it should: the area's own layers (901, with the inn's 6 interior
  layers, the same as without the mod) already left only ~820 MB, so nothing was preloaded.
* Neither the texture budget nor the draw distance level changed the load or the time to the
  crash. At 4K the game's own Direct3D 9 path alone runs out in this scene, since it uses 0.7 GB
  more than DXVK at the main menu. The crash sites are allocation failures in the renderer
  (`009CB57D` after `00AC85F0`, `009C4AE0`, `00AC883A`).
* `door_memory_mb` triggered at 58-60 MB free: by then it is too late to help.
* `witcher2.exe` already has the Large Address Aware flag (characteristics `0x0122`), so a
  "4 GB patch" changes nothing; this is the 4 GB ceiling itself.

Back on DXVK 3.1.1 with `dxvk.enableDescriptorHeap = False` (its log then says `Binding model:
Descriptor buffer`), 15:56-16:03: the same save loaded with 1 384 MB private and 2 091 MB free,
`preload: 110 of 111 preloaded layers loaded, 0 left`, then 8 doors (inn and clerk, all without a
fade, spinner hidden) and a normal quit, with no NVIDIA event and no `DEVICE_LOST`. One run is
not proof, but the five runs with the descriptor heap all hung within minutes.

Decision (owner): at 4K, use DXVK and recommend it strongly; on Nexus it can be listed as a
recommended requirement. The memory guard stays: it costs nothing with DXVK and keeps the mod
from adding to the load where room is short. Not measured: native Direct3D 9 at 1440p or 1080p.

## What is not proven

* That what the player saw is the environment snap. The data and the code point there, but no
  run has logged the environment manager at a door. A check with the installed DLL and no
  changes: the clerk's door (`03_clerk_marketplace`) or the inn should show the change, Einar
  Gausel's door (no environment in its partition) should not. Proof: a hook on `0095DC5E` logging, before
  and after Stabilize, `env: door re-evaluation at (x, y, z) - N entries: '<name>' prio P blend B
  rate R act A deact D [global]`, at the inn door both ways and at a house.
* Whether `world+0x2E0` is the camera or Geralt, and who drives the area triggers.
* That `m_cameraPlanes.farPlane` is the camera's far clip (the values fit: interiors with
  `hasWindows` have 386 m and 1 202 m, those without 26-69 m), and that the curve value (`W`)
  is the distance in metres.
* Whether skipping Stabilize leaves anything stale: Stabilize also drops finished entries
  (`EnvMgrRemoveFinishedBlends`); that the normal tick drops them too is assumed.
* `environmentType` (`SWorldPartition` `+0x24`, a CName) has no reader found; `hasWindows`
  (`+0x2C`) none outside the data.
* The ground circle: what the player actually sees (DOF, mip falloff or something else).
* A spinner that still shows: on GOG (owner, 2026-11-11, 0.12.1) the loading spinner flashed
  once on the way out of the craftsmen's bar, on a door without a fade and with nothing to
  load. "Show" commands are dropped for 2 s after the door starts and for 2 s after the
  occluder switch, which came 3.4-3.9 s after the door, so one arriving between the two gets
  through; that it was such a command is a guess, since a command that passed was not logged.
  After 0.12.1 one is: for 10 s after a door without a fade, `doors: loading spinner shown N ms
  after a door without a fade (its occluders still wait | M ms after its occluder switch | no
  occluder switch for this door) - hidden only for 2000 ms after each`, and `doors: loading
  spinner taken away again N ms later` (at most 8 lines a door; passive, render thread). A
  `shown` between 2 000 ms and the occluder switch confirms the guess; the fix would then hide
  it from the door until 2 s after the switch.

No in-game run was needed for this research: the installed `load_trace` does not log the
environment manager, so a run with the current DLL could not show it. The proof run belongs to
option 1's first build.

## Reading the level data

Everything above about layers and environments was read offline from `CookedPC\pack0.dzip`
(DZIP reader as in `tools/build-interior-maps.py`) with a throwaway CR2W reader; no game file
was changed. Format notes for the next reader (W2 CR2W version 115):

* Header: `"CR2W"`, version, flags, then (offset, count) pairs for names, objects, links and
  dependencies (11 dwords, 44 bytes). Names are encoded strings: first byte bit 7 = ASCII (else
  UTF-16), bit 6 = more length bytes (7 bits each), low 6 bits = length.
* Object table: per object `u16 type name index, u32 parent, u32 size, u32 offset, u32, u32`,
  then an encoded template path (empty for most; `CWorldPartitions` in a `.w2w` carries
  `game\2_act2\l02-port.w2part`).
* Properties: `u16 name, u16 type, u16 0xFFFF, u32 size (including itself), value`, ended by
  `u16 0`. Structs nest a property list; arrays are `u32 count, u16 element type, u16 0xFFFF`,
  then the elements (encoded strings for `@String`, nested lists for structs). A property equal
  to the class default is not written, so a missing `activationTime` means "class default", not
  0.
* `CWorldPartitions` (embedded in the `.w2w`, no `.w2part` file in the archives): 3 bytes, then a
  tree; each node is a property list (`name`, `layerGroups`, `dependencies`,
  `loadingScreenPath`, `soundAmbient`, `musicCue`, `locationType`, `hasWindows`) followed by an
  encoded child count and the children. A partition's groups are its own plus its parents'
  (`Root` -> `Outdoor` -> `global` -> `landing_site` -> `outside` -> `inn_interior`).
* A `.w2w` also holds the world's `CLayerGroup` and `CLayerInfo` objects; a layer info's
  `depotFilePath` is its `.w2l`. A partition's layers are the layers directly in its groups'
  folders (its own groups and its parents').
* A `CStreamingDoor` in a `.w2l` has `inside` and `outside` (`SDoorPartition`: a nested property
  list with `worldPartition`, the full partition name without the root, and `transitionType`
  when it is not the default).
* An entity's `transform` (`EngineTransform`) is a flag byte (seen: `03` with position and
  rotation, `07` with scale too) followed by the position as three floats. A cooked layer of static meshes only
  (the fake rooms) is one `CLayer` object: after its property list, one record per mesh with
  the mesh's import index (a negative `i32`, -1 the first import) and a 4x4 float matrix whose
  last row is the position.
* Survey of all 210 `CAreaEnvironmentComponent`s in `pack0.dzip`: interiors with explicit
  0/0 times: Act 1 12 of 20, `03_camp` 22 of 26 (a few 2/2 or 3/3), the prologue castle's 19
  interiors 0.3/0.3, `04_city` 5 at the class default (1 s, see above). Outdoor areas mostly
  blend over 0.5-30 s (forest 4 s, `podejcie` 10 s, rain 20/30 s).

## Built 2026-11-04 (Steam; ported to GOG the same day, see the end of this section)

`src/enginefix.c`, section "door lighting and the loading spinner", plus `ks_*` after the
partition tree code. All of it acts only on doors that go without a fade (`instant_doors`, with
`keep_outdoors` and `keep_interiors` or `preload_interiors`). `build_gate` turns a part off on a
build whose addresses for it are not all known.

| Switch | Default | What it does | Hook |
|---|---|---|---|
| `[loading] door_light_blend` | 1.0 | For 4 s after an instant door starts: area activations get at least this blend time both ways, active non-global environments at least this fade-out, and the door's Stabilize is skipped. 0 = the game's hard switch | call `00A367CB` -> `env_add_stub` (raises arg 7's times, then `FUN_00a75014`); call `0095DC5E` -> `env_stab_stub` (skips `FUN_00a74fbf`, EAX kept); entries `+0x08` raised in `hook_DoorStart` |
| `[loading] preload_street` | 1 | A load (mode 1) into a house also loads its street's own layers, without `fake`, sound, occluder, interior-environment and low-poly layers | `hook_PartRun`, last bucket, `ks_street` + `ks_walk` |
| (no switch) | | Before a door goes without a fade, the target partition's own groups are checked; more than 16 layers still to load -> `area_fade` (0.25 s) | `hook_DoorStart`, `ks_missing` |
| (no switch) | | A pair marked slow (>= 50 ms) goes without a fade again after a fast run | `hook_PartRun` |
| `[loading] door_spinner` | 0 | 0 drops the spinner's "show" commands for 2 s after an instant door starts, and for 2 s after the DLL's own occluder switch (`ko_frame`, seconds after a kept door), whose job showed it too (GOG, 2026-11-04); 1 = the game's spinner | slot `00E7C088` (`009DC3F1`) -> `hook_SpinnerExec` |

Fingerprints checked before patching (after the SteamStub has decrypted `.text`):
`00A367C2: 51 8D 8E C0 01 00 00 51 50 E8 44 E8 03 00` and
`0095DC55: 8B 40 40 8B 80 B8 02 00 00 E8 5C 73 11 00` (from -9; the bytes before hold `VA_GAME`,
relocated on GOG); `patch_call` / `patch_rel32` also check each rel32 against its target. The street groups are matched the way the game loads them
(`FUN_00a83097` -> `FUN_00b0582e` with "recursive" 0: only layers directly in a group's folder),
not with the recursive match the interior preload uses. A layer listed twice in one job is loaded
once (`FUN_00a91600` skips infos whose `+0x38` is set; the per-layer load `FUN_00a5ac6c` is
synchronous).

Log lines to look for: `doors: environment activation hooked, door Stabilize hooked -
door_light_blend on`, `doors: loading spinner command hooked`, per door `is loaded - no fade (N to
load, lighting fades out slowly, spinner hidden)` and `doors: lighting blends over 1.00 s instead
of switching at once - 'house_01_inn' 15 at 0.10 (+1.00/s)`; on a save inside a house
`preload: '...inn_interior' is inside 'Flotsam' - its N layers load now too`.

GOG 3.5.0.26g (ported 2026-11-04; seen on GOG the same day: `lighting blends over 1.00 s` at
`taves_craftsmen` and `house_01_inn`, `spinner hidden`; see `gog-build.md`):

- **The environment code is the same.** The activation, Stabilize, the manager tick and its
  blend advance have the same bodies, entry offsets and constants.
- **Area activation.** `VA_ENV_ADD` is `00AC9EED` (stdcall, 9 arguments, `RET 0x24`), called
  once at `00A89F59` in the component's activate `00A89ED3`. That function is `__thiscall` on
  GOG (Steam passes the component in ESI). The argument order is unchanged, so
  `env_add_stub` works as it is.
- **The door's Stabilize.** The door finish `005AF51F` (Steam `0095DB81`) is rewritten:
  - it calls the manager's shared re-evaluation `00ACA3B4(world+0x2E0, 1, 0)`, `__thiscall`,
    `RET 0xC`, which is also used by cutscenes, so it is not hooked;
  - it then **tail-jumps** to Stabilize `00AC8CDF`, which is `__thiscall` (ECX = the manager)
    with a plain `RET`.

  So `VA_ENV_STAB_CALL` is the `JMP` at `005AF54B`. It is patched with `patch_rel32(..., 0xE9,
  ...)`, which keeps the opcode. `env_stab_stub` keeps ECX for Stabilize. Its skip `ret`
  returns to the door's caller, as Stabilize would. None of the five callers reads EAX.
- **Fingerprints.** Both builds are checked from -9, because the bytes before that hold
  `VA_GAME`, which is relocated on GOG.
  - Steam `0095DC55: 8B 40 40 8B 80 B8 02 00 00 E8 5C 73 11 00`
  - GOG `005AF542: 8B 40 40 8B 88 B8 02 00 00 E9 8F 97 51 00` (`88`: ECX)
  - GOG add site `00A89F50: 51 8D 8E C0 01 00 00 51 50 E8 8F FF 03 00`
- **The spinner** (`CRenderCommand_ToggleLoadingIndicator`) is GOG's vtable `00F1F0B4`, slot 3
  `00F1F0C0` -> `00A2D9F9`, with the show byte at +0x08. It is created by the same door task
  and partition job code.

Checked offline: build clean at /W4, `tools/buildprobe` (both exes; since the GOG port it also
checks GOG's add call, the Stabilize jump and the spinner slot),
`tools/check-repo.ps1 -Game` (Steam). To check in game: the inn and the clerk's house both ways
(the change should take about a second, with no jump), Einar Gausel's door (no change, no
spinner), a save made inside the inn then out (no freeze, no fade), and `door_light_blend=0`
for the old behaviour.

## Freeze at a door: a pathfinding job that does not finish (2026-11-06)

Steam, DXVK, Flotsam, 0.11.0 plus the day's builds: about six minutes after a load, a kept door
(`'Flotsam' -> '04_guards_marketplace' is loaded - no fade (0 to load)`, 17:05:56) froze the game
for good. The owner took a full dump from Task Manager 30 s later (read offline with cdb, x86
view of the WOW64 dump).

* **Main thread:** in the partition job `FUN_00a91952` (run from our `hook_PartRun`), first stage
  "Flushing...": `FUN_00b0cea0` spins with Enter/LeaveCriticalSection on the job manager at
  `0258A240` (array `+0x20`, count `+0x24`) until no job has `+0x30 == 1`, `+0x34 == 0`,
  `+0x38 != 0`. No sleep in that loop.
* **The queue:** 53 jobs, all still in that state: 51 `CJobSpawnEntity` (vtable `00E8DFC4`) and 2
  `CPathfindingQueryJob` (`00ECE1D0`). The critical section was free.
* **The one busy job thread (T4):** inside PathEngine (`cMesh.cpp` asserts in `FUN_0065a4c0`,
  "findShortestPath_MultipleStartPositions"), at `FUN_006550c0`, an edge walk around a mesh
  vertex in a `while (true)` loop (`0065525A`). Stack: `0098E60E` -> `FUN_008e498c` (the query job)
  -> `FUN_0065a4c0` -> `FUN_00658160` -> ... -> `FUN_006550c0`. The other job threads (T3,
  T12-T14, T35) waited.
* **Not DXVK and not the driver:** no NVIDIA event, no `DEVICE_LOST`, the DXVK log's last entry
  was two minutes earlier, and every DXVK thread waited idle. Unlike the 2026-11-04 hangs, no
  thread was in `d3d9`.

So a pathfinding query never ended and every job behind it waited; the door's flush is simply the
first thing that waits for all of them. The owner has seen such freezes before without a cause.

### Why the walk never ends

The job thread's arguments in the dump: `FUN_006550c0(line, startEdge, before, after)` with
`line = {65C9, 5381, 65C9, 5381}`, both ends the same point. The function walks the edges around
a mesh vertex until the cross product of an edge with the line's direction changes sign; the
first loop (`00655214`) leaves only when the two products differ. With no direction both are
always 0, so it goes round the vertex for good. Any zero-length line that reaches it hangs that
job thread, in the vanilla game too; nothing in the mod produces the line. Whether kept doors make
such a query likelier (more obstacles in the mesh at once) is not known.

The function is cdecl with four stack arguments and a plain `RET`, has nine callers (the crossing
walk `FUN_008bbf70` through `FUN_0065c0c0`, and eight in `008B`-`008D`), and is byte-identical on
GOG at `00725FC0` (PathEngine is a static library; 0x2D4 bytes compared).

### Fix: `[stability] path_guard` (default 1)

The entry (8 bytes, `55 8B EC 83 EC 28 53 56`, checked over 26) becomes a jump to `path_stub`.
For a line whose ends are equal it sets both outputs to the start edge and returns: what the
function itself returns when its first loop stops on the start edge, after which its second loop
stops at once on a product of 0. Every other line runs the original through a trampoline. Patched
behind the patch barrier from the worker. A test harness ran the stub: a point gets the start
edge without calling the original, other lines reach the original with the outputs it sets.

Log: `stability: path guard installed (PathEngine edge walk at 006550C0)`, and when it catches
one: `stability: path guard: N pathfinding line(s) of zero length answered at once`. Installed on
Steam (2026-11-06, a ten-minute Flotsam session with doors, no catch, clean quit) and on GOG
(at `00355FC0` with ASLR, four loads, no catch). Not yet seen
catching one: the frozen query is not reproducible on demand, so the evidence will be a caught line in
a long session's log and no more freezes of this kind. Not proven either: that the callers then
finish; `FUN_008bbf70` does (the end point lies in the start face, so `FUN_0065c0c0` returns 0).
