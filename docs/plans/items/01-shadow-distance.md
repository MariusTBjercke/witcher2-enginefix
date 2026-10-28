# Item: shadow draw distance

## Current experiment (2026-10-28)

An optional mesh visibility / caster-distance experiment is implemented and
installed locally, with both new factors initially 1.0. Distribution defaults off.
Native probe: 27 checks pass. Independent code review completed after correcting
the thread-creation race in installation. No in-game startup or visual success
is claimed. Next action: owner tests Ctrl+Shift+F9/F10 at the supplied tent scene.
See [exact sites, controls and rollback](../../fixes/render-distance-experiment.md).
The older research directions below remain relevant if the caster gate does not
affect the observed shadow cutoff; no per-light/sun-cascade fix is established.

## Why

The owner reports shadows and characters vanishing at roughly the same distance, which
is what makes the world feel small. LOD is solved; shadow range is the counterpart on
the lighting side.

## What is known

There is **no ini key** for shadow distance. The full settings key pool contains only
quality/count entries: `ShadowQuality`, `ShadowedLights`, `MaxCubeShadowSize`,
`MaxCubeShadowCount`, `MaxSpotShadowSize`, `MaxSpotShadowCount`, `m_shadowQuality`,
`DF_CastShadows`, `DF_CastShadowsWhenNotVisible`, `DF_ForceNoShadowLOD`.

The distance itself is per light / per caster, serialised in `.w2l` / `.w2ent`:

| Property | Role |
|---|---|
| `m_shadowRenderingDistance` | per-light range |
| `m_overrideMeshShadowRenderingDistance` | per-mesh override |
| `m_shadowDistanceScale` | per-mesh scale |
| `EShadowRenderingDistance` | enum mode |
| `m_sunShadowDepth`, `EGlobalShadowMode`, `m_shadowWithScale` | sun / global |
| `CEnvShadowsParameters` | level environment block |

These names live in the CName pool, so **they have no static xrefs**. Do not try to
follow the string.

## How to attack it

1. In Ghidra (`/witcher2/witcher2.exe.unpacked.exe`, 39 339 functions), find the code
   that computes a shadow-map frustum or culls a caster by distance. Useful anchors:
   the D3D9 light/shadow setup paths, and the renderer object reached from
   `DAT_0160c6ec` (the renderer interface touched during startup in `FUN_00aa6e94`).
2. Look for the multiply by a per-light float around the shadow projection - the same
   shape as the mesh LOD multiply we already found for `[settings+0x6C]`.
3. Decide between: scale at the consumer (one global factor, hotkey-tunable) or patch
   each light when the level loads.
4. Verify at one fixed spot, day time, same camera: capture before/after frame and
   compare where the shadow edge ends.

## Traps

* `m_*` property names have no xrefs; the sunchild is the *getter* by property id.
* Per-level data means a fix that works in the prologue may not work in Flotsam.
* Shadow range and shadow resolution are independent - do not raise `MaxSpotShadowSize`
  to fix range.
