# Interior preloading: can doors be seamless?

Status 2026-11-02: shipped in 0.8.0 as `[loading] interiors=2` (default). The sections below
follow the work in order; the run logs are in `docs/validation/`. Vergen (Act 2) is covered from
0.12.1: built 2026-11-11, checked offline for all 24 houses and seen in game for four of them
(Steam, DXVK); Act 1 was seen unchanged with the same build. See the last section.

## Question

Fast doors cut a house door to ~0.65 s on screen, of which ~0.1 s is loading. Can interiors be
loaded together with the level, or preloaded when the player is near a door, so the black
transition goes away?

## How interiors are organised

Interiors are not separate levels. A world (`CWorld`) has a `CWorldPartitions` resource
(`.w2part`, `CWorld::m_worldPartitions` / `m_startupPartitionName`). Each `SWorldPartition` has
a name, `m_environmentType`, `m_locationType` (`LT_DEFAULT/INDOOR/OUTDOOR/CAVE`), `m_hasWindows`,
`m_soundAmbient`, `m_musicCue`, `m_reverb`, **`m_layerGroups`** and **`m_dependencies`**. Exactly
one partition is current; its layer groups (plus dependencies) are loaded, the rest are not.

A `CStreamingDoor` has `m_inside` / `m_outside` (`SDoorPartition`: partition name + transition
type `EStreamingDoorTransitionType`). Using the door changes the current partition.

## The partition switch

* `FUN_00aa7e18(name, mode, loadingScreen)` queues a change on the game object (array at
  `DAT_01404950+0xE4`). Callers: script `StreamWorldPartition` (native `00AA87FC`, **mode 0**),
  script `LoadWorldPartition` (`00AA8855`, mode 1), quest block `FUN_00967b02` (mode from the
  block), house doors (mode 4).
* `FUN_00a91bf9` (`"Streaming PH: "`) runs it on the world:
  * `FUN_00a91269` diffs the current partition's layer groups against the target's: a list to
    unload and a list to load.
  * `FUN_00a90fc4` builds a job (`0x58` bytes) from both lists; the load list is split into 5
    buckets by the layer's priority (`layer+0x44`).
  * **mode 0 (stream)**: the job is stored in `world+0x30C` and runs asynchronously over the
    next frames; `world+0x320` (current partition) is cleared until it is done. `IsStreaming`
    (`00AA723F`) is `world+0x30C != 0`.
  * **any other mode**: the job runs to completion now (`FUN_00a91952`, `FUN_00a91340`) and
    `world+0x320` is set. Modes other than 4 show the loading screen (game vtable `+0x108` /
    `+0x10C`) when there is something to load; **mode 4 (doors) never does**.
  * A new request while a stream job runs finishes that job synchronously first, then recurses.
* `FUN_00a90e00` returns the partition being streamed (job `+0x2C`) or the current one
  (`world+0x310`); the door tasks compare against it.

## Door transition types (task created in `FUN_0095dcc4` from `SDoorPartition` type)

| Type | Enum | Behaviour |
|---|---|---|
| 0 | `SDTT_LoadingScreen` | waits for the anim event `flush_streaming` (`FUN_005029cf`), then mode 1 (sync, loading screen) |
| 1 | `SDTT_BlackScreen` | fade out, mode 4 (sync), fade in on `fade_out`. **All house doors seen so far** |
| 2 | `SDTT_WaitOnKnob` | constructor `FUN_00502d3e` starts **mode 0 (async)** immediately unless the target is already current/streaming; Tick `FUN_00502dc0` waits for `world+0x30C == 0` and `FUN_00502ed8`; fades to black (0.3 s) only after 2.5 s |
| 3 | `SDTT_AsyncNoBlock` | done at once if the target is already current; otherwise starts on the anim event `start_async` |

So the engine already has asynchronous partition streaming and door types that use it; CDPR
chose the synchronous black-screen type for houses.

## What preloading would and would not buy

* Memory is not the constraint it was in 2011: the exe is LAA (4 GB on 64-bit Windows) and a
  house is ~25-40 MB of reads (`load-times.md`, run 2), an inn ~300 MB.
* **Preload only** (load the interior's layer groups before the door, keep the black fade):
  the switch's load list would be empty, saving the 110-140 ms of a house and ~0.3-0.9 s of
  an inn. The fade stays, because the switch still unloads the exterior and changes
  environment, sound and music, and that must not be seen.
* Mode 0 is not a preload: it is a full switch, so the exterior is unloaded when it completes
  even if the player never goes in. A real preload needs a "load these layer groups, unload
  nothing" call, e.g. the switch with an emptied unload list, or the world's layer group
  show path (`ShowLayerGroup`, `CWorld` scripted function).
* **No fade at all** needs both worlds visible at once (never unload the exterior, or keep
  every partition's layer groups loaded). Unknowns that decide whether that can look right:
  1. Are interiors at the house's position (inside the shell) or elsewhere in the world? If
     elsewhere, the door teleports and a fade is unavoidable.
  2. Exterior and interior meshes overlapping (the shell drawn around/through the interior).
  3. The environment/lighting switch (`m_environmentType`, indoor vs outdoor) is per
     partition and would pop without a fade.
  4. Layers contain NPCs, communities and quest entities; keeping them attached changes
     gameplay (AI ticking, spawns) and possibly what goes into a save. Highest risk.

## Possible next steps (none started)

1. **Diagnostics first** (passive, `load_trace`; built, see "Partition trace" below): per switch log the from/to partition names,
   the number of layer groups loaded/unloaded and their names, and the player position at the
   start and end of the door task. Answers unknown 1 and shows what each door actually swaps.
2. **Experiment: keep the exterior** (ini flag, off by default): empty the unload list in the
   diff for mode 4 so leaving a house reloads nothing and the exterior stays in memory while
   inside. Measures whether the way back out becomes instant, and what breaks.
3. **Experiment: async house doors**: override type 1 -> 2 in `FUN_0095dcc4` for houses, so
   streaming starts when Geralt touches the knob and no fade happens if it finishes in time.
   Expect a visible pop when the exterior unloads; only worth it if 1 shows co-located
   interiors.
4. **Proximity preload**: needs 1 and 2, plus finding streaming doors near the player each
   tick. Most work; only after 2 shows that loaded-but-unused layer groups are harmless.

## Partition trace (`[diagnostics] load_trace=1`)

`src/enginefix.c`, section "partition trace". Installed by the load trace thread once the
SteamStub has decrypted `.text` (fingerprints on `FUN_00a91bf9` and `FUN_00a91952`):

| Hook | Where | Logs |
|---|---|---|
| partition switch | calls `00AAA1FF` (request queue) and `00AAB089` -> `FUN_00a91bf9` | from/to partition, mode (`stream`/`load`/`door`), player position, time; for mode 0 the async job's lists |
| sync job run | call `00A91DAD` -> `FUN_00a91952` | the job's unload list and load buckets with each layer's name (layer info `+0x78`, max 80 names), run time |
| door task start/end | existing `hook_DoorTimer` | player position (`CWitcherGame+0x250` -> `+0xA0`) and door position (`+0xA0`) |

Log lines start with `partition:`. What to look for: whether the player position jumps
between "door task starts" and "door task ends" (interior elsewhere -> teleport), and which
layers a house swaps (only the house, or large parts of the town).

### Run 5 (2026-11-02 20:14, owner)

`docs/validation/partition-trace-run5-2026-11-02.log`. Two houses outside Flotsam in and out,
the Flotsam gate, the inn in and out. Fast doors on (0.25 s fades). No crash.

| Door | Switch | Unload | Load | Sync job |
|---|---|---|---|---|
| house 1 in | `Binduga` -> `03_elves_village` | 48 | 3 (`anezka_bird`, `interior`, `lights_port_01`) | 99 ms |
| house 1 out | back | 3 | 48 | 120 ms |
| house 2 in | `Binduga` -> `01_chorab_village` | 48 | 2 (`interior`, `lights_port_01`) | 104 ms |
| house 2 out | back | 2 | 48 | 120 ms |
| Flotsam gate | `Binduga` -> `landing_site\outside\Flotsam` | 195 | 348 | 849 ms |
| inn in | `Flotsam` -> `inn_interior` | 58 | 19 | 284 ms |
| inn out | back | 19 | 58 | 284 ms |
| (save load) | `''` -> `Binduga` | 0 | 787 | 1 999 ms |

Findings:

* **Interiors are where the house is.** The player moves < 0.2 m between "door task starts"
  and "door task ends" on every door; no teleport. Unknown 1 is answered.
* **A house interior is tiny**: 2-3 layers (`interior`, its lights, one quest prop). The 48
  layers unloaded on the way in are outdoor detail - bushes, rocks, animals, insects, fog,
  light shafts, monster nests, `full_collision`, `terrain_shadows` - and none of them is the
  house's own shell. So for a house the shell stays and the interior sits inside it.
* **Most of a house switch is unloading and reloading the outdoors**, not the interior: going
  in drops 48 layers, coming out reloads the same 48 (120 ms).
* **The inn is different**: it unloads the town's building shells (`exterior` x5, `windows`,
  other buildings' `interior`, `occluders`), the town communities (`l02_town*`) and quest
  layers, and loads its own communities (`inn_basement_*`, `inn_whorehouse_*`), quest layers
  and low-poly stand-ins for the view out of the windows (`ships_lowpoly`, `roche_lowpoly`).
  Keeping everything loaded there would overlap meshes and run two sets of NPCs.
* The Flotsam gate is a full area change (195 out, 348 in), not an interior.

What this means for the options:

* Houses are the good case. Loading a house's 2-3 layers early is cheap and invisible (they
  are inside a closed shell), and the 48 outdoor layers do not have to be unloaded at all on a
  machine with memory to spare. With both, a house door would load and unload nothing; what
  is left is the partition's environment/sound/music change.
* Large interiors (inn) swap NPC communities and building shells by design; preloading their
  layers is possible, keeping the exterior is not.

## `[loading] keep_outdoors` (default 1 from 0.8.0)

`src/enginefix.c`, section "partition trace" (`ko_allowed`, `hook_PartRun`); the partition
hooks are installed when `load_trace` or `keep_outdoors` is on. A mode 4 (door) job that loads
at most 8 layers and whose unload list holds no `exterior`, `windows`, `interior`, `occluders`
or `comm*` layer (small houses, going in) runs with its unload count (job `+0x10`) set to 0,
restored after the run, so the outdoors stays loaded while inside. Leaving a house unloads its
`interior` layers, so that job is left to the game. The loader skips layers that are already
loaded (`FUN_00a91600` checks layer info `+0x38`) and the unloader skips unloaded ones
(`FUN_00a5aa35`), so the way out should load nothing. Log line: `partition: sync job ran N ms -
keep_outdoors: outdoor layers left loaded`.

To check in game: the switch times in and out (load_trace), the house from inside (lighting,
sound, anything visible through the door), the way out, and a save/load inside a house.

### Run 6 (2026-11-02 20:31, owner): keep_outdoors on

`docs/validation/keep-outdoors-run6-2026-11-02.log`. Same route as run 5. No crash.

| Door | Run 5 | Run 6 |
|---|---|---|
| house in (unload 48, load 2-3) | 99-104 ms | 82-96 ms, 48 kept loaded |
| house out | unload 2-3, **load 48**, 120 ms | unload 2-3, **load 0**, 23-27 ms |
| inn in / out (left to the game) | 284 / 284 ms | 272-275 / 291 ms |

* The diff is taken against what is actually loaded: on the way out the load list was empty.
  So the job's load list on the way in (3 layers, ~85 ms) is now the whole cost of a house.
* Owner: works, and the outdoors is visible through the windows from inside. **Problem:** the
  flat "fake room" box that stands behind open doors while the interior is not loaded stayed,
  so Geralt stood inside it. The house's unload list has a layer named `fake`; the next build
  still unloads layers named `fake` (`k_koStillUnload`) and keeps the rest. The trace now also
  prints up to three other text fields of each layer info (`[+off text]`) to find the layer's
  path, i.e. whether `fake` is one layer per house or one per area.

### Run 7 (2026-11-02 20:52, owner): fake rooms unloaded, door_fade 0.05

`docs/validation/keep-outdoors-run7-fake-2026-11-02.log`. Owner: houses look right, no fake
box, the near-instant transition should be the default.

* `fake` is **one layer per area**: `levels\l02-port\fake_interiors\binduga\fake.w2l` (group
  `fake_interiors\flotsam\group.w2lg`) and `fake_interiors\flotsam\fake.w2l`. Unloading it
  for one house removes every fake room of the area. Fine while inside; it matters for
  keeping interiors loaded after leaving (other houses' open doors would show nothing).
* House interiors are one layer each: `village\buildings\<house>\interior\interior.w2l`;
  `05_commoners_village` loads two houses (05 and 06). The outdoor Flotsam partition already
  contains one interior (`marketplace\buildings\merchant_shop\interior.w2l`), which the inn
  partition drops.
* House in 88-174 ms (the 174 ms was the first door of the session), out 44-48 ms (`fake` is
  reloaded). Inn 413 / 292 ms. Flotsam gate 793-1 207 ms in, 490-500 ms out.
* With `door_fade=0.05` the gate showed ~1 s of black with a hard cut: owner found it jarring.
  It is a full area change (different partition folder), so from 0.8.0 such doors use
  `area_fade` (0.25 s), chosen in a hook on the door's start method (slot `00EF02E8` ->
  `FUN_0095dcc4`) before the task constructor reads the fade length.

Not done: keeping interiors loaded after leaving (needs the area's `fake` layer and every
interior handled together) and loading interiors before the first visit (the DLL would have
to start layer loads itself).

### Run 8 (2026-11-02 21:07, owner): 0.8.0 defaults, area fade

Owner: the gate fade works. Remaining: a small hitch just before the door opens on the way in,
and a dark box popping up around Geralt on the way out. From the trace (not kept):

* Way in: the sync job is **84-90 ms on the game thread** loading the house's one
  `interior.w2l` (plus removing `fake`). The game does not draw during it, so that is the
  hitch; the light/sound switch of the partition happens at the same moment.
* Way out: 44-47 ms - unload `interior`, reload the area's `fake` layer (the dark box).
* Gate: `changes area - 0.25 s fades`, job 812 ms.

## `[loading] keep_interiors` (experiment, off by default; built, not yet run in game)

Needs `keep_outdoors=1`. Leaving a house runs an empty job: the `interior` layer is not
unloaded and `fake` is dropped from the load buckets (bucket `+0x08` data / `+0x0C` count,
restored after the run). Visited interiors stay loaded until the next area change (the
Flotsam gate's switch is left to the game and unloads them with everything else), and going
back into a visited house loads nothing. `interior` no longer counts as a shell when this is
on, so entering house B with house A's interior loaded keeps A's too. Expected costs: other
houses' open doors show no fake room until the next area change (what is behind them then is
not known yet), and the quest props in an interior layer (`anezka_bird`) stay attached.

Preloading before the first visit (to remove the ~85 ms on the way in) is still not done: it
needs the DLL to start a layer load itself, outside a partition switch.

### Run 9 (2026-11-02 21:17, owner): keep_interiors on

Owner: much smoother; the dark box was the biggest hitch, also on the way in. What is left is
a small hitch in both directions. Trace:

| Door | Job content | Sync job |
|---|---|---|
| house in, first visit | unload `fake`, load `interior` | 66-96 ms |
| house in, visited | nothing (47-50 kept) | 25 ms |
| house out | nothing (3-4 kept, `fake` not reloaded) | 38-42 ms |

So a job with nothing in it still costs 25-40 ms: `FUN_00a91952` runs every stage whatever
the lists hold - "Flushing...", "Unloading...", **"GC..." (`FUN_00abd770`, a full object
GC)**, loading, "Flushing aync loader...", "Attaching entities...", then `FUN_00aa95fa`
applies the partition (environment, sound). The GC is the stage that does real work on an
empty job.

## `[loading] skip_door_gc` (default 1; built, not yet run in game)

The GC call at `00A91AD7` (`call FUN_00abd770`, stdcall 1 arg; it takes and releases its own
counter at `FUN_00409257()+0x40`, so skipping the whole call stays balanced) is redirected to
`hook_PartGc`, which skips it while `hook_PartRun` runs a job that keep_outdoors handled, and
times it otherwise. The trace line shows `(GC skipped, 0 ms)` or `(GC N ms)`. The skipped
garbage is at most the `fake` layer's objects; area changes and save loads still collect.

### Run 10 (2026-11-02 21:24, owner): skip_door_gc on

GC skipped on every house door. Sync job: first visit 67 ms, then **3-13 ms** in both
directions (was 25-42 ms). Owner: a bit smoother, but still a short black blink - that is
the door fade itself (0.05 s to black, the switch, 0.05 s back), now the largest part of a
house door (door window ~78 ms).

The door also issues render command `00E7C07C(1/0)` (`FUN_00aa1d17` -> `FUN_009d1b60`): it
only shows/hides the `00_loading.swf` spinner, not a blackout.

## `[loading] instant_doors` (default 1, only with keep_interiors; built, not yet run in game)

Fade internals: the fade-out command (vtable `00E7C374`, execute slot 3 `009DC316`, length at
`+0x0C`) and fade-in command (`00E7C388`, slot 3 `009DC2C3`, length at `+0x08`) both end in
`FUN_009d68e7`, which with length <= 0 jumps straight to the target colour, so a zero fade is
still a black frame. Instead the two execute slots are wrapped and skip a command whose length
is the marker `KO_NO_FADE` (0.00123). `hook_DoorStart` sets that length for a door whose
(from, to) pair keep_interiors has already handled (both directions are recorded on the first
visit, in `hook_PartRun`). With no fade-out issued the last fade's progress stays 1.0, so the
task's "fade over" wait (`FUN_00aa72de`) passes at once. Risk to check: if that wait tests
something else, the door would hang with Geralt in the doorway; `instant_doors=0` turns it off.

### Run 11 (2026-11-02 21:31, owner): instant_doors on

Owner: "much better", visited houses outside Flotsam have no blink either way. Two issues:

1. **Flotsam interiors are not handled** (fake boxes and fades as before). Every Flotsam
   interior (`03_clerk_marketplace`, `04_einar_gausel_craftsmen`, `shealas_quarter`, the inn)
   unloads the same ~54 town layers, and that list contains `exterior`/`windows` of *other*
   buildings (Loredo's residence, the piers), `occluders`/`flotsam_out_occ`, the town's sound
   layers (`sounds\l02_town*`) and `comm_roche_ship_*`, so `ko_allowed` refuses it. The
   house's own shell is never in the list (not even the inn's): the rule "no shells" is too
   blunt. Things that do need care: sound layers (each partition swaps its ambience), occlusion
   layers (outdoor occluders around a building the camera is inside), and the inn's low-poly
   stand-ins (`city_building_lowpoly\ships_lowpoly`, `roche_lowpoly`) that would overlap the
   real ships if those stayed loaded. Shared `environments\interior_city\interior.w2l` is
   loaded by every Flotsam interior.
2. **Houses not yet visited show nothing through an open door** with keep_interiors: `fake`
   is one layer per area, so it is all or nothing.

### What the DLL could load itself (static analysis)

* The world's layer tree: root group `world+0x50`; a group has its layer infos at `+0x90`
  (count `+0x94`) and child groups at `+0x80` (count `+0x84`) (`FUN_00a90f59` walks it to
  list the loaded ones: layer info `+0x38` loaded layer, `+0x5C` pending load). Layer info
  path at `+0x68`. So every interior layer of the act (`...\buildings\<name>\interior\*.w2l`)
  can be found without visiting it.
* The job's load stage calls, per layer info in a bucket, `FUN_00a5ac6c({0, 1})` (in
  `FUN_00a91600`, after a progress command) - the per-layer load; the job then flushes the
  async loader and attaches entities (`FUN_00a7015c(world)`). Calling convention of
  `FUN_00a5ac6c` not checked yet.
* `FUN_00a91e46` just runs the world's pending job through the same `FUN_00a91952`: stream
  mode is not incremental in this path.

Proposed next step: load every building interior of the act (and drop `fake`) once, when the
area loads behind the loading screen or the area-change fade, so open doors show the real
rooms, no house ever loads on its door and every house door can go without a fade; plus a
same-folder rule for Flotsam that still swaps sounds, occluders and low-poly stand-ins.
First a passive count (how many interior layers, how large) from the layer tree.

## Flotsam rule and `[loading] preload_interiors` (built, not yet run in game)

* `ko_allowed` now takes a door within one partition folder (`ko_same_folder` on the switch's
  from/to names; an area change is between folders) and `ko_class` sorts each layer by path
  and name: FAKE (`fake`), SWAP (`\sounds\`, `occ` in the name, `\environments\interior`),
  LOWPOLY (`_lowpoly\`), INTERIOR (`\buildings\<name>\interior\`), OTHER. A kept door unloads
  FAKE, SWAP and LOWPOLY only and does not load LOWPOLY (the real ships stay). The way out of
  a house is recognised by its load list holding `fake`; without keep_interiors or preload it
  is left to the game as before.
* The job's unload list and load buckets are no longer edited in place: they are pointed at
  DLL arrays (`g_koUnloadOut`, `g_koLoadOut`) for the run and put back after it.
* Instant-door pairs are forgotten on an area change (mode 1, or a door between folders)
  unless preload is on: 0.8.0 kept them, so after the Flotsam gate and back a visited house
  would have gone without a fade although its rooms were unloaded.
* preload_interiors: on an area change the DLL walks the world's layer tree (root
  `*(world+0x50)`, infos `+0x90/+0x94`, children `+0x80/+0x84`), appends every INTERIOR info
  that is not loaded (`+0x38`) or loading (`+0x5C`) to the job's last bucket, drops FAKE from
  the load lists, and keeps INTERIOR layers out of every unload list. `load_trace` logs
  `interiors: before/after the switch - N layers in the world, ...; M building interiors, K
  loaded` with a memory line, and each added layer (`preload <name> [path]`).
* Risks to check in game: memory and the area load time; interior lights now loaded
  everywhere (GPU cost?); interiors that exist in several quest states under
  `buildings\...\interior\` would all be loaded at once (look for doubled furniture or
  people); the inn's own sound still swaps.

### Run 12 (2026-11-02 21:47, owner): preload on, Binduga only (stopped early)

`docs/validation/preload-run12-2026-11-02.log`. The world (`l02-port`) has **1 690 layer
infos and 64 building interior layers** (interior + lights per building, Binduga and Flotsam
together). Loading a save: 850 layers instead of 787, job 2 837 ms instead of ~2 000 ms
(+0.8 s behind the loading screen); process private memory 1 032 -> 1 260 MB across the
whole load (no baseline without preload in this run). Every house door afterwards: nothing
unloaded, 0-1 layers loaded (the elves house loads one more small layer).

Owner: the first door of each house still faded. Cause: the no-fade decision is made when
the door starts, before the job is known, and only for pairs already seen to load nothing.
Fix: with preload every door within one folder goes without a fade unless that pair's job
(the game's run alone, without the trace's logging) once took >= `KO_SLOW_MS` (50 ms); then
it keeps `door_fade`. The interior count/memory lines now run on area changes only (they
added ~15-20 ms to every traced door).

### Run 13 (2026-11-02 21:53, owner): preload, no fade on any house door

`docs/validation/preload-run13-2026-11-02.log`. Owner: works very well; light and sound
change correctly in and out. Binduga, Flotsam (clerk, Einar Gausel, the inn, Sheala's
quarter), then a prologue save (`all\Village`, `l01-keep`).

* Save load (Binduga): 850 layers in 2 509 ms; private memory 1 048 -> 1 281 MB over the
  load. Flotsam gate: 193 out, 344 in, 771 ms; 1 539 -> 1 601 MB. Largest free block stayed
  above 1.6 GB all session.
* Every house door in Binduga and Flotsam: no fade, sync job 7-49 ms (game run alone), 0-12
  layers loaded (Flotsam swaps sound/occluder/environment layers). The inn: 47 ms in, 23 ms
  out, 5 layers swapped each way.
* The prologue world has 0 layers matching `\buildings\...\interior\`: its interiors live
  under `island\village_b_ints\interior<N>\interior<N>.w2l` and
  `containers\village\containers\interior<N>\...` and are not preloaded. Its doors kept
  everything anyway (no `fake` there); `interior20` took 57 ms once and kept its fade after.
* **Ground vanished behind Geralt walking into an interior at La Valette** (seen there, not
  in Binduga). Nothing was unloaded on that door. Cause: the partition's location type
  (`partition+0x28`) goes to the renderer (`FUN_00aa95fa` -> render command `00E7C3B0`, execute
  `009DC0BB`: `renderer+0xA8 = value`), and the frame setup `FUN_009c011e` turns three draw
  flags off (frame `+0x2775`, `+0x2776`, `+0x278F`; terrain, grass and one more) when
  `renderer+0xAC` is set and the type is `LT_INDOOR` (1). The game hid that behind the fade.

## `[loading] indoor_outdoors` (default 1 with keep_outdoors; built, not yet run in game)

The `cmp dword [eax+0xA8], 1` at `009C022C` (the only such compare in `.text`) gets the
immediate `0x7F`, so an indoor partition keeps drawing terrain and grass. Light, sound and
music still switch (they come from the partition's other fields). To check: grass or
terrain showing through floors or in cellars, and the frame rate indoors.

`ko_class` now also counts `_ints\` paths (the prologue village's
`island\village_b_ints\interior<N>\`) as INTERIOR, so preload covers them too.

### Run 14 (2026-11-02 22:08, owner): prologue, ground still vanishes

The `indoor_outdoors` patch never applied: the fingerprint started one byte late
(`80 B8 AC ...` is at `009C0223`, not `009C0224`), so the check never matched and the
retry ran out silently within the session. Fixed; the immediate stays at `009C0232`.
The prologue's 24 `village_b_ints` interiors were preloaded (folder rule), all doors kept
everything and went without a fade.

## Interiors from the partition tree (built, not yet run in game)

Folder names differ per level, so preload now reads the game's own partition tree
(`kp_*` in `src/enginefix.c`). Static analysis: root `*(*(world+0x358)+0x48)`; a node has its
name at `+0x04` (String; `FUN_00a82c99` walks children by name), location type at `+0x28`
(0 = inherit from the parent, `FUN_00aa95fa`), parent `+0x60`, children `+0x64/+0x68`, own
layer groups `+0x74/+0x78` (String, stride `0x10`; `FUN_00a83097` adds the parents' groups
and resolves each path with `FUN_00b059d9`). An area's interiors are its indoor
(`LT_INDOOR` = 1) siblings; their own group paths are matched as whole path segments
against each layer info's path. Excluded: SWAP/LOWPOLY/FAKE and townspeople (`comm*`,
`\community`), which stay with the door. Preloaded infos go into `g_kpSet` (sorted, reset on
a save load) and no job unloads them. If the tree yields nothing, the folder rule is used.
`load_trace` logs `preload: '<area>' found, N indoor siblings, M layer groups` and each
added layer.

## One user setting

The shipped ini has `[loading] interiors=2` (2 keep outdoors + preload, 1 keep outdoors
and load interiors on first visit, 0 game). It only sets the defaults of `keep_outdoors`,
`keep_interiors` and `preload_interiors`; these and `skip_door_gc`, `instant_doors`,
`indoor_outdoors` (all default 1) still work as overrides for testing.

### Run 15 (2026-11-02 22:21, owner): release candidate

`docs/validation/release-run15-2026-11-02.log`. Owner: works very well. `indoor_outdoors`
applied (`doors: indoor partitions keep drawing terrain and grass`); the ground stays. The
partition tree found 27 indoor siblings / 36 layer groups for the prologue village, but **0
for Binduga and Flotsam** (Act 1's houses are not `LT_INDOOR`), where the folder rule
supplied the 64 interiors. Every house door in all three places went without a fade. For
0.8.0 both sources are used together (tree first, then folder names, no duplicates).

## 0.8.1

More logging so a player's log shows missed interiors without `load_trace`:

* On every area change: `preload: '<area>' found (type N); siblings by location type:
  default .., indoor .., outdoor .., cave ..; M layer groups from the indoor ones`, and
  `preload: X interior layers from the partition tree, Y more by folder name`. With
  `load_trace` each sibling's name and location type is listed too.
* On a kept door with preload on: `doors: '<from>' -> '<to>' loaded N layer(s) not preloaded
  (T ms): <paths>`, for every loaded layer that is not sound/occluder/environment, low-poly,
  fake rooms or townspeople - i.e. an interior the preload did not cover.
* **Occluders wait for Geralt to clear the door** (runs 16-17). Owner, 0.8.0:
  walking out of the Flotsam inn's harbour door, everything in front turned see-through
  (only water showed) until Geralt was outside. The way out of every Flotsam building loads
  the town's occluders (`marketplace\occluders\occluders.w2l`,
  `occluders\flotsam_outside\flotsam_out_occ.w2l`, run 13 trace); with no fade the camera is
  still inside the building's occluder block when they arrive, so the renderer culls
  everything in front of it except water and sky. The game hid this behind its fade (and its
  ~1.2 s wait for the door animation's `fade_out` event). Now a kept door leaves its occluder
  layers out of the job; `ko_frame` (called from the viewport tick on the game thread) waits
  until the player is 5 m from the door (at least 0.4 s, at most 30 s), then queues a switch to
  the partition that is already current through the game's own request function
  `FUN_00aa7e18` (EAX = game, stdcall `(String* name, mode 4, String* screen)`, `RET 0xC`;
  name = `world+0x310`, screen = the empty String `0160FBA0`). That job's diff is exactly the
  missing occluders. A new switch in between cancels the wait. Without the viewport tick
  (window features off) nothing is deferred. Log: `doors: Geralt is through the door -
  loading the occluders (N ms after the door)`.

  Run 16 (`docs/validation/occluders-run16-loop-2026-11-02.log`): the view stayed right, but
  the reload switch (from and to the same partition) deferred the occluders again, so it
  repeated every 0.4 s and they never loaded. Fixed: a switch with from == to is never
  deferred. Run 17 (`docs/validation/occluders-run17-2026-11-02.log`): out through both inn
  doors and back, one reload per door 3.3-5.0 s after it, 2 layers in 12-13 ms; owner: much
  better.

## Whole indoor areas are not houses (2026-11-06)

**Symptom** (a player on Nexus, reproduced by the owner on Steam with 0.11.0 and the current
build): in the prologue's interrogation cutscene the wall behind Roche is a flat, fog-coloured
surface and his face is missing inside the hood, while his body, the table and the stone pillar
are drawn. Draw distance, LOD and shadows at Vanilla before the load did not change it;
`interiors=0` did.

**Cause.** The partition-tree rule takes every `LT_INDOOR` sibling of the target partition as a
house. The prologue's partitions under `all` were read from `levels\l01-keep\l01-keep.w2w`
(`pack0.dzip`, offline):

| Partition | Location type | Own layer groups | What it is |
|---|---|---|---|
| `Dream` (the interrogation; the room itself is in `all`) | default | 14 | `basement\torture_room`, `occluders\interogation_room`, `environmets\dungeon` ... |
| `Basement` | `LT_INDOOR` | 31 | the whole dungeon: `basement`, `occluders\basement`, `quests\q002_prison_break` ... |
| `OldTunnel` | `LT_INDOOR` | 12 | `old_tunnel`, `occluders\old_tunnel`, `environmets\old_tunnel` ... |
| `interior1` ... `interior25`, `with_basement`, `with_basement2` (village houses) | `LT_INDOOR` | 1-2 | `island\village_b_ints\interior<N>`, `containers\village\containers\interior<N>` |

A save in `all\Dream` or `all\Bridge` therefore preloaded `Basement` and `OldTunnel` (log:
`43 layer groups from the indoor ones`, `205 interior layers from the partition tree`). Group
paths match as path prefixes, so that pulled in `basement\xbox_only\rocks.w2l`, every
`quests\q002_prison_break\...` layer and the dungeon's occluders. Those occluders,
`occluders\basement\basement.w2l` and `occluders\old_tunnel\old_tunnel.w2l`, have no `occ` in
their names, so `ko_class` did not hold them back. The most likely culprit for the missing wall
and face is an occluder from the basement next to the interrogation room: the renderer skips
whatever is behind it, per component. Not proven which of these layers does it; removing all of
them is the fix.

The other worlds: `l02-port` has no `LT_INDOOR` partition (only `LT_CAVE`), `03_camp` none, and
`04_city` one, `area_sewers` with 62 groups: the same mistake waiting in Act 3.

**Fix** (`src/enginefix.c`, `kp_interior_groups` and `ko_class`):

* An indoor sibling with more than 4 own layer groups (`KP_HOUSE_GROUPS`) is a whole area and is
  skipped. Log: `preload: indoor sibling 'Basement' skipped - 31 layer groups of its own, a whole
  area, not a house`. The prologue houses still preload, through the tree and the folder rule
  (`_ints\`).
* Any layer under an `\occluders\` folder is an occluder (`LC_SWAP`, and `ko_is_occluder` for the
  door deferral), whatever its file is called.

To check in game: a save in `all\Dream` logs both siblings as skipped, about 0 layers from the
partition tree, and the interrogation shows the wall and Roche's face with `interiors=2`.
Village houses still open without a fade.

## Vergen: buildings by their lighting, fake rooms by content (2026-11-11)

**Symptom** (a player on Nexus, 0.12.0, and the owner on Steam the same week): in Vergen the
dark "fake room" box that stands behind a door from outside is still there inside the house,
around Geralt. The owner's log, a save loaded in `common\dwarf_city\city_dec_unload`:

```
preload: 'common\dwarf_city\city_dec_unload' found (type 0); siblings by location type: default 24, indoor 0, ...
preload: 0 interior layers from the partition tree, 0 more by folder name
doors: '...city_dec_unload' -> '...building04_deco' loaded 3 layer(s) not preloaded (172 ms): levels\03_camp\dwarf_city\building04_dec\building04_dec.w2l ...lightsources.w2l levels\03_camp\environments\environment_interior\building04_dec\building04_dec.w2l
partition: keep_outdoors kept 5 of 6 layers, unloaded 1
```

**Cause.** Act 2's level (`levels\03_camp\world.w2w`, read offline from `pack0.dzip`) names
nothing the way Act 1 does, so each name rule missed:

| What | Act 1 (`l02-port`) | Vergen (`03_camp`) | Rule before | Result in Vergen |
|---|---|---|---|---|
| Fake rooms | `fake_interiors\<area>\fake.w2l` | `dwarf_city\windows_outside\windows_outside.w2l` (25 x `fake_inside_dwarf.w2mesh` + `dark_interrior.w2mesh`, 29 wall pieces) | layer called `fake` | kept loaded inside: the box |
| A house's rooms | `...\buildings\<house>\interior\*.w2l` | `dwarf_city\building04_dec\*.w2l` | `\buildings\...\interior\`, or an `LT_INDOOR` sibling | no partition is `LT_INDOOR`, no folder matches: nothing preloaded |
| A house's lighting | `environments\interior_city\interior.w2l` | `environments\environment_interior\<house>\<house>.w2l` | `\environments\interior` | kept loaded after leaving the house |

The street partition is `city_dec_unload` (5 groups of its own); its 24 siblings under
`common\dwarf_city` are the houses (2-7 groups each); the town itself is their parent's 77
groups. A door from the street unloads the street's 6 own layers (`windows_outside`,
`fabric_roof`, `occluders_outside`, `background`, two `finisher_spots`) and loads the house's
2-8.

### The survey behind the new rules

Every `.w2l` under `levels\` in `pack0.dzip` (5 271 layers, five worlds) was read for the meshes
it names, and every `CStreamingDoor` for its `inside`/`outside` partition: 153 doors, 106
partition pairs. With each world's partition tree and layer list, a model of the door rules
gives, for every pair, what the game swaps and what the mod keeps. It reproduces the logged
jobs (Binduga 48 out / 2-3 in, Flotsam to the inn 58 / 19, the Flotsam gate 195 / 348, Vergen
6 / 2-4). Findings:

* `fake_inside*.w2mesh` is in three layers that a partition loads: the two Act 1 `fake` layers
  and Vergen's `windows_outside`. No other world has fake rooms. (`dark_interrior*.w2mesh` alone
  is also the black plane in cave mouths, in `black_plane`/`dark_plane` layers; those belong to
  area changes.)
* Every Vergen house partition lists an interior environment group
  (`environments\environment_interior\<house>`), as 13 of Flotsam's 14 do
  (`environments\interior_city`). No street and no outdoor area lists one. The only other
  partitions that do are the Arena's `room` and `arena_cutscene` (17 and 22 groups).
* Each of the 25 Vergen fake rooms, the 15 in Flotsam and the 6 in Binduga has entities of a
  preloaded interior within 4 m of its centre (all but one within 2.6 m, and at least 11
  within 6 m), so leaving the fake rooms out shows a real room behind every one.

### The rules (`src/enginefix.c`)

* **`ko_class`**: FAKE is a layer called `fake` or `windows_outside`. SWAP also takes an
  interior's environment wherever `interior` follows an `environments` folder
  (`ko_env_interior`), and the rest of a `\windows_outside\` folder (Vergen's awning layer),
  which the game takes away inside every house. In the five worlds this changes the class of
  29 layers of `03_camp` and one of the Arena, none in the prologue, Act 1 or Act 3.
* **Buildings by their lighting** (`kx_is_house`, `kx_family`, `kx_groups`): a partition with an
  interior environment group and at most 8 groups of its own (`KX_HOUSE_GROUPS`; Philippa's
  house has 7, the Arena's rooms 17 and 22) is a building, anywhere in the partition tree. Its
  rooms are the layers directly in its own groups that no parent and no sibling that is not a
  building lists as well: what its door loads. That is needed for Flotsam, whose houses also
  list parts of the street (`pier\buildings\01_crane_pier`). Group folders are matched the way
  the game takes a partition's layers (`grp_holds`, not as path prefixes). Preloaded on every
  area load, like the folder rule: 59 layers for Vergen's 24 houses, and in Act 1 one layer the
  folder rule did not have (`animals\floatsam_03commoners_house`).
* **The street of a save inside a house** (`ks_street`): Vergen's street is smaller than some of
  its houses, so "the sibling with the most groups" found none. The street of a building is now
  its one sibling that is not a building, when there is exactly one.
* **Fake rooms follow the preload.** Up to 0.12.0 the area's fake rooms were left out whenever
  `preload_interiors` was on, also when the memory guard had skipped every interior (0 of 111
  with the system's Direct3D 9 at 4K, `door-transitions.md`): open doors then showed nothing.
  Now an area load holds its fake-room layers back to the very end of the job, after the
  preloaded layers; `kp_layer_ok` loads them only if a preloaded layer was skipped. While they
  are in use (`g_kpFakeKeep`: a skipped layer, the coarse skip, a memory door, or a world where
  the mod knows no interior) doors swap them the way the game does and keep `door_fade`.
* Townspeople are left to the door by `\communit` in the path (`communities\...` as well as
  `community...`).

Log lines: `preload: 24 building(s) with interior lighting of their own in the partition tree,
57 layer groups only they list`, `preload: N interior layers from the indoor partitions, M from
the buildings with their own lighting, K more by folder name (T known in this world)`,
`preload: the fake rooms behind doors and windows are left out - every interior is loaded` (or
`stay in use - not every interior is loaded (doors keep their fade)`).

### Checked offline

The DLL's own functions (`ko_class`, `kx_groups`, `kp_walk`, `ks_street`) were run in a harness
on each world's partition tree and layer list, rebuilt in memory at the offsets the DLL reads,
and compared with the model: the same class for all 5 271 layers, the same buildings (24 in
`03_camp` with 57 groups, 9 in `l02-port` with 10, none elsewhere), the same 59 and 19
preloaded layers, and `city_dec_unload` as the street of all 24 Vergen houses. With these rules
a Vergen door loads only its house's environment (1-2 layers) and unloads the awning and the
outdoor occluders. The harness and the survey scripts were throwaway and are not in the
repository; the level-data format notes are in `door-transitions.md`, "Reading the level data".

### Doors between whole areas: `[loading] area_memory_mb` (default 1536)

`ko_same_folder` takes any door between two partitions of one folder for a house door. The
survey shows such doors between whole areas: in Act 2 quarry / harpy lair / secret passage,
old tower / its cave, assassins' cave / ravine (16-64 layers a side); in Act 3 every pair under
`outdoor\` (the centre has 166-218 layers of its own) and the rooms under the Vran tower; in the
prologue `Basement` / `AfterBasement`. On those the area left behind stays loaded apart from
sound, occluders and its interior environment, so the way back loads nothing, and in Loc Muinne
every quarter visited stays. Up to 0.12.0 only the memory door (`door_memory_mb`, 1 024 MB)
ended that, together with every interior.

How much room there is, from the owner's Act 2 session of 2026-11-10 (Steam, DXVK), in which
the mod kept nothing extra (no interior was found in Act 2 yet and no area door was used): free
address space 2 191 MB after the first load, 1 469 MB after 52 minutes, 1 048-1 143 MB after
2 h 10 min, and 870-1 210 MB for the two hours after that (`with only 884-987 MB ... the game's
door` on most doors). A save loaded late in the session started from 1 122 MB free: the game
itself does not give the room back. What one kept area costs has not been measured.

So keeping whole areas is tied to the room left (owner's request, 2026-11-11):

* **A house door** has a side that is a building or a small place: a partition that lists an
  interior environment (any size: Flotsam's inn has 11 groups) or has at most 8 groups of its
  own (`kx_small`, `ko_house_door`). In the five worlds that is 70 same-folder door pairs: all
  of Act 1's, the prologue village's and `TowerCemetery`'s, Vergen's. They keep their street
  down to `door_memory_mb`, as before: the street is needed again on the way out.
* **An area door** is every other same-folder door: 21 pairs, 11-82 groups a side (the list
  above). With less than `area_memory_mb` free when it starts, it runs as `g_koMemLow = 2`: the
  game's job unloads the area left behind and whatever older areas were still kept, its GC
  runs, the fade is `area_fade`, and its occluders wait as on a memory door. The preloaded
  interiors stay loaded and the fake rooms stay out. With more free, it keeps as before.
  Log: `doors: '...' -> '...' joins two areas with N MB of address space free (under 1536) -
  the area left behind unloads, interiors stay, 0.25 s fades`, then `partition: area door kept
  K of N layers, ...`.
* 1 536 is above `preload_memory_mb` (1 280) on purpose: kept areas should not use the room the
  interiors need. Not tuned in game. 0 = never (0.12.0's behaviour).

`kx_small` was run in the harness for every partition of the five worlds and agrees with the
model. Not seen in game: no area door has been used with this build, and Act 3 not at all.

### Seen in game (owner, 2026-11-11, Steam, DXVK, 3840x2160, a fresh session)

`docs/validation/vergen-interiors-2026-11-11.log`. Roche's path, where only part of Vergen is
open: houses 04, 06, 07 and 17 in and out, some of them several times, then a save made inside
house 04, loaded, and out. Owner: everything works, no problem leaving the house of the save.

| | 0.12.0, same save, same morning | This build |
|---|---|---|
| Area load | 801 layers, 0 preloaded, 4 432 ms | 859 layers, `59 of 59 preloaded layers loaded`, fake rooms left out, 3 528 ms |
| Private memory after the load | 1 293 MB (2 184 MB free) | 1 328 MB (2 130 MB free): +35 MB for the 59 layers |
| Door in | `loaded 3 layer(s) not preloaded (172 ms)`, `keeps its fade`, the fake rooms kept inside | `no fade (0 to load)`, job 11-22 ms, unloads 2 (awning, outdoor occluders), loads 1-2 (the house's environment) |
| Door out | - | `no fade`, job 10-28 ms, unloads 1-2, loads the awning, fake rooms skipped; the occluders 3.4-3.9 s later in 10-13 ms |
| Save inside house 04 | - | `'...building04_deco' is inside 'city_dec_unload' - its 8 layers load now too`, `60 of 60`, then out without a fade in 28 ms |

No `loaded N layer(s) not preloaded` and no `keeps its fade` line in the session. The lighting
lines show the house's environment blending in and out at each door (`'building04_dec' 100`).
A kept door's unload list is now 59-62 layers (the other houses' rooms, which the target does
not list), of which it keeps all but the swap layers.

**A crash while quitting, not from this change.** That session ended with the pause menu's Quit
and, one second after the exit flag, `crash: access violation executing 0x4906E000` on the main
thread, with the world already gone. It is the UI library's shutdown destroying the heap of a
loading-screen movie that was still alive; the door code does not run once the world is gone
and holds no UI object. `quit-crash.md`, "A loading-screen movie still alive at the GUI
shutdown", has the dump analysis and what is not proven.

### Act 1 with the same build (owner, 2026-11-11, same setup)

`docs/validation/flotsam-regression-2026-11-11.log`, one session: Vergen from the main menu
(house 17 in and out), then a Flotsam save loaded from the in-game Load menu, starting inside
the inn: out, the clerk's house and the barracks' house in and out, the inn in and out and in
again, a second save inside the inn loaded, out, then the pause menu's Quit. Owner: looked right.

* Both Flotsam loads: `9 building(s) with interior lighting of their own ..., 10 layer groups
  only they list`, `0 interior layers from the indoor partitions, 19 from the buildings with
  their own lighting, 46 more by folder name (83 known in this world)`, `111 of 112 preloaded
  layers loaded, 0 left for their first visit`, `64 building interiors, 64 loaded`. 0.12.0
  logged `110 of 111` for the same kind of load (`door-transitions.md`): the one layer more is
  the one the offline check predicted (`animals\floatsam_03commoners_house`). One layer of the list
  does not load, as before.
* No fake-room line on these loads, as expected: the load starts inside the inn, so the job
  holds no fake-room layer and there is nothing to report (`fakeN` 0). The first door out
  skips it (`skipped 1 (fake rooms, low-poly)`), as does every later door to the street.
* All 9 Flotsam doors and both Vergen doors: `is loaded - no fade (0 to load)`, jobs of
  13-35 ms, picture back after 31-63 ms. No `loaded N layer(s) not preloaded`, no `keeps its
  fade`, no memory door.
* Memory: 1 348 MB private and 2 147 MB free after the first Flotsam load, 1 456 MB and
  2 071 MB after the second, the third area load of the session.
* The quit was clean: `quit: quitting from the main menu, 2609 ms after Quit, no loading screen
  up`, `exit: ExitProcess(0)`, `unloaded`, no dump.

GOG 3.5.0.26g, the same DLL and day (`docs/validation/gog-0.12.1-2026-11-11.log`, DXVK): a
Flotsam save from the main menu, the same 9 buildings and 10 groups, `63 of 63 preloaded layers
loaded` (0.11.0 on GOG: `62 of 62`), the fake rooms left out, and the craftsmen's bar in and out
twice without a fade (jobs of 13-18 ms). Vergen was not visited on GOG.

## Known cost: the kept outdoors deep inside a large interior (2026-11-11)

`keep_outdoors=1` (implied by `interiors=1` and `2`) suppresses the game's unloading of the
outdoor layers at a door, so they stay in the world while you are inside. In a large area
that is not free: the visibility pass walks their meshes every frame even when the interior
hides them. Measured in the cave near Vergen, 4K borderless, `VSync=0`, DXVK 72 fps cap,
`[distance]` at the vanilla preset:

| `interiors` | state at the same spot | fps | GPU busy | range-hook calls / 5 s |
|---|---|---|---|---|
| 2 (kept outdoors) | walked there | 49.4 | 11.0 ms | ~5.1 M |
| 2 | after save + load (area rebuilt: 59 preloaded interiors, no kept outdoors) | 74.6 | 6.6 ms | ~0.65 M |
| 0 | walked there | 75.0 | 6.8 ms | ~0.52 M |

The preloaded interiors are not the cost - the save + load had the same 59 and was fast; the
kept outdoor layers are. The log at the switch before the walk showed `partition:
keep_outdoors kept 112 of 114 layers, unloaded 2; loaded 16`. Workaround: `interiors=0`
(the game's own unloading, at the price of a reload at every house door), or `release_kept`
below.

### `[loading] release_kept` (default 30 after 0.12.1, doors between whole areas only; seen in game on Steam and GOG)

**Default and scope (2026-11-12; seen in game on Steam).** Up to here it was
off by default and recorded what any kept door kept. It is now 30 m by default and only a door
between two whole areas records (`areaDoor` in `hook_PartRun`: `ko_house_door` says neither side
is a building or a small place). A house door keeps its street in the world as before: the
street is what the house's windows show, it is 2-50 layers, and Flotsam's inn is larger than
30 m. What an area door leaves behind is hidden, so beyond 30 m the picture is the game's own
(it unloads that area at the door) plus the light layers; low-poly stand-ins exist only at Act
1's house doors, so nothing the game would show is missing.

Seen in game (owner, 2026-11-12, Steam, `docs/validation/release-area-doors-2026-11-12.log`): the
cave as before (`52 kept outdoor layers recorded`, `hid 39 ...`, `13 light layers ...`, shown
again 14 m from the door, out without a fade), then an Act 1 save: the Flotsam inn in and out
twice, the clerk's house, the gate to Binduga and a house there. Every house door went without
a fade and none of them logged `kept outdoor layers recorded`; no `not preloaded`, no `keeps its
fade`; Flotsam preloaded `109 of 110` layers (two fewer than before: the quest-named ones);
clean quit. Owner: everything worked very well. Not seen in game: Act 3, where the areas are
open-air quarters and the one left behind goes out of the picture 30 m past the door.


`release_kept=<metres>` unloads the kept outdoor layers once Geralt is that far from the door
that kept them. The way out loads them again at the door, behind its fade: one ordinary door
load, in exchange for the interior's frame rate while deep inside.

* **What is recorded.** After a kept door (`hook_PartRun`, `keep_outdoors` path, a real door,
  not the occluders' switch to the same partition) the layers it kept, except interiors
  (`LC_INTERIOR`) and preloaded layers (`kp_in_set`), which `preload_interiors` keeps on
  purpose. That leaves the outdoors: in Vergen a house door keeps only the other houses' rooms
  (all preloaded), so a house door records nothing; the cave records its ~112 outdoor layers.
  Geralt's position at the door is the anchor; a later kept door adds its layers and moves the
  anchor. At most 1 024 layers; more and nothing is released (no partial release).
* **When.** `kr_frame` (viewport tick, game thread, every 250 ms) once Geralt is
  `release_kept` metres from the anchor, at least 3 s after the door, with no switch in
  progress or queued (`game+0xE8`), no stream job (`world+0x30C`), no occluders waiting and no
  loading screen (render thread `+0x268`/`+0x26C`). A blocked release logs why, once.
* **How.** Not by calling the unload primitives from outside a job: `kr_frame` queues a switch
  to the partition that is already current (`FUN_00aa7e18`, mode 4, as the occluders' switch
  does), and `hook_PartRun` adds the recorded layers to that job's unload list. The game's own
  runner (`FUN_00a91952`) then unloads them as it does at a door: `FUN_00a7015c(world)`,
  `FUN_00a5aa35` per layer, `FUN_00a7015c(world)`, `FUN_00b05b13` on the root group (it
  destroys the unloaded layer objects; the world tick `FUN_00a91724` runs it every frame too),
  the GC (not skipped for a release), entity attach and the partition settings of the partition
  Geralt is already in. No new engine address, so GOG takes the same path (the hide and the
  show were seen there on 2026-11-12; the unload was not).
* **Safety.** The switch lists for unloading the layers that are loaded but not in the target
  partition (the release job in the run below listed exactly the 112 kept layers), so only
  recorded layers that are also in the release job's own unload list go: a layer of the
  partition Geralt is in is never released. Every job also strikes the layers it loads off the
  list. A stream
  switch (mode 0, never seen by `hook_PartRun`), an area load (mode 1) or another world forgets
  the list. The release forgets the learned instant-door pairs, so the door that loads the
  layers back has its fade again; the way out does not report them as interiors the preload
  missed.
* **Log.** `interiors: N kept outdoor layers recorded (M in all) - released once Geralt is D m
  from (x, y, z)`, then `interiors: released N kept outdoor layers D m from the door (job T ms,
  GC G ms); they load again at the door`, and `partition: release kept 0 of 0 layers, unloaded
  N`. Off: `release_kept=0`.

#### Seen in game (owner, 2026-11-11, Steam, DXVK, 4K, `release_kept=30`)

`docs/validation/release-kept-2026-11-11.log`. A save in front of the cave (Act 2, quarry),
in through the door to `common\secret_passage`, on to the open spot (the cliff), out again:

| Step | Log |
|---|---|
| Door in | `keep_outdoors kept 112 of 114 layers`, `53 kept outdoor layers recorded` (the other 59 are preloaded interiors and stay) |
| 12 s later, 30 m in | `released 53 kept outdoor layers 30 m from the door (job 72 ms, GC 33 ms)` |
| At the spot | `distance stats` ~0.72 M range calls per 5 s (kept state: ~5.1 M; after a fresh load: ~0.65 M); owner: frame rate stable, clearly better than before |
| Door out | the 53 layers loaded again in 372 ms behind the 0.25 s area fade (`keeps its fade`), picture back after 438 ms; the world normal; no crash, no dump |

The black plane in the cave mouth seen from outside is the game's own
(`quarry\black_plane_harpy.w2l`, part of the quarry partition, loaded by the game's job on the
way out); the cave has one of its own (`secret_passage\black_plane.w2l`). Not measured with
PresentMon yet. The intersection with the release job's unload list (Safety, above) came after
this run; the run released all 53, which the job listed.

Retest with that check (`docs/validation/release-kept-retest-2026-11-11.log`, same route): the
door in recorded 53, `released 53 kept outdoor layers 31 m from the door (job 74 ms, GC 36 ms)`,
the way out loaded them back in 365 ms behind the fade; owner: everything looked normal, no
dump. An earlier short trip in the same session showed why the check is needed: in for 10 s
and out again, the door out loaded nothing (the quarry was still loaded), so its load list
struck nothing off and the recorded list still held the 53 quarry layers while Geralt stood in
the quarry (`69 in all`); only the release job's unload list tells them apart.

#### Research: hiding the kept layers instead of unloading them (Steam, static only)

Unloading frees memory but costs a load at the door on the way out. The engine can also keep a
layer loaded and only take its objects out of the world; quests do that during play. Steam
3.5.0.26, unpacked copy:

| What | Where | Contract |
|---|---|---|
| Sync a loaded layer's attachment | `FUN_00a5a90e` | EAX = layer info; info `+0x38` (the layer) must be set. If the predicate says visible and the layer is not attached (layer `+0x4C` world is 0): `FUN_00a8b3d2` (ESI = layer), then `FUN_00a8bb48` (EDI = layer, stack: world `info+0x34`). If not visible and attached: `FUN_00a8ba97` (EDI = layer). Guarded by the bytes at `01608658` (must be 0) and `010CB8FF` (must be set) |
| The predicate | `FUN_00a5a568`, only call `00A5A92A` (`E8` rel32) in `FUN_00a5a90e` | EAX = layer info, result in AL. 0 when info `+0x58` is 0; in game (`[01404950]+0x4C` set) also 0 when the layer group of info `+0x4` (`FUN_00423cc3`) has `+0xA0` = 0; else 1 |
| Detach | `FUN_00a8ba97` | per entity: saves its state into the layer storage (`FUN_00afe46f`) and calls entity vtable `+0x198` (detached, world); then `FUN_00a46ca7(world)` and removes the layer from the world's list (`world+0x54/+0x58`); layer `+0x4C` = 0 |
| Attach | `FUN_00a8bb48` | layer `+0x4C` = world, adds it to the world's list, `FUN_00a46f45(world)`, per entity: restores its state (`FUN_00afe506`), vtable `+0x194` (attached, world) |
| Group visibility | `FUN_00b05aa2` (EAX = group, CL = visible) | sets group `+0xA0` and syncs every loaded layer under it (`FUN_00b057ab`, then `FUN_00a5a90e`) |
| Its callers | `HideLayerGroup()` `FUN_00a92654`, `ShowLayerGroup()` `FUN_00a92839` (script natives, their warning strings), quest blocks `FUN_00965dbe`/`FUN_00965ea2` (`CQuestPhaseBlock_OnDeactivate`/`OnActivate`), `FUN_00ae297c` | the game's own runtime hide/show |

`FUN_00a5a90e` is also run for every loaded layer by each job's "Attaching entities..." stage
(`FUN_00b05a52`) and by the world tick when a layer finishes loading (`FUN_00a5a95e`), so a hide
decided in the predicate holds through door jobs.

The way in this suggests: redirect the predicate's one call (`00A5A92A`) to a stub that returns 0
for a DLL-owned set of hidden layer infos and calls the original otherwise, then run
`FUN_00a5a90e` on those layers. No game flag is written, so the quests' own `+0xA0`/`+0x58` state
stays theirs; showing again is removing them from the set and syncing. Not proven: that a
detached layer leaves the visibility pass (expected: entities leave the world), what attaching
53 layers costs (no disk reads, but every entity's components are rebuilt), and what repeated
detach/attach does to the layer storage (`[stability] layer_storage_leak`). GOG has not been
looked at; the `HideLayerGroup()` warning string is the anchor there.

### `[loading] hide_kept` (default 1, only with `release_kept`; seen working in game once, Steam)

At the same point where `release_kept` would unload, the kept layers are hidden instead
while at least `area_memory_mb` (1 536 MB) of address space is free:

* **How.** The call `00A5A92A` (GOG `00AAE743`) to the visibility predicate goes through
  `kh_pred_stub`, which answers "not visible" for the layer infos in a DLL-owned sorted set and
  passes every other layer to the game's predicate. The release switch to the current partition
  is queued as for an unload; `hook_PartRun` fills the set before the job runs, and the job's own
  attach stage (`FUN_00b05a52` on the root group) detaches them. No unload, no GC, no game flag
  written.
* **Back.** Within half of `release_kept` from the door the same kind of switch empties the
  set and the job attaches them again, before Geralt reaches the door. Any other switch (a door,
  an area load) first empties the set, and its own job attaches them; a stream switch (mode 0)
  gets them synced at once with `FUN_00a5a90e` (GOG `00AAE727`). A save load never touches the
  old world's pointers.
* **Memory.** Less than `area_memory_mb` free while hidden: the next check unloads them as
  `release_kept` does. `hide_kept=0`: always unload.
* **Light layers stay (2026-11-12).** With everything hidden, every crack in the cave's roof
  showed blue sky and clouds where the kept state showed rock (owner, both days). The game's own
  unloading takes the same layers away, so that is most likely the game's own picture there (not
  compared with `interiors=0`); the owner prefers the rock. What is hidden around the quarry,
  read from `levels\03_camp` in `pack0.dzip` (52 layers that only the quarry's partition lists):
  ~870 rocks in eight `rocks` layers, ~500 cooked meshes in layers with no entity, three foliage
  layers (1 to 3 `CFoliageEntity` each, with 8 to 30 foliage containers), four terrain tiles (one
  entity with a `CTerrainTileComponent` each), and a few layers of a handful of plain mesh
  entities: two cliff meshes in `common_dwarf_quarry\rocks` (between Vergen and the quarry, where
  the secret passage runs), two in `common_battle_dwarf_quarry\rocks`. The cave itself spans
  463 x 618 m under most of the quarry, so "what lies over the cave" picks nearly every layer and
  was dropped as a rule.
  * First rule, run in game the same day (`docs/validation/hide-light-v1-2026-11-12.log`): a kept
    layer of 1 to 8 entities stays. 26 of the 53 layers stayed (`... rocks rocks ... tile 000x001
    ... grass grass ... grass ...`), the owner saw the rock through the cracks again, and the
    frame rate was 55-60 at the cliff: 5.2-5.4 M range calls per 5 s, as with nothing hidden
    (5.1 M), against 0.4 M with everything hidden. So the entity count says nothing about the
    cost: almost all of it is in layers of one to three entities, the foliage or the terrain
    tiles (not told apart by that run).
  * Second rule, run the same day (`docs/validation/hide-light-v2-2026-11-12.log`): 1 to 8
    entities, every one a plain `CEntity` (its own vtable, Steam `00E807C4`, GOG `00F236FC`, both
    read from the exe's RTTI by `tools/dump-engine-classes.py`), and not under `hires terrain\`.
    9 layers stayed (`comet rocks rocks one_way_obstacle watter water l03_quarry l03_ravine
    work`), 44 were hidden; 0.36 M range calls at the cliff, and the sky was back in the cracks.
    So the cliff meshes are not what the cracks show: it is the terrain, seen from below.
  * The rule now (`kr_is_light`), run the same day (`docs/validation/hide-light-v3-2026-11-12.log`):
    the same without the terrain exception. A layer stays when it has 1 to 8 entities and every
    one is a plain `CEntity`: a mesh holder or a terrain tile. Any subclass (foliage, a harpy
    nest, an animal manager, a container, an item) makes the layer heavy or alive, and it is
    hidden. 13 layers stayed (the 9 above and the four terrain tiles), 40 were hidden; 0.35-0.5 M
    range calls at the cliff; owner: the rock is in the cracks and the frame rate is good. So the
    cost of the kept outdoors there was the three foliage layers, not the rocks and not the
    terrain. `hide_kept=2` hides everything. Log: `interiors: N light layers (at most 8 plain
    entities each: terrain, single meshes) stay in the world: <names>`.

### What a door between two areas swaps: stand-ins and environments (2026-11-12; seen working in game once, Steam)

Owner, same run, on the way out of the cave: a black wall stood between Geralt and the quarry
until he walked through it, as around the door of a house in the unmodded game; the ground
outside the cave mouth was much darker than before he went in; the sky there was a natural blue
with clouds, where it had been a hazy cyan without clouds before. Log, three seconds after the
door out: `doors: lighting blends ... 'AreaEnvironment_dwarf_mine' 105 at 1.00`.

The cave's partition lists three things of its own that the game unloads at that door and the
mod kept (`keep_outdoors` keeps what a door leaves, and none of them had a class):

| Layer | What it is |
|---|---|
| `secret_passage\black_plane.w2l` (215 bytes, one mesh) | the black plane in the cave mouth: what the cave shows in place of the unloaded quarry |
| `secret_passage\shadow_box.w2l` (one entity) | a box around the cave that keeps the sun out of it; a plain entity, so `kr_is_light` even left it in the world when the rest was hidden |
| `environments\environment_dwarf_mine\environment_dwarf_mine.w2l` | the cave's area environment (priority 105): its light, fog and sky. It covers the cave mouth, so it stayed active outside |

The same kinds of layer in the five worlds (`pack0.dzip`): `black_plane*`, `dark_plane*`,
`shadow_plane*`, `dark_fog`, `assas_cave_black`, the three `old_tower\dark_planes\...\planes`
layers, six `shadow_box` layers; and 45 / 10 / 3 / 38 layers under `environments\` in Act 2, Act
3, the Arena and Act 1 (the prologue spells it `environmets\` and is left as it was).

Now (`ko_class`, `ko_stand_in`, `hook_PartRun`):

* The stand-ins are `LC_SWAP`: unloaded at the door that leaves their area and loaded at the
  door that enters it, as the game does; never preloaded, never counted by the door check.
* An environment that is no interior's is the new class `LC_ENV`. A door between two whole
  areas (`ko_house_door` says neither side is a house or a small place) swaps it; a house door
  keeps its street's, as before; it is not preloaded with a neighbour area, so it loads at
  that area's door (one layer).
* Not changed: inside the cave the black plane stands in the mouth, as in the game and as
  before this change (the mod has always loaded it there).

The sky: the hazy cyan before the cave is the quarry without any change by the mod (the cave's
layers were loaded but hidden then, its environment not active). The blue sky with clouds after
it was the cave's environment still active outside. With the swap the sky after the cave
should be the one from before it.

Seen in game (owner, 2026-11-12, Steam, DXVK, `docs/validation/area-door-swap-2026-11-12.log`): in
to the cliff and out, then in and out again. Owner: no black wall, the light and the sky outside
as before going in.

| Step | Log |
|---|---|
| Door in | `13 preloaded layer(s) of 'common\secret_passage' joined the world` (16 before: the plane, the box and the environment now come with the door), `keep_outdoors kept 129 of 132 layers, unloaded 3; loaded 3` |
| 31 m in | `hid 39 kept outdoor layers`, `13 light layers ... stay in the world` |
| Door out | `no fade (0 to load, spinner hidden)`, `kept 90 of 93 layers, unloaded 3; loaded 1`, and 2.6 s later `lighting blends ... - no area environment active` (the run before: `'AreaEnvironment_dwarf_mine' 105 at 1.00`) |
| Quit | `no loading screen up`, `ExitProcess(0)` |

#### The first visit keeps its fade (2026-11-12; seen in game on Steam)

A door without a fade is decided before its job exists, from the layers the target still has to
load (`hook_DoorStart`, `ks_missing`): up to 16 it goes without a fade. The count leaves out what a
door swaps, so with the stand-ins and the environment now the door's own the cave's 17 became
14, and its first visit (with `preload_areas` off, the default) went without a fade: 13 layers in
89-99 ms on Steam and GOG, a freeze with nothing to hide it
(`docs/validation/release-0.12.2-steam-2026-11-12.log`: `no fade (14 to load)`, `took 99 ms - it keeps
its fade`). In 0.12.1 that door had the 0.25 s area fade. 16 was set for houses, whose layers are
small. A door between two whole areas now goes without a fade only with at most 4 layers to load
(`KO_MISSING_AREA`): kept or preloaded areas, where 0 or 1 is left. Otherwise it keeps the area
fade, as in 0.12.1; the log says `still has N layers to load - 0.25 s fades`. Seen in game (owner, Steam, `docs/validation/area-door-first-visit-fade-2026-11-12.log`): the first way in `still has 14 layers to load - 0.25 s fades`, the way out and the later ways in `no fade (1 to load)`.

#### GOG, everything of this section (owner, 2026-11-12)

GOG 3.5.0.26g, the same build and day (`docs/validation/gog-area-preload-2026-11-12.log`, DXVK, the Steam save copied over): the same round, loaded twice. Owner: all of it worked.

| Step | Log |
|---|---|
| First load | `the doors of 'common\quarry' lead to 3 partition(s): 2 whole area(s) in its folder (2 new ...)` |
| Second load | `2 to load now, 'common\harpy_lair' (22) 'common\secret_passage' (27)`, `32 of 32 layers of 2 area(s) behind the doors loaded, hidden until their door`; job 2.6 s |
| Cave door | `no fade (0 to load)`, `13 preloaded layer(s) ... joined the world (18 stay hidden)`, `unloaded 3; loaded 3` |
| 31 m in | `hid 39 kept outdoor layers`, the same `13 light layers` as on Steam; shown again at 15 m (8 ms) |
| Door out | `no fade`, `unloaded 3; loaded 1`, 2.6 s later `no area environment active` |
| Quit | `no loading screen up`, `CorExitProcess(0)` |

Not run on either build: the other areas with such layers (Act 1's `spa_cave` and `madhouse`,
Act 3's `area_center`), a house door of a street that has an environment of its own, the
memory paths (`preload: only N MB ... unloading the preloaded area layers`, a kept area unloaded
instead of hidden), and other large interiors, where the kept outdoors may cost something other
than foliage.
* **GOG.** Same functions, bytes and convention (EAX = layer info); found from
  `HideLayerGroup() called when no world is loaded.` (`00ECE518`, native `00AE70B8`, group setter
  `00B3F3F7`), and the GOG predicate reads the game object at `01475740`, GOG's `VA_GAME`. Not run
  in game on GOG.
* **Log.** `interiors: hid N kept outdoor layers D m from the door (job T ms, F MB free); shown
  again within D/2 m of it or at the next switch`, `interiors: showed N hidden layers again,
  Geralt D m from the door (job T ms)`, `interiors: N hidden layers out of the hidden set before
  a door - its job attaches them`.

#### Seen in game (owner, 2026-11-11, Steam, DXVK, 4K, `release_kept=30`, `hide_kept=1`)

`docs/validation/hide-kept-2026-11-11.log`: the save in front of the cave, two rounds in to the
cliff and out again, then Quit.

| Step | Log |
|---|---|
| Hide, 30-31 m in | `hid 53 kept outdoor layers ... (job 38-39 ms, 2081 / 1997 MB free)`; no GC (the unload took 72-74 ms) |
| Hidden | `distance stats` ~0.34-0.40 M range calls per 5 s at the cliff, ~3.2 M just before the hide |
| Show, 14 m from the door | `showed 53 hidden layers again ... (job 11 ms)` |
| Door out | `is loaded - no fade (1 to load, spinner hidden)`, jobs of 8-11 ms, both rounds |
| Quit | clean, no dump |

Owner: everything as expected. Through a crack in the tunnel roof before the cliff the sky now
shows where the kept state showed rock: the hidden layers include the quarry's rocks above the
tunnel; with the game's own unloading the sky shows there too (not compared with
`interiors=0` at that spot). Free address space went from 2 081 to 1 997 MB between the two
hides, three minutes apart; whether hide and show cost memory over many rounds is not known yet.

### Research: preloading the small areas behind an area's doors (2026-11-11, not built)

`preload_interiors` loads building interiors only. A small area behind a door of the same
folder (the quarry's `common\secret_passage`: 16 layers, 96 ms) is loaded on the first visit,
behind the 0.25 s area fade, and only kept from then on. Owner's wish: load such areas with the
area while memory allows, so the first visit has no fade either.

Which partitions are behind the area's doors is in the doors themselves (Steam, static):

| What | Where |
|---|---|
| `CStreamingDoor` | class size `0x250`, vtable `00EF0084` (slot 78 = `00EF01BC` OnTimer, slot 153 = `00EF02E8` start, both already mapped); registered by `FUN_00502779` |
| `inside` / `outside` | `SDoorPartition` at door `+0x200` / `+0x214` (property registration `FUN_0050282a`); its first field is the partition name String (the door start reads it so) |
| A layer's entities | layer object (info `+0x38`) `+0x50` array, `+0x54` count (`FUN_00a8ba97`, `FUN_00a8bb48`) |

Two ways to use it:

1. **At run time.** After an area load, walk the loaded layers' entities for that vtable and
   read both partitions of each door. The neighbours are only known once the area is loaded,
   so they would load in a second job (a queued switch to the current partition, as for
   `release_kept`) right after the loading screen: a short hitch there instead of a fade at the
   first door.
2. **From the offline survey.** The survey of `pack0.dzip` read every door's partitions (153
   doors, 106 pairs, 21 of them area doors). A table of the area-door pairs in the DLL would let
   the area load add the small neighbours to its own last bucket, behind the loading screen, as
   it does for interiors. The survey output was not kept; it has to be run again.

Either way: only neighbours in the same folder that are not houses, at most a few dozen layers
each, under the `preload_memory_mb` guard; their black planes (`black_plane*`, `dark_plane*`,
which belong to the area changes, see "The survey behind the new rules") could then be left out
like the fake rooms. GOG: same offsets (`+0x200`/`+0x214`, registration `005AFCC5`), class size
`0x250`, vtable `00D7A394`.

### `[loading] preload_areas` (default 0; third version, seen working in game once, Steam)

The small areas behind an area's doors load with the area, in its own job, and stay out of the
world until their door is used.

**Two versions that loaded them right after the area (2026-11-11, both dropped).** The doors can
only be read once the area's layers are loaded, so the first version called the switch a second
time (mode 4, same partition) after the area load's switch had returned, and the second ran the
area's job once more from stage 3 with one bucket of the DLL's. Both loaded the two areas
(`'common\harpy_lair' (20) 'common\secret_passage' (17)`, 167-285 ms) and the cave door went
without a fade. Both sessions also ended with a loading-screen object left on the render thread
(`+0x26C` set, `+0x268` 0): `release_kept` then waited for "a loading screen is up" for the rest
of the session (5.1 M range calls at the cliff, ~55 fps), and the first session's Quit crashed
(`docs/validation/preload-areas-v1-2026-11-11.log`, `preload-areas-v2-2026-11-11.log`). The extra
load was blamed for that object and was not the cause: the run of 2026-11-12 below left the same
object after a plain load with nothing added. What the object is and how it is dealt with is in
`quit-crash.md`, "A loading-screen movie still alive at the GUI shutdown". The frame rate was
lost to the check alone: `release_kept` took the object for a loading screen. Now only `+0x268`
(a viewport while a loading screen is up) blocks a release.

**Now** (`pa_*` in `src/enginefix.c`):

* **Learn, do not load.** After an area job (`hook_PartRun`, mode 1 or a door into another
  folder, job stage 5) `pa_learn` walks the loaded layers' entities (layer `+0x50`/`+0x54`) for
  the `CStreamingDoor` vtable and notes both partitions of each door. The other partitions of
  the same folder that are not houses (`kx_small`) are remembered as pairs (area, neighbour), in
  memory and in `<game>\bin\witcher2_enginefix_cache\area-doors.txt` (one `area<TAB>neighbour`
  per line, UTF-8). Nothing is loaded at that point.
* **Load with the area, from its next load on.** `pa_plan` runs before the area's job: for every
  known neighbour of the target, the layers of its own groups that are not loaded (`ks_walk`: no
  sound, occluders, interior environment or fake rooms), 1 to 64 per area, go to the end of the
  job's last bucket, after the interiors and the street and before the fake rooms. One run, the
  game's own; no second switch. Layers the job lists itself, interiors and layers added by the
  other rules are left out of the plan.
* **Loaded, not attached.** The planned layers go into `g_paSet` before the run, and
  `kh_pred_stub` (the visibility predicate's one call, as for `hide_kept`) answers "not visible"
  for them, so the job's attach stage leaves them out of the world, as it does for a layer group
  a quest has hidden. They cost address space, and nothing in the visibility pass.
* **At the door.** `hook_DoorStart` finds nothing to load (`ks_missing`): no fade. The door's job
  lists for unloading the loaded layers its partition does not hold; a hidden layer that is not
  in that list belongs to the partition being entered and leaves the set before the run
  (`pa_keep_listed`), so the same job attaches it. Other neighbours stay hidden. From then on the
  area is an ordinary kept area (`keep_outdoors`, `release_kept`, `hide_kept`).
* **Memory.** The plan needs `area_memory_mb` free (or `preload_memory_mb`, if that is higher),
  and so does every single layer (`kp_layer_ok`): what does not fit loads on its first visit.
  Once less than `area_memory_mb` is free during play, `pa_frame` (once a second, game thread)
  queues a switch to the current partition and `hook_PartRun` lets the hidden layers in its
  unload list go, with the GC, as `release_kept` does. A door left to the game for memory
  unloads them with everything else. After every job the set drops what is no longer loaded
  (`pa_prune`); a stream switch, an area load or another world empties it (`pa_clear`).
* Needs `interiors=2` (`keep_outdoors` and `preload_interiors`) and the visibility hook; without
  them it is off. GOG takes the same path (no new address beyond `hide_kept`'s).
* Log: `preload: the doors of '<area>' lead to N partition(s): K whole area(s) in its folder (M
  new - they load with this area from its next load on), ...`, `preload: K area(s) known behind
  the doors of '<area>': N to load now, '<name>' (layers) ...`, `preload: X of Y layers of K
  area(s) behind the doors loaded, hidden until their door`, `preload: N preloaded layer(s) of
  '<area>' joined the world at this switch (M stay hidden)`, `preload: only F MB of address space
  free (under 1536) - unloading the N preloaded area layers`.

So the tiers by free address space are: under `door_memory_mb` (1 024) the game's own doors; from
there house doors keep their street; from `preload_memory_mb` (1 280) interiors preload; from
`area_memory_mb` (1 536) area doors keep both areas, kept layers are hidden instead of unloaded,
and neighbour areas preload hidden.

#### Seen in game (owner, 2026-11-12, Steam, DXVK, 4K, `release_kept=30`, `hide_kept=1`, `preload_areas=1`)

`docs/validation/preload-areas-v3-2026-11-12.log`: the save in front of the cave loaded twice,
then in to the cliff, out, in and out, in for ~50 m and out, Quit. Owner: no frame drops at the
cliff.

| Step | Log |
|---|---|
| First load | `the doors of 'common\quarry' lead to 3 partition(s): 2 whole area(s) in its folder (2 new ...)`; `area-doors.txt` holds the two pairs |
| Second load | `2 area(s) known ...: 2 to load now, 'common\harpy_lair' (24) 'common\secret_passage' (30)`, `37 of 37 layers of 2 area(s) behind the doors loaded, hidden until their door` (17 of the 54 planned are the quarry's own too and load as that); job 2.9 s, as without them |
| Cave door, first time | `is loaded - no fade (0 to load)`, `16 preloaded layer(s) of 'common\secret_passage' joined the world at this switch (20 stay hidden)` |
| 31 m in | `hid 53 kept outdoor layers ... (job 40 ms, 2044 MB free)`; shown again at 14 m (11 ms); twice |
| Four more doors | all `no fade (0 to load, spinner hidden)`, jobs under 60 ms |
| Quit | `no loading screen up`, `ExitProcess(0)`, no dump |

Not explained: one of the 17 cave layers was no longer loaded after the first door's job (`16
joined`, `20 stay hidden` of 37); the door check of the hide test of 2026-11-11 also counted `1 to
load` there on every later door. Not covered by a test yet: the memory drop (`area_memory_mb`
above the free address space while the areas are loaded), and GOG.

### Quest markers inside buildings: Poker Face: Vergen (2026-11-12; the selection seen in game, the quest not)

**Symptom** (a player on Nexus, 0.12.1): the last dice player of *Poker Face: Vergen* skips the
game and declares Geralt the loser, also when met in the street; with 0.12.0 he played there.
The game has that fault on its own when he is spoken to in his house at night; the old way round
it was to meet him outside.

**Cause.** The quest (`game\2_act2\sq204_dice.w2phase`) holds a `CQuestTagsPresenceCondition` on
the tag `sq204_wp_inside_building`, and a `QStartMinigameEx` script block that names
`game\minigames\dices_inside.w2ent` and the same tag: "is the indoor waypoint in the world?" is how it
learns that Geralt is indoors. The waypoint is a layer of one entity, `sq204_wp_inside.w2l`, in
the folders of three Vergen houses and the inn (`dwarf_city\building09_dec`, `building12_dec`,
`building15_dec`, `building_inn_dec`). In the game it is loaded only while Geralt is in that
house. 0.12.1 preloads Vergen's houses, every layer in their folders, so the waypoint was in the
world all the time and the quest took its indoor branch in the street.

**How many more.** Read offline from `pack0.dzip`: the 512 distinct tags of every
`CQuestTagsPresenceCondition` in the game's `.w2phase` and `.w2quest` files, against the name
tables of the 178 layers inside the buildings of the five worlds (Vergen's `dwarf_city\building*`,
Act 1's `\buildings\...\interior\`, the prologue's `_ints\` and `containers\...\interior*`):

| Layer | Holds | Tested by |
|---|---|---|
| `dwarf_city\building09_dec`, `12`, `15`, `building_inn_dec` `\sq204_wp_inside.w2l` | one waypoint, `sq204_wp_inside_building` | `sq204_dice` (dice in Vergen) |
| `dwarf_city\building_inn_dec\armwrestling.w2l` | the inn's arm wrestling spot, `sq205_armwrestling_spot_vergen_inn` | `sq205i_handwrestling` (it also knows `..._vergen_outdoor`) |
| `village\buildings\02_anezka_village\interior\sq107_troll_head_zegota\sq107_troll_head.w2l` | the troll's head (in a group of its own) | `sq107_bridge_troll` |
| `pier\buildings\06_custom_house_pier\interior\decorations.w2l` | the furniture of the customs tower, tagged per floor (`q110_tower_hay01_floor01` ...) | `q110_barge` (the burning tower; not looked into here) |
| `dwarf_city\building14_dec\door_stream\door.w2l`, `dwarf_city\buildings.w2l` | doors and the town's shell | several; they are in the world in the game too |

**Fix** (`kq_marker_name`, `kq_marker`, `kp_walk`, `ko_walk`, `hook_PartRun`): a layer inside a
building (under an interior folder, or directly in a group only a building lists) that is named
after a quest (`q` or `sq`, three digits, `_`) or is called `armwrestling` holds no room. It is
not preloaded and not kept: it loads at its building's door and is unloaded by the next door
job that does not switch to that building, as in the game. That is the four waypoints, the arm
wrestling spot and the troll's head, and two more that the survey by folder did not have:
Philippa's house lists two quest groups of its own, `quests\streamable\q206_figurine` and
`quests\streamable\q215_blockade` (the latter one cooked layer of 248 bytes), which the game
also loads only inside that house. The customs tower's furniture is ordinary room content and
is not touched by this. Log, on an area load: `preload: 7 quest marker layer(s) inside buildings
are left to their doors (a quest tells from them where Geralt is): <paths>`, and the preload
count for Act 2 goes from 59 to 52 layers.

Not covered: the kept outdoors and a kept area are still in the world while Geralt is indoors
(since 0.8.0), so a presence test for something in the street is true inside a house where the
game says false. No report of that; the full fix would be to keep kept layers out of the tag
test itself.

Seen in game (owner, 2026-11-12, Steam, `docs/validation/quest-markers-2026-11-12.log`): an Act
2 load logs the seven layers by path and `52 of 52 preloaded layers loaded`. The owner is on
Roche's path and cannot reach the dice players or those houses, so not seen: the dice player in
the street playing, and house 09, 12 or 15 or the inn loading its marker at the door and the
door out unloading it. GOG takes the same code; not run there with this change.

### Still to check in game

Not seen: Vergen's other 20 houses (Iorveth's path opens the whole town; they are covered by
the offline check only), open doors and windows looked at from the street on purpose, the
prologue and Act 3 with this build. For the memory path: `preload_memory_mb=4000` skips every
preloaded layer, and the fake rooms must then stand behind the doors as in the game. An area
door with `area_memory_mb` above the free address space.
