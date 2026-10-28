# Modern-hardware candidates

Survey of what is left to squeeze out of the engine, with the evidence for each.
Ordered by value / effort.

## Applied

| Item | Evidence | Effect |
|---|---|---|
| `TextureTimeBudget=10` | present in **all four** `bin/config/profiles/PC*.ini`, absent from the live `User.ini` | streamer time slice; 10 is the Uber value, the only `[Rendering]` key we were missing |
| `TextureMemoryBudget=1024`, `MaxTextureSize=4096`, `MaxResidentMipMap=11`, texture groups 4096 | shipped defaults are 400/2048/6 | the single biggest visual win at 4K |
| LOD multipliers `MeshDistanceScale=4.0`, `FoliageDistanceScale=2.0` | presets cap at 1.0; readers do not clamp | kills most pop-in |
| both `*LodGameplayDownscale` = 0 | presets use 1 on Low/Medium | removes the extra gameplay-time LOD cut |

## Proposed, needs a taste call

**Mouse smoothing off.** `User.ini` currently has `MouseSmoothness=1` and
`Smoothness=1`; `UseMouseDamp=false` so `MouseDampX/Y=7000` is dormant. On a modern
mouse the smoothing filter mostly adds lag. Setting both smoothness values to 0 is
the usual "modern feel" tweak, but it changes a feel the player has already adapted
to, so it should be an explicit choice rather than a silent default.

## Open, needs code work

**Shadow draw distance.** Not an ini setting anywhere in the key pool. Lives per light
in `m_shadowRenderingDistance` / `m_overrideMeshShadowRenderingDistance` /
`m_shadowDistanceScale` / `EShadowRenderingDistance` / `m_sunShadowDepth` /
`EGlobalShadowMode`. The values are read through the CName property system, so there
is no static xref to follow - the consumer (shadow-map pass) has to be found from the
renderer side.

**Character cull distance.** A separate mesh-proxy squared-distance cutoff is now
verified in `FUN_009bade3`, using proxy `+0xF0`; see the
[2026-10-28 investigation](quality-audit-2026-10-28.md). Its effect on the specific
disappearing NPC and its mapping to `m_maxRenderDistance` remain unverified.
Actor streaming is another possible limit.

**Frame pacing.** DXVK logs `Built-in frame rate limiter enabled. Please enable an
external limiter instead in order to avoid poor frame pacing.` The current cap is
`d3d9.maxFrameRate=72` against a 74 Hz panel. Two alternatives worth measuring:
`VSync=1` in `User.ini` with the cap removed, or keep the cap and accept it.

**Shader-stage messages — investigated.** The repeated
`err: CreateShaderModule: Bytecode does not match shader stage` messages are known
Witcher 2 behavior. A DXVK developer identifies the cause as the game supplying the
wrong shader stage and says it has no performance effect
([upstream #1488](https://github.com/doitsujin/dxvk/issues/1488)).
These messages alone are not evidence of missing effects or bad shader caches;
the earlier cache-rebuild suggestion is withdrawn.

**Leave alone:** `UberSampling` (4× SSAA - brutal at 4K and a known cliff with DXVK),
`AllowSSAO`/`AllowShafts` (already on), anisotropic filtering (DXVK does 16×).
