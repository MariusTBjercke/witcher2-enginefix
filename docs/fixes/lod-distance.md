# Pop-in / draw distance (LOD)

**Goal:** eliminate the short-range foliage pop-in that is visible in the prologue
today, without changing art direction or adding content.

**Status:** ini-level attempt applied, awaiting in-game verification. Code path not
yet located in the disassembly (Ghidra analysis still running at time of writing).

## Symptom

Bushes and general foliage appear/pop in at short range; larger vegetation holds up
better. Unrelated to texture quality — this is geometry/LOD visibility, not texture
streaming.

## What the install already has

* Nexus *Extreme Vegetation Rendering Distance* (mod 898) — replaces 296 `.w2mesh`
  files, raising per-mesh LOD distances for a subset of vegetation.
* Nexus *AI Upscale Textures* — irrelevant to pop-in (textures only).

Pop-in surviving that mod means the bottleneck is either the global multiplier, or
per-instance visibility that the mesh replacement does not touch.

## Attempt 1 — ini knobs (applied 2026-10-28)

Source of truth: `bin/config/profiles/*.ini` shows the shipped ranges.

| Preset | MeshDistanceScale | FoliageDistanceScale |
|---|---|---|
| PCLow | 0.7 | 0.5 |
| PCMedium | 0.8 | 0.6 |
| PCHigh | 0.9 | 1.0 |
| PCUber | 1.0 | 1.0 |

Applied to `%USERPROFILE%\Documents\Witcher 2\Config\User.ini`, `[Rendering]`:

```ini
FoliageDistanceScale=2.0          ; key did not exist in the file before
MeshDistanceScale=2.0             ; was 1.2
CharacterLodGameplayDownscale=0   ; explicit
MeshLodGameplayDownscale=0        ; key did not exist in the file before — prime suspect
```

`MeshLodGameplayDownscale` was found by dumping the settings string pool in `.rdata`
(`0x00A42000`-`0x00A46000`, 470 strings). It is the mesh counterpart of
`CharacterLodGameplayDownscale`, which the presets set to `1` on Low/Medium and `0`
on High/Uber — i.e. `0` means "do not downscale LOD during gameplay". Neither key
was present in the live `User.ini`, so the engine was almost certainly applying its
own default, which is the most likely explanation for foliage popping in despite the
LOD slider already sitting at 1.2 (above PCUber's 1.0).

Both files are marked read-only so the launcher and the in-game options menu cannot
overwrite them; the game rewrote `User.ini` on every launch before this.

**Why 2.0:** every shipped preset caps at 1.0, and the launcher exposes no control
for `FoliageDistanceScale` at all, so the engine's tolerance for values above 1.0 is
unknown. 2.0 is the smallest step that should produce a clearly visible difference
if the value is honoured unclamped.

### Expected outcomes

| Observation | Interpretation |
|---|---|
| Pop-in noticeably better | multiplier honoured; tune upward, then find the clamp in code |
| No change at all | value clamped to 1.0, or key read from a different section/name |
| Change in mesh LOD but not foliage | `MeshDistanceScale` works, `FoliageDistanceScale` is not the foliage path |
| Worse than before / artifacts | overshoot; back off and inspect the clamp |

## Observation log

| Date | Grid | Observation |
|---|---|---|
| 2026-10-28 01:42 | `FoliageDistanceScale=2.0`, `MeshDistanceScale=2.0`, both downscales `0` | **Foliage holds noticeably longer.** Soldiers' shoulder pauldrons still switch to low LOD at short range. |
| 2026-10-28 12:20 | same, via `dinput8.dll` enforcement | F10 visibly changes foliage, but only at specific steps; the active value was not visible to the player. **F9 is quick load in game** and could not be used for the mesh steps. |

Fixes applied after that report:

* mesh cycling moved from F9 to **F7** (`[hotkeys] cycle_mesh`);
* an on-screen readout (topmost layered window, `[osd]`) shows the active value and the
  step index for ~2.5 s after each press, e.g. `Foliage LOD 2.0 (2/4)`;
* the same readout confirms at startup (`LOD fix active mesh 4.0 foliage 2.0`).

If the readout does not appear the game is presenting in true exclusive fullscreen,
which a DWM-composited topmost window cannot cover — then the only option is W1's
approach, drawing inside the D3D scene by hooking `IDirect3DDevice9::EndScene`.

Two things changed at once, so the observation does **not** isolate which key did the
work. `FoliageDistanceScale=2.0` is the likely cause, but `MeshDistanceScale` 1.2→2.0
cannot be ruled out yet.

The pauldrons matter because they are *character* sub-meshes, not foliage: if the
global `MeshDistanceScale` is applied to character meshes too, raising it should push
their LOD switch out. If it is not, characters take a separate path and the lever is
somewhere else.

### Experiment 2 (applied 2026-10-28 01:50)

```ini
MeshDistanceScale=4.0     ; was 2.0 — deliberately extreme, to make the effect obvious
```

`FoliageDistanceScale` stays at 2.0 so foliage is a control.

| Result | Conclusion |
|---|---|
| Pauldrons hold high LOD much longer | `MeshDistanceScale` drives character meshes; settle on a tasteful value (2.5–3.0) |
| No visible change on pauldrons | character LOD does not read `+0x6c`; needs a code-level fix or mesh-data edit |
| Whole scene gets heavy / artifacts | value works but overshoots; back off |

## Next step — code side

1. Locate the config registration for `FoliageDistanceScale` / `MeshDistanceScale`
   (xrefs to the UTF-16 literal in `.rdata`).
2. Find the clamp/validation around them — this decides whether ini values above
   1.0 survive into the renderer.
3. Trace where `FoliageDistanceScale` is multiplied into a distance (foliage
   container build vs. per-instance visibility test).
4. If the clamp is the blocker, patch the clamp in memory via a proxy DLL rather
   than shipping a modified exe.

## Verification protocol

Change one knob per run, same save/position, look at the same bush at the same
distance. Note the value and the observation in this file.
