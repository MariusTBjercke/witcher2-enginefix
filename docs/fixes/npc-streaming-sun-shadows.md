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

## Result and next step

The remaining symptoms have concrete, separate implementation leads:

1. Community NPC distance/area streaming with heading-dependent center and
   saved-state radii: inspect an affected actor before adding an optional test.
2. Fixed three-cascade sun-shadow coverage and 1024-per-cascade resources:
   audit the matching coordinate/filter math before exposing a quality control.

The successful two distance controls remain installed unchanged. No higher
default, NPC cap change, forced visibility flag, or unverified shadow-map resize
was installed during this investigation.
