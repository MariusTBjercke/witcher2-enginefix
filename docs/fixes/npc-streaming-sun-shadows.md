# Remaining NPC disappearance and sun-shadow quality

Investigation after the owner's successful distance-control test, 2026-10-28.
No new runtime patch or installed setting was changed in this investigation.
Target: Steam 3.5.0.26, Ghidra `/witcher2/witcher2.exe.unpacked.exe`.

Owner decision later on 2026-10-28: retain the improvements already obtained and
defer further NPC/sun-shadow work. Findings below are preserved for a future
explicit request; they are not an instruction to continue patching. The four
working controls are now grouped by [graphics presets](graphics-presets.md).

## What the owner verified

Ctrl+Shift+F10 restores most missing shadows. Ctrl+Shift+F9 additionally restores
smaller shadows and other objects. Combined improvement is large. Remaining NPC
disappearance varies with position/orientation and sometimes persists until the
player approaches substantially. A boundary between near and distant shadow
quality remains.

The runtime log records both hooks running and actual new mesh admissions. One
five-second interval at 15:40:23 reports 667387 mesh evaluations, 346563 shadow
evaluations, 84608 baseline distance rejections and 74750 admissions, at factors
3/3. These are repeated mesh evaluations, **not unique object counts**. The
session ends with `unloaded` at 15:40:32. User confirmation establishes in-game
startup/control operation and a visual improvement, not complete removal of pop-in.

The visibility control can restore shadows too: a mesh excluded from rendering
cannot contribute to the normal caster path even when the caster distance is
increased. The independent shadow pass in `FUN_009bb613` calls the same range
routine and rejects invalid selected mesh LODs.

## Community NPC spawning is separate from mesh visibility

The shipped `bin/config/Community.ini`, section `[Constants]`, explicitly names:

| Key | Shipped file value | Runtime global VA |
|---|---:|---|
| VISIBILITY_SPAWN_RADIUS | 40 | 010D1B44 |
| VISIBILITY_DESPAWN_RADIUS | 60 | 010D1B50 |
| VISIBILITY_AREA_SPAWN_RADIUS | absent; compiled default 10 | 010D1B58 |
| VISIBILITY_AREA_DESPAWN_RADIUS | 30; compiled default 10 | 010D1B54 |
| VISIBILITY_CENTER_SHIFT_FACTOR | 2 | 010D1B5C |
| VISIBILITY_RADIUS_CHANGE_ENABLED | true | see save-loading conditional below |
| MAX_SPAWNED_NPC_LIMIT | 300 | 010D1B64 |

These names are confirmed by their own shipped comments and by config loader
`FUN_0099fa55`, which reads these float keys from configuration `Community`,
section `Constants`, into the globals above. Binary default bytes independently
confirm 40/60/10/10/2 and integer 300 before the ini is loaded.

### Center and distance consumers

`FUN_0099cbbb` reads an entity pointer from `[DAT_0160d0fc + 0x250]`, takes its
position at +0xA0/+0xA4/+0xA8, derives a heading angle from its +0x70 transform
using `FUN_00423fd6`, and returns an XY center displaced by:

```text
center.x = position.x - sin(heading) * VISIBILITY_CENTER_SHIFT_FACTOR
center.y = position.y + cos(heading) * VISIBILITY_CENTER_SHIFT_FACTOR
```

This establishes a heading-dependent center, not a camera-frustum test. The
pointer's exact actor identity was not independently typed in this pass. The
2-unit shift alone does not explain an arbitrarily large disappearance distance.

`FUN_0099d7a4` is the spawn eligibility path, anchored by diagnostic text
`Cannot spawn NPC from agent stub: SPAWN LIMIT EXCEEDED!`. It checks the NPC cap,
community/partition state, XY distance to this shifted center against the spawn
radius, area eligibility/radius, and unobstructed spawn position. It records
different failure reason codes at agent-stub +0x78.

`FUN_0099d029` is the complementary eligibility/removal decision. It uses the
same XY center, despawn/spawn/area-despawn radii and other entity/community state
checks. Both functions are called by the community update `FUN_009a10f4`.
Read helper `FUN_0099c70d` returns the global spawn/despawn pair. Setter
`FUN_0099c75d` requires `1 < spawn < despawn`; the single-value setters enforce
the same ordering against the other current radius.

Separate spawn and despawn thresholds create hysteresis: a previously loaded
actor can remain present beyond the distance at which an absent actor may spawn
again. This is a **strong candidate** for the reported disappearance/reappearance
behavior. It is not proof that the particular missing NPC is a community NPC or
that its disappearance was triggered here. Renderer occlusion/frustum tests and
scripted visibility remain alternatives.

### Important: saves can override ini radii

`FUN_0099ea36` serializes `global_community` values named `spawnRadius`,
`despawnRadius`, `areaSpawnRadius`, `areaDespawnRadius`, `prevSpawnRadius`, and
`prevDespawnRadius`. `FUN_0099f60e` restores them. It conditionally restores the
main spawn/despawn pair via `FUN_0099c75d` when `DAT_010cb8fe` is set, and restores
the area radii via `FUN_0099c72f` / `FUN_0099c746`.

`FUN_009a18ba` first loads Community.ini, then takes the save-loading path for an
existing saved state. Changing only Community.ini is therefore not a reliable
test against the owner's existing save. Forcing the globals every 120 ms would
also override legitimate gameplay radius changes and is not recommended.

A useful next diagnostic is to observe the actual radius values and stub reason
codes around one affected NPC, then test a modest multiplier at the consumers
while preserving the engine's own current values, radius ordering, NPC cap and
community/quest decisions. Do not begin by globally disabling NPC visibility or
raising the 300-NPC cap. Extending spawn range can cost AI/CPU work, unlike merely
drawing an already loaded mesh.

## Sun-shadow cascades explain a separate quality boundary

### Three fixed camera intervals

Render context constructor `FUN_009c011e` builds three intervals with lengths
7, 17 and 40. It starts at the camera near value and accumulates the distances
into context +0xEC/+0xF0/+0xF4 (count 3 at +0xE4, near boundary at +0xE8).
Relative to that near value, the endpoints are 7, 24 and 64.

Verified instruction sequence:

```text
009C016A  MOVSS XMM0,[00F11340]       ; 7.0
009C019A  MOVSS XMM0,[00F117B4]       ; 17.0
009C01AA  MOVSS XMM0,[00F11328]       ; 40.0
009C01C5  MOVSS XMM0,[EBP+ECX*4-C]
009C01CB  ADDSS XMM0,[EAX-4]
009C01D0  MOVSS [EAX],XMM0
009C01D4  INC dword ptr [EBX+E4]
009C01DE  CMP ECX,3
```

`FUN_009bfad1` consumes these boundaries to construct three directional-light
projection views and calls `FUN_009cc596` to collect their casters. It uses
hard-coded 1023/1024 terms while aligning adjacent projections. `FUN_00aee02c`
renders these views into separate regions of a shadow atlas, using a square
viewport of renderer +0x20 per cascade and an index-dependent viewport offset.
Renderer initialization also compiles `deferredLightDir.fx` with NUM_CASCADES.

### Separate 1024 map size

Renderer constructor `FUN_009dc712` stores 0x400 at renderer +0x20 at VA 009DC72F.
The instruction bytes are `C7 40 20 00 04 00 00`.
`FUN_00b12e39` consumes that dimension, allocates shadow resources with one
dimension `3 * size`, and stores resource handles at renderer +0x24/+0x28.
Combined with the three square cascade viewports, this establishes a 1024-per-
cascade atlas layout for the traced initialization path. The separate +0x2C
value (0x200) is used by another shadow resource, not this cascade size.

This path does not read MaxCubeShadowSize or MaxSpotShadowSize. Those are
separate resources. The existing Ctrl+Shift+F10 hook scales caster eligibility;
it does not alter the three camera intervals, atlas dimensions or filtering.

The reported near/far quality boundary is consistent with these cascades, but
the exact visible seam has not been correlated with a captured camera distance.
Longer intervals alone spread a fixed number of texels over more world area;
that can reduce sharpness. Raising only a distance multiplier is not a general
quality improvement.

A future quality experiment should keep the current intervals initially and
validate 2048-per-cascade allocation **together with all matching texel/alignment,
filter and bias calculations**. Only after that should the interval boundaries
be moved outward. The larger atlas has four times the texels; frame time and
shadow stability must be measured. No GPU cost was measured in this pass.

### The visible cutoff past the last cascade (2026-11-04, static)

The owner reproduced a sharp line on the ground some way ahead of Geralt, shaped like a chevron with
its point away from the camera; a player had reported it as a low-res circle. Screenshot only, no
distance measured. The code below was the first explanation; the in-game test further down rules out
the last cascade's edge as the line.

**Past the last cascade the ground is in full sun.** The sun shader (`CookedPC\staticShader.cache`,
file offset `0x25DA4` and variants; sAttrib0-3, sShadowMask, sSSAO, so the `deferredLightDir.fx`
SHADOWS build by inference, the cache holds no names) ends its shadow term with

```text
fade = saturate((vShadowFadeMax - |p - vShadowFadeCenter|) * vShadowFadeInvRange)  ; product of x,y,z
sun  = 1 + vSunEnvParams.x * fade * (pcf - 1)                                      ; depth > 20 m only
```

`p` is the position in sun space, so the fade region is a **square in the sun's plane**, not a
distance from the camera. Its corners show as chevrons on the ground. Under a forest canopy, shade
turns to full sun across the fade, which reads as a line. Depth of field cannot make this shape.

**Constants, set per frame by `FUN_00aec932`** (only values derived from the three cascade views):
c59/c60 offsets X/Y, c61 half sizes (fourth entry 999), c62/c63 fade centre and half size of the last
cascade, c64 inverse range `1 / (halfSize * 0.09765625)`, so the fade is 100/1024 of the half size;
z inverse range 20 (`010D1B6C`). c65.y is the depth bias, `-0.1475 / depthRange`, applied 3 times:
about 0.44 m, the same in every cascade. Forward shaders (`shader.cache`, for example `0x21E9A8`, water)
use the same registers, so they follow without changes.

**Coverage.** `FUN_009befe9` fits a sphere to each slice from the camera near value to its end; the
square has half size r = that radius. The model below assumes near ~ 0 and 16:9; the gameplay FOV was
not read. Ground reach depends on the sun and the terrain slope, and the box follows the camera, so
the line moves when the camera turns.

| Slice end | Half size, hFOV 60-80 | Forward reach | Texel at 1024 | Fade width |
|---|---|---|---|---|
| 7 m | 5-7 m | 10-14 m | ~1 cm | ~0.6 m |
| 24 m | 17-23 m | 35-46 m | ~4 cm | ~2 m |
| **64 m (last)** | 46-62 m | **~90-125 m, corners 110-150 m** | ~10 cm | ~5 m |
| 128 m (test) | 92-123 m | 185-245 m | ~20 cm | ~10 m |

Which edge the screenshot shows is **not established**. The bright-past-the-line look fits the last
cascade's fade; the apparent distance could also fit the 24/64 seam at ~40 m. A seam alone keeps the
shadows, so a seam that loses them would mean some casters skip the last cascade (SpeedTree and
foliage casters per cascade are not traced).

**Levers, none tried:**

1. **Lengths.** Each is a single `MOVSS XMM0,[disp32]` in the per-frame constructor; repointing the
   disp32 at a float in the DLL takes effect on the next frame, so a preset could drive it. Each
   pattern below matches once per build; the operands are `.rdata` constants shared with other code,
   so overwrite the load, not the constant. `SetupRenderFrameCollector` also has a caller in `FUN_00b15317`.

   | Interval | Steam VA | GOG VA | Original |
   |---|---|---|---|
   | 7 | `009C016A` | `00A111CD` | `F3 0F 10 05` + `00F11340` / `00F781F8` |
   | 17 | `009C019A` | `00A111FD` | `F3 0F 10 05` + `00F117B4` / `00F78620` |
   | 40 | `009C01AA` | `00A1120D` | `F3 0F 10 05` + `00F11328` / `00F781D0` |

   Only the last one needs to grow: the near cascades keep their sharpness, and the far one gets softer.
2. **Atlas size.** `renderer+0x20` (`009DC72F`, GOG `00A2DBED`; `C7 40 20 00 04 00 00`). The readers
   traced are `FUN_00b12e39` and `FUN_00aee02c`; others are not ruled out. The shader's 1024-based
   constants are only a half-texel clamp at the atlas edge (`0.33317`, `-1/6144`), which stays safe at a
   larger size, and the filter radius is in cascade UV (`0.007125`, `0.003135`, `0.002375`), so it
   keeps its world size. 2048 gives a 2048 x 6144 atlas and restores today's texel size at 128 m.
3. **Bias.** The filter radius in metres grows with the cascade (0.25 m today in the last one, 0.5 m at
   128 m) while the bias stays ~0.44 m, so low sun on slopes may show acne (reasoning, not tested). The
   `-0.1475` is a CPU value, so it can be scaled; its load site is not located yet.

**Test keys used below, removed since.** `ef_cascade_install` repoints the disp32s of the lengths at floats in the
DLL. It refuses unless the opcodes, the disp32s and the values 7.0, 17.0 and 40.0 all match, and rechecks
under the thread barrier. The tests stepped each zone's end with its own key.

**Seen in game (owner, Steam, DXVK, 2026-11-04): moving the end does not move the line.** The log shows
the patch installed and the end stepped through 64, 128 and 256 m many times, without a crash; the line
stayed where it was. So **the line is not the last cascade's edge**, and the fade explanation above does
not account for it. The chevron need not come from the square either: any limit in view depth or on the
ground crossing a ridge can draw the same shape.

**Second test (same setup and day): the seam does not move it either.** Ctrl+Shift+F9 and the presets only
changed how strong the line looks. Stepping the sun-shadow distances changed the shadows *past* the line,
so **shadows exist beyond it and the line is not where they end**. The owner describes it as something
odd inside a roughly circular area around Geralt.

**Third test (same setup and day): the near/far colour mod is not it.** `CEnvColorModOpaqueParameters`
blends sharpen, contrast and colour filter by view depth in a full-screen pass (`staticShader.cache` `0xD760`,
`FUN_004A92E5`): `saturate((depth - c68) * c69)`. A throwaway jump at `004AA224` cleared chosen channels of c69, so
they kept their near values everywhere. The log shows every step used; none changed the line. The values
are still worth knowing: `global_environments` has contrast 1.00 -> 0.79 over 60.8-66.6 m, and
`environment_forest` takes sharpen from `|1 - 2.43|` to 0 over 35.6-75 m.

**What the owner then described points back at the cascades.** Past the line a tree's shadow is weaker and
pixelated; at the line a stronger version is drawn in. That is two resolutions meeting, and the shader has
no blend between cascades: it picks one by `cmp` per pixel, so every seam is a hard edge. Only the end (with
a fade) and the 24 m seam had been moved, never the **7 m seam**, whose forward reach is about 10-14 m from
the camera. It also fits the earlier result: shadows past the line changed because they belong to the outer
cascades. Weaker further out is expected, since the filter radius in metres grows from about 8 cm (first)
to 12 cm and 25 cm while thin caster detail falls below a texel. Not established until the test below.

**Fourth test (same setup, 2026-11-05): it is the 7 m seam.** Ctrl+Alt+F10 steps the first end; the log shows
7 -> 14 -> 24 -> 48 m repeatedly. The owner: the line is clearly visible only at 7, gone from 14 m upwards, and
shadows get blurrier and more pixelated the higher it goes. Whether it moved or only got too faint to see
could not be told. Both match the arithmetic: the same 1024 texels and the same UV filter radius cover more
ground, about 8 cm radius at 7 m and 16 cm at 14 m.

**Fifth test (same setup, 2026-11-05): 12 m with a 2048 atlas.** The log reads back 2048, so the patch came in
time; no crash. Owner: at 7, 9 and 10 m the line only moves out; at **12 m it is gone**, with the zone fading out
naturally, but shadows are more pixelated. A likely reason it works: the filter radius at the seam, about
14 cm in the first cascade against 12 cm in the next, nearly match at 12 m.

**Built: `[shadows] quality` and a Shadow quality row in Options > Graphics, Steam and GOG.** Vanilla, the shipped
default, is the game's own values. `[shadows.high]`/`[shadows.ultra]` hold the rest, defaults 12 / 24 / 64 m, atlas 2048 / 4096, filter 0.6 /
0.5. Ctrl+Shift+F10 cycles in preset mode. All of it switches live:

* **Zones:** the three disp32s above, re-read every frame.
* **Filter:** the sample offset in the shader is the cascade's UV radius times a sample from `vPoissonSamples`,
  and `FUN_00AEC932` sets those from the CPU each frame. A bridge on that call (`00AECA7A`, GOG `00B256E0`, to
  `FUN_009D0C70`: EAX device, stack register and vector, `RET 8`) scales the frame's vector. No shader changes. One
  factor keeps the three cascades' ratio, so the seam should stay hidden. Too narrow shows the map's jagged
  edge; the 0.44 m bias is untouched.
* **Atlas:** at start, one immediate byte at `009DC72F` (GOG `00A2DBED`). Later, `wf_vp_tick_hook` walks the engine's
  own lost-device states 1, 2, 3 with the mode unchanged, and a hook on the allocator's call in the reset
  (`009DDC39`, GOG `00A2F0CD`, to `FUN_00B12E39`, stdcall, `RET 4`) sets `renderer+0x20` in step with the new atlas,
  because the per-cascade viewport reads it each frame. Needs `[window] enabled=1`; without it the size waits for
  the next start. A refused switch keeps the old size.

Hooks install from `shadow_thread` before the renderer exists, fail closed on other bytes, and do nothing at Vanilla
apart from the 1x factor. On GOG the bytes around each site were compared and its functions read before the run below.
Log: `shadows: level`, `shadows: atlas N per cascade, wanted M`, `shadows: <level>, zones`, `shadows: atlas switched`.
Script row: `9203` in `window_graphics.ws`/`window_input.ws`, id 10 in `window_texts.ws`; `OV_CACHE_LAYOUT` 11. The key has
to pass the filter in `wf_note_write`: the first build left it out, so the row changed nothing (log: no
`shadows: <level>` line), and `windowprobe` now checks every row's key. Frame cost is not measured.

**Seen in game (owner, Steam, DXVK, 2026-11-05):** on Vanilla the line is there, on High and Ultra it is gone, and
shadows look clearly better. The log shows the row switching all three levels and the atlas switching live to 4096 and
back in 2.3-2.8 s, many times in one session, with a normal exit. Log: `docs/validation/shadow-quality-billboards-2026-11-05.md`.
Native Direct3D 9 was not run with a larger atlas.

**Seen on GOG (owner, DXVK, 2026-11-05):** starts at Ultra with `atlas 4096 per cascade, wanted 4096`, zones patched, and the row and
Ctrl+Shift+F10 switch every level, about 3.4 s per atlas change; owner: seemed to work fine. That log also showed a defect:
pressing the hotkey again during a switch ended in `atlas switch refused`, with the on-screen text to restart and the old size
kept, because the end of a switch took any Want other than Now for a refused Reset. Now `g_shadowAllocRan` tells them apart, and a level
picked meanwhile gets its own switch on the next frame (`shadow_switch_end`). Second GOG run, same day: rapid hotkey presses during
switches, no refusal, and the atlas ends at the level's size. The hotkey is polled about every 120 ms, so a very quick double tap can count once.

What the code says about a larger atlas:

* The only readers of the size found are `FUN_00b12e39` (creates the 3-tall atlas, at start and on device
  reset) and `FUN_00aee02c` (the per-cascade viewport). The scan looked at `[reg+0x20]` following the
  renderer global and in the shadow functions, so a reader through another pointer is not ruled out.
* The shader's 1024-based constants are only the half-texel clamp at the edge, which stays safe. The filter
  radius is in UV, so a larger atlas removes **pixelation, not blur**: 14 m stays at 16 cm radius. Less blur
  comes from the filter factor above.
* Size: 2048 x 6144 is about 12.6 million texels, 4 x 1024's; 4096 x 12288 about 50 million. Each is a colour
  and a depth surface. With native Direct3D 9 this adds to a 32-bit process already short of address space
  (CHANGELOG 0.10.0); with DXVK it should not. Not measured.

**Options once the test is in:**

* **Move the seam out**, as little as hides the line.
* **Larger atlas**, to pay for it.
* **Blend across seams:** the real fix for a hard edge, but it means changing the shaders in `staticShader.cache`
  and `shader.cache`, or replacing them at runtime. Not looked into.

The ground texture is a separate matter. The terrain shader's level is continuous (the `frc`/`exp` pair is
only the atlas border clamp), so a hard line would need a POINT mip filter on the atlas samplers, not read
yet; `PSC_AtlasLodSize.z` caps the level, set at `009CE171` (the only writer of c41).

## Result and next step

The remaining symptoms have concrete, separate implementation leads:

1. Community NPC distance/area streaming with heading-dependent center and
   saved-state radii: inspect an affected actor before adding an optional test.
2. Sun shadows: done as Shadow quality above, off by default. Blending across seams remains open.

The successful two distance controls remain installed unchanged. No NPC cap change or forced
visibility flag was installed during this investigation.
