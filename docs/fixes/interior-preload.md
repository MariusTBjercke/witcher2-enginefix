# Interior preloading: can doors be seamless?

Status 2026-11-02: shipped in 0.8.0 as `[loading] interiors=2` (default). The sections below
follow the work in order; the run logs are in `docs/validation/`.

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
