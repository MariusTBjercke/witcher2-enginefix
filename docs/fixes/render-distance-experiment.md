# Experimental visibility and mesh-shadow distance

Steam 3.5.0.26, 2026-10-28. In-game startup and both controls are now confirmed
by the owner: most mesh-shadow pop-in improves substantially. Remaining NPC
disappearance and a near/far shadow-quality boundary are under separate
[investigation](npc-streaming-sun-shadows.md). Complete removal of pop-in is
not claimed.

## Controls

The current interface groups these controls into [graphics presets](graphics-presets.md),
with Ctrl+F10 cycling all four factors and ini `default=extreme`. Presets activate
the distance hooks automatically. The table below describes the optional legacy
mode: set `[presets] enabled=0` and `[distance] enabled=1` before startup. This
mode uses the individual ini factors (1.0 initially). The original experimental
release defaulted to disabled; the owner has since confirmed both controls.

| Keys | Control | Steps |
|---|---|---|
| Ctrl+F9 | Existing mesh LOD | unchanged |
| Ctrl+F10 | Existing foliage LOD | unchanged |
| Ctrl+Shift+F9 | Hard mesh visibility range | 1, 1.5, 2, 3, 1 |
| Ctrl+Shift+F10 | Mesh shadow-caster distance gate | 1, 1.5, 2, 3, 1 |

The new controls have their own OSD messages and do not change LOD settings.
No keyboard COM hook or suppress_mod_keys behavior was changed. Values are live
for the session and reset to `visibility_scale` / `shadow_scale` on restart.
Valid ini factors are 1 through 4; invalid/nonfinite values fall back to 1.

Visibility applies to **all CRenderProxy_Mesh objects**, not a verified
character-only subset. It cannot extend actor streaming, spawning or AI range.
The shadow control scales one mesh-caster gate; it does not enlarge sun-shadow
cascades or per-light projection coverage. Either control may have no effect on
the specific missing object if a different limit acts first.

## Verified code sites

Both are inside `FUN_009bade3`. See quality-audit-2026-10-28.md for class identity,
threshold producers, camera-distance adjustments and analysis limitations.

| VA / RVA | Original load | Fingerprint including following compare/branch |
|---|---|---|
| 009BB09F / 005BB09F | `MOVSS XMM0,[EBX+F0]` | `F3 0F 10 83 F0 00 00 00 0F 2F C6 0F 86 D2 01 00 00` |
| 009BB465 / 005BB465 | `MOVSS XMM0,[ESP+EAX*4+30]` | `F3 0F 10 44 84 30 0F 2F C6 76 07 B0 01` |

The first 8 / 6 bytes are replaced by an E9 relative jump plus NOP padding.
Compiled x86 naked bridges reproduce the load, multiply XMM0 by factor squared,
and jump to the original comparison at site+8 / site+6. Other SSE registers,
integer registers, EFLAGS and the stack are preserved. Original mesh fields and
LOD tables are not rewritten; there is no accumulating scale across frames.

The range bridge also counts calls, baseline distance rejections and cases newly
admitted by the multiplier. The shadow bridge counts calls. Every five seconds
the worker logs interval totals. These totals cover all meshes and do not identify
the target NPC. The instrumentation adds overhead; this is a diagnostic build.

Installation waits for the settings singleton, validates an x86 PE and both code
fingerprints, and pins the DLL so installed jumps cannot target an unloaded module.
The installer resolves native `NtGetNextThread` before stopping peers, then
repeatedly enumerates and suspends newly discovered threads until a complete pass
finds none. This covers children created while earlier passes were stopping their
creators. Enumeration uses kernel calls without snapshot/heap allocation under
suspension. Patching aborts if a peer cannot be stopped or its instruction pointer
is inside an overwritten span. Both fingerprints are rechecked under the barrier.
The API signature was checked against the primary
[System Informer header](https://github.com/winsiderss/phnt/blob/master/ntpsapi.h).
Both sites share a page; protection restoration handles this nesting. Threads are
resumed before logging/allocating. Concurrent external thread injection is outside
this startup-only protocol. Unknown byte patterns leave both sites untouched.

## Verification

`tools/distanceprobe/build-and-run.bat` builds a CFG-enabled x86 executable and
executes the same bridge/installer code against synthetic sites containing the
verified original instructions. Checks include unmodified baseline branches,
exact boundaries, factor-squared behavior, register/stack preservation, shadow
tier indexing, independent controls, unchanged proxy data, telemetry, invalid
factors, repeat installation and rejection without partial patching.

The first run failed with `distance hook implementation is absent`; after the
implementation the probe passed 20 checks. Independent review found and corrected
a single-snapshot thread-creation race; the expanded probe passes **27 checks**,
including a peer parked inside an overwritten instruction, balanced suspension
counts and shared-page RX protection after refusal and success. The revised
installer was independently reviewed and the probe independently rerun.
`build.bat` also built the DLL.
Nine additional checks run the actual DLL hotkey handlers with deterministic
keyboard-state input, including releasing/reapplying Shift while F9/F10 remains
held. Physical key edges prevent a shifted distance press from also changing LOD.
All nine pass. No COM keyboard filter code was changed.
These are machine-code/installer checks, not proof of an in-game visual fix.

The local test build was installed with `[distance] enabled=1`, both scales=1.
Installed DLL SHA-256:
`8E51C92924F0A2B04FD1A922F00D87EC53428694AEDB3A22C4F85A8D4A8811C7`.
The installed game exe retained its original SHA-256
`CB9FBC9C1DC7AE0AC73C5768E55724CC8E7B78999299F0A2E6E5AB7D74A55FE8`.
Previous DLL and installed ini are in build/before-distance/dinput8.dll and
build/before-distance/installed.ini. No game start has been performed by the agent.

## Human test

1. Start through Steam and load the tent scene. Confirm the game starts and the
   new controls show `Visibility range` and `Shadow range` messages. If they do
   not, inspect the log for `distance: hooks installed` versus install refusal.
2. Leave existing mesh/foliage settings fixed. At the farther screenshot position,
   keep the camera still and cycle **only Ctrl+Shift+F9** through 1/1.5/2/3.
   Record whether the missing person appears, and any FPS change.
3. With visibility high enough for the person to exist, cycle **only
   Ctrl+Shift+F10** and inspect the ground shadow independently.
4. Return both controls to 1 to check reversibility. If neither helps, telemetry
   and the repeatable scene guide the next actor-streaming / sun-shadow analysis.

## Revert

Set both `[presets] enabled=0` and `[distance] enabled=0`, then restart to remove
both code hooks. Factor 1 restores
the distance calculation while hooks remain installed; disabling the section
removes hook overhead at the next startup. The previous DLL and installed ini
are retained in `build/before-distance/`. No game executable on disk is changed.
