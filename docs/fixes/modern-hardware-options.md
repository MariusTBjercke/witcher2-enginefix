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

## Rejected

**Mouse smoothing off: do not set `MouseSmoothness=0`.** An earlier version of this file proposed
it. Steam 3.5.0.26, read from the unpacked binary, not tried in game:

* `MouseSmoothness` is `+0x0C` of `SInputUserConfig` (default 1.0, `FUN_00a94043`; read by
  `FUN_00a950e4`), which sits at `+0xE4` of the input object: `+0xF0` in `FUN_00a943d6`, where
  `+0xE8`, `+0xEC`, `+0xFC` and `+0x100` line up with the other keys.
* There it is a multiplier, not a filter strength: `speed = |target - previous| * MouseSmoothness`
  (`MULSS XMM0,[ECX+0xF0]` at `00A94443`) feeds the `GI_MouseX` output (key index `0x100`). 0
  makes that output always 0. `playeraimedthrow.ws`, `prisoner.ws` and `wristwrestling.ws` read
  `GI_MouseX`.
* The camera does not read that output. `quest_functions.ws` labels `GI_MouseDampX` "camera X-axis
  mouse", and `FUN_00a943d6` copies that path (`+0xC8`, `+0xCC`) straight through while
  `UseMouseDamp=false`. So the camera has no smoothing from these keys to turn off.
* `Smoothness` has no whole-word UTF-16 string in the exe, so the engine most likely ignores it.

Not excluded: the scan for `+0xF0` covered `00A80000-00AB0000`, and the camera may smooth on its
own further on.

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
external limiter instead in order to avoid poor frame pacing.` Measured 2026-11-04 with
PresentMon 2.6.0 (`tools/frametime/`), owner's machine: DXVK 3.1.1, RTX 5070 Ti, 3840x2160,
extreme preset, G-Sync on with the driver's indicator green, 60 s per run at one outdoor spot
(stand still, turn, walk):

| Setup | Runs | fps | Frames held 2+ refreshes /s | Never displayed /s | Present jitter (ms) | Present to screen (ms) |
|---|---|---:|---:|---:|---:|---:|
| Cap 72, VSync off, borderless, CPU-heavy install running | 3 | 68.9-70.1 | 5.5-8.2 | 1.4-3.2 | 0.83-1.88 | 23.5 |
| Cap 72, VSync off, borderless | 2 | 71.7-71.9 | 3.6-3.8 | 1.2-1.3 | 0.87-0.98 | 23.9 |
| Cap 72, VSync off, full screen | 2 | 71.7-71.9 | 3.4-3.9 | 0.9-1.3 | 0.88-0.96 | 24 |
| No cap, `d3d9.presentInterval=1`, borderless | 1 | 74.3 | 1.5 | 1.3 | 0.10 | 26.7 |

* Every frame in every run is `Composed: Copy with GPU GDI` with `PresentRuntime=Other`, also
  in full screen (`Windowed: false`) and with the driver's Vulkan present method on *Prefer
  layered on DXGI Swapchain*. Why the driver takes this path is open.
* Display times sit on a fixed ~13.4 ms grid (13.4 / 26.8 / 40.2 ms), not on the 13.9 ms the cap
  delivers, despite the G-Sync indicator. The held-frame rate under the cap matches the gap
  between that ~74.6 Hz grid and the frames shown. PresentMon sees the composition step, not the
  panel, so this does not prove what the panel did.
* The GPU is not the limit (p99 ~11.5 ms), and without the background install the game delivers
  evenly, so preset, streaming and CPU are not suspects at this spot.
* VSync without the cap is the smoothest measured, at ~3 ms more latency, from one run. The
  remaining ~1.5 held frames/s also occur when standing still, so they are not caused by load.

**Shader-stage messages — investigated.** The repeated
`err: CreateShaderModule: Bytecode does not match shader stage` messages are known
Witcher 2 behavior. A DXVK developer identifies the cause as the game supplying the
wrong shader stage and says it has no performance effect
([upstream #1488](https://github.com/doitsujin/dxvk/issues/1488)).
These messages alone are not evidence of missing effects or bad shader caches;
the earlier cache-rebuild suggestion is withdrawn.

**Leave alone:** `UberSampling` (4× SSAA - brutal at 4K and a known cliff with DXVK),
`AllowSSAO`/`AllowShafts` (already on), anisotropic filtering (DXVK does 16×).
