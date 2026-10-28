# Quality, character distance and save previews

Investigation, 2026-10-28. No runtime code, installed settings, game files or saves
changed. Static analysis: Steam 3.5.0.26, Ghidra program
`/witcher2/witcher2.exe.unpacked.exe`. Addresses are image VAs.

## Existing implementation

Installed `bin/dinput8.dll` and `build/dinput8.dll` have identical SHA-256:
`7872C4CF6F922D8252BF7471B75284FC1CC459F4AEB86D1A2FA497EDB1061C84`.

| Control | Installed value / steps |
|---|---|
| Mesh scale | 4.0; Ctrl+F9 cycles 1, 2, 4, 8 |
| Foliage scale | 2.0; Ctrl+F10 cycles 1, 2, 3, 5 |
| Gameplay mesh / character LOD downscale | both forced off |
| Display | layered-window OSD, 2500 ms |

The source enforces settings approximately every 120 ms. Hotkeys change memory
only; restarting restores the ini values. Step indices start at -1, so the first
press selects 1.0, even if the initial scale is 4.0 or 2.0. The keyboard filter
suppresses F9/F10 while Ctrl or Alt is held.

The latest Ctrl-hotkey build still needs the human check in progress.json.
There was no `bin/witcher2_enginefix.log` at inspection; the latest DXVK log ended
before the installed DLL's 12:56 build time. This does not prove a load failure.

User.ini currently has 3840x2160, UberSampling=0, mesh=4, foliage=2,
TextureMemoryBudget=1024, TextureTimeBudget=10 and 2048 spot/cube shadow maps.
User.ini and Rendering.ini remain read-only. The DXVK log identifies v3.1.1 and
RTX 5070 Ti with a 15.62 GiB local-memory heap.

## Verified mesh disappearance test

This is a concrete consumer beyond the previously known property-name strings.

`FUN_009bc8ed` constructs an object with vtable `0x00EC6CAC`. Its preceding pointer
at `0x00EC6CA8` points to RTTI complete-object locator `0x00F4EFF4`, whose type
descriptor is `0x010EB8B4`. The descriptor name at +8 is
`.?AVCRenderProxy_Mesh@@`. `FUN_00b3562c` allocates 0x150 bytes and calls this
constructor for its mesh branch.

In the constructor:

* proxy +0xF0 initially receives 4,000,000.0, a squared-distance threshold;
* `FUN_009ba848` resolves the source mesh/resource from the construction inputs;
* if source +0xF4 is positive and `FUN_009ba777` returns false, its square is
  stored in proxy +0xF0;
* mesh LOD distances separately receive settings +0x6C as a multiplier;
* later billboard/foliage branches can overwrite +0xF0. Changing just the first
  store would not cover every path.

The source field's serialized property name is **not yet proved**. Finding
`m_maxRenderDistance` in the name pool does not establish its offset or its role
in the particular disappearing NPC.

`FUN_009bade3` computes squared camera-to-bounds distance, or center distance for
a flagged path. For a camera parameter below 50 it also adjusts effective distance
using `53 / parameter`; this is not a pure world-distance test in all camera modes.

Verified assembly:

```text
009BB09F  F3 0F 10 83 F0 00 00 00  MOVSS XMM0,[EBX+0xF0]
009BB0A7  0F 2F C6                 COMISS XMM0,XMM6
009BB0AA  0F 86 D2 01 00 00        JBE 009BB282
```

When the threshold is reached, one branch writes -1 to proxy +0xF4 at
`0x009BB344`, clears transition flags and selects the end of the LOD range.
A fading branch can transition there instead. Inside the threshold, the function
selects a mesh LOD using the separate table at +0xFC.

The routine also contains a shadow-related distance gate: when proxy +0x9C bit 0
is set and selected LOD is not -1, proxy +0x140 indexes squared thresholds
`[4, 25, 1225, 4900, 40000, 1000000]`, updating +0xC0 with a fade. Its connection
to the serialized shadow-distance enum and the user's shadow cutoff still needs
verification. It is not a proven sun-shadow cascade control.

Next: trace an affected NPC's body and attachments into this proxy, recording its
threshold, LOD table, flags and lifetime around disappearance. Check whether actor
streaming removes it before the test fires. The script-facing
`CNewNPC::VisiblityTest` wrapper at `0x00977AFB` is a separate visibility-query path.

A future experimental control should scale the verified threshold for intended
characters while preserving normal LOD selection. A linear range multiplier k
requires a squared-threshold multiplier k*k. Character classification must come
first: scaling all mesh proxies also extends vegetation and world geometry.
Compare 1.0, 1.5 and 2.0 at the same position, measuring frame time. This cannot
make an actor visible after it has been unloaded. No such patch was implemented.

## Save previews: missing files confirmed, DXVK attribution open

Follow-up investigation: [save-previews.md](save-previews.md). The newer
ManualSave_0006 also lacks a sidecar. An isolated 4K readback/downsample/BMP
probe now succeeds with both installed DXVK 3.1.1 and native D3D9, producing
identical output; an in-game storage-backend test is still needed.

The supplied screenshot selects QuickSave at 2026/10/28 01:09. Active save folder:

`C:/Program Files (x86)/Steam/userdata/<steamid>/20920/remote`

There are seven .sav files: AutoSave_0000, ManualSave_0001 through 0005 and
QuickSave. Only AutoSave_0000 has its matching `_640x360.bmp` sidecar. Documents'
gamesaves folder is empty. Searches found no other sidecars under Documents/Witcher 2
or the game install.

Steam's `C:/Program Files (x86)/Steam/logs/cloud_log.txt` records a failed
`FileRead()` for `QuickSave_640x360.bmp` at **12:05:39**, ten seconds before the
supplied screenshot, naming this exact remote directory. Similar failures occur
at 01:43 and in later sessions. Steam lists a pending deletion for an older
QuickSave BMP; that does not prove Steam caused its loss. Sync lines say
`Sync Disabled`, while User.ini has `CloudStorage=true`. Local use of Steam's
storage backend and cloud upload/download synchronization are separate facts.

The surviving BMP decodes as a normal 640x360 24-bit image. It is byte-identical to
`CookedPC/globals/gui/savescreens/l01.bmp`, SHA-256
`798F7D64F712E133C4AF85D143EE1B0B78E4E330DDAC3D5E8728738A77740E9F`.
It is a stock fallback scene, not proof that live capture works in this setup.

Relevant engine paths:

| Function | Observed behavior |
|---|---|
| `009B70DA` | Builds `_%ix%i.bmp` suffix with 640 and 360 |
| `009B6BC5` | Schedules capture into the save-manager buffer |
| `009B747E` | Schedules capture/callback or copies a stock l0N.bmp |
| `009B733A` | Transfers result through storage backend and removes temporary data, conditional on storage state |
| `00AA1C4B` | Creates a 640x360 capture request |
| `00B15317` | Render loop invokes `00B12D8F`, then completion callback |
| `0053CEF0` | System-memory surface, GetRenderTargetData, LockRect, CPU downsampling, D3DX BMP encoding to file or memory |

DXVK could affect GPU readback. Missing output could also arise in scheduling,
encoding, storage transfer or cleanup. No failed HRESULT has been captured and
no native-D3D9 comparison performed. The gradient's exact fallback-texture origin
has not been traced. The strong finding is the missing requested file, not proof
of corrupt saved pixels or a DXVK bug.

Minimal next experiment (not yet performed; human launch required):

1. Load a save in the current setup and create a **new manual slot**. Immediately
   inspect the new .sav and matching BMP, preserving files/logs.
2. Repeat at the same position with native D3D9, temporarily moving only the local
   bin/d3d9.dll aside while the game is closed, then restoring it.
3. If only DXVK fails, instrument capture HRESULTs and the completion callback.
   If both fail, prioritize the common save/storage path.

Do not disable cloud storage or relocate the owner's saves as the first test.
Recreating a missing BMP cannot recover the old camera view.

## Corrected shader-cache lead and quality priorities

The older notes proposed rebuilding shader caches because of repeated
`Bytecode does not match shader stage` messages. Upstream DXVK
[#1488](https://github.com/doitsujin/dxvk/issues/1488) identifies this Witcher 2
behavior as a game bug with no performance effect. These messages alone prove
neither missing effects nor corrupt shader caches.

The same issue explains that UberSampling renders the scene four times with
subpixel offsets, adding CPU work as well as GPU work. Keep it off here.

Recommended order:

1. Confirm the installed Ctrl-hotkey build in game; keep mesh=4 and foliage=2 as
   the comparison baseline.
2. Diagnose one new save's thumbnail using the A/B protocol.
3. Trace a disappearing NPC into the verified proxy cutoff, then design an optional
   character-range multiplier if that test is the limiting factor.
4. Investigate the shadow-distance gate separately. Raising spot/cube shadow-map
   resolution does not extend its distance and may not affect sun shadows.

## In-game follow-up, 14:25

The owner confirms Ctrl+F9 and Ctrl+F10 work as expected. The runtime log now
exists and records mesh cycling through 1/2/4/8 and foliage through 1/2/3/5,
with matching enforcement messages. This supersedes the earlier missing-log and
pending Ctrl-test observations. Plain F9 pass-through was not separately reported.

The last logged changes before the supplied images are mesh=8 at 14:18:54 and
foliage=5 at 14:19:26. No intervening scale changes appear before the images at
14:24:59.95 and 14:25:11.74. These images therefore appear to show the problem
even with these high LOD multipliers, rather than at the ini baseline 4/2.

At the indicated tent edge, a person and a broad dark ground-shadow region are
visible in the nearer view and absent in the farther view. Player/camera position
changes and approximately twelve seconds elapse, so the pair is a useful scene
reference, not a controlled proof that character and shadow share one cutoff.
Disregard the small unintended annotation arrow.

Useful next observation: hold position and camera still at the farther spot,
cycle only mesh between 1 and 8, and record separately whether the whole person
and the ground shadow appear. If neither changes, approach and retreat over the
boundary to check that the transition repeats. Leave foliage fixed. A new manual
save remains the separate thumbnail-generation experiment.

Higher texture-memory budgets and extreme global LOD scales are not automatic
wins on a 16 GB GPU: this is still a 32-bit process with CPU/draw-call and virtual
address-space constraints. Measure a fixed scene before changing the baseline.
