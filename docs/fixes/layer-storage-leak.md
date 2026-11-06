# Layer storage leak: every save load leaves the world's NPC states behind

Status 2026-11-06: found in a running game (Steam, five-hour session), proven in the code on Steam
and GOG, fixed by `[stability] layer_storage_leak=1` (default). Run on Steam: five loads, ~6 570
states freed per reload, no orphans left (`docs/validation/layer-storage-leak-2026-11-06.md`).
Run on GOG (2026-11-06, Flotsam, four loads): both calls hooked, 10 276 states (27 MB) freed;
after the first load private memory rose 36 and 9 MB per load.

## Symptom

Over a long session the game's 32-bit address space fills up: in one evening (Steam, DXVK, Act 1,
many save loads around the Kayran fight) private memory went from 1 299 MB after the first load
to ~2 800 MB, and the free address space from 2 165 MB to 350-550 MB. From about an hour in, the
interior preload stopped (`preload: 0 of 64 preloaded layers loaded ... floor 1280 MB`) and later
every door fell back to the game's own (`... with only 432 MB of address space free - the game's
door`), so the seamless interiors were gone. Loading the same area again did not bring the memory
back: Binduga took 1 299 MB at 18:53 and 2 794 MB at 22:37.

## What fills the address space

Read from outside the process (`VirtualQueryEx` and `ReadProcessMemory`, no breakpoints, no
suspension) at 2 795 MB private:

* The engine's allocator takes its memory in 64 KB pages, one `VirtualAlloc` + `VirtualLock` each
  (`FUN_00ac84cd`, the only `VirtualAlloc` call in the exe; page table at `+0x1600FC`, indexed by
  address >> 16). 18 190 such pages, 1.14 GB.
* 6 408 of those pages (400 MB) begin with `BLCK` + `entityBase`: entity state written by the game
  file saver (`CGameFileSaver`, vtable `00E7E22C`; `BLCK` written in `00A985D9`). Each page holds
  16 such states of ~4 KB (a 4 KB size class): position, appearance (`scoiatael_man_07`,
  `loredo_guard_03`, `_flotsam_light_keeper__a1` ...), components. That is ~100 000 states for an
  area with a few hundred NPCs.
* 3 094 of those pages are byte-identical copies of another (MD5 over the whole 64 KB), up to 22
  copies of the same page: the same states in the same order, written again and kept.
* 3 304 pages (207 MB) are Havok packfiles (`Havok-6.5.0-r1`, `hkaSplineCompressedAnimation`),
  all distinct. Not looked into further.

## Who owns the states

A pointer scan over all committed memory, naming each owner by the RTTI of the nearest vtable
before the pointer:

* 81 692 pointers into the state pages sit at `+4` of a `CGameDataStorage` (vtable `00E7E264`,
  RTTI name `.?AVCGameDataStorage@@`): a 0x14-byte object, vtable then a `TDynArray<Uint8>`
  (data, size, capacity) of the serialized state.
* There are **132 774** `CGameDataStorage` objects, 123 208 of them 0x18 apart (a 24-byte size
  class, so allocated one by one). Only **10 075** are pointed at by anything in the process. The
  other **122 699** hold 285 MB of state (370 MB of buffer capacity) that nothing can reach.

## The code

`CGameDataStorage` is created in one place, `FUN_00a970b8`, called from `FUN_00afe3fe`. That
function belongs to the per-layer entity state store, whose save block is named `layerStorage`:

* The store is an array of record pointers. A record is 0x1C bytes: the entity's idTag (5 dwords)
  at `+0`, its state (`CGameDataStorage*`) at `+0x14`, spawn data (0x70 bytes) at `+0x18`.
* `FUN_00afe46f(store, entity)` looks the record up by idTag (or adds one) and calls
  `FUN_00afe3fe`, which creates `+0x14` once and then rewrites it in place. Callers: the layer
  unload `FUN_00a8ba97`, the entity detach `FUN_00a8b931`, the save writer `FUN_00afe841`.
* `FUN_00afe3b1` restores an entity from `+0x14` (a loader over it, entity vtable `+0x1F4`) and
  leaves the storage in the record. `FUN_00afe7c6` writes it to the save. Neither frees it.
* **The record destructor `00AFE0A2` (EAX = record, plain RET) frees `+0x18` and never `+0x14`.**
  Its two callers free the record right after it:
  * `004461B1` clears the whole array (call at `004461C8`). It runs from the save load
    `FUN_00afe9f2` ("layerStorage", "numEntities"), which then reads every record from the save
    with a new storage each (`FUN_00afe743`, `+0x14` from the loader's vtable `+0x18`), and from
    `FUN_00afe392` (the store's teardown) and `FUN_00a9eed2`.
  * `00AFE12C` removes one spawned entity's record (call at `00AFE1C3`).

So each save load leaves every state stored in the world behind, and loads a fresh copy from the
save: the identical pages above. A Flotsam load stores the town's communities; the Kayran arena
was loaded 11 times in a row that evening, about +20 MB each.

## The fix: `[stability] layer_storage_leak`

`src/enginefix.c`, section "layer storage leak". Both `call 00AFE0A2` sites are redirected to
`ls_record_hook` (same convention: record in EAX, no stack arguments). It reads `+0x14`, and when
that object's vtable is `CGameDataStorage`'s, clears the field and deletes the object with its own
scalar deleting destructor (slot 0, `00421438`, thiscall, flags 1, `RET 4`: frees the array at
`+4`, then the object), then jumps to the original destructor. The callers keep EBX, ESI and EDI
across the call (the clear loop uses EBX/ESI/EDI, the removal EDI); the C function keeps them.

| | Steam 3.5.0.26 | GOG 3.5.0.26g |
|---|---|---|
| Record destructor | `00AFE0A2`: `57 8B 78 18 85 FF 74 24` | `00B37334`: `56 8B 70 18 85 F6 74 22` |
| Array clear call | `004461C8` (after `8B C7`, in `004461B1`) | `0044F348` (after `8B C3`, in `0044F334`) |
| Single removal call | `00AFE1C3` (after `8B C7`, in `00AFE12C`) | `00B3746F` (after `8B C2`, in `00B373CD`) |
| `CGameDataStorage` vtable | `00E7E264` | `00F21290` (RTTI `01151A7C`, locator `00FB57E8`) |
| Its destructor (slot 0) | `00421438`: `55 8B EC 56 8B F1 8D 46 04 E8` | `00423212`: `55 8B EC 56 8B F1 8D 4E 04 E8` |

GOG was found from the RTTI name and the `layerStorage` strings (`00B37ABE` save, `00B37C82`
load, which clears with `0044F334`); the record layout is the same (`00B376C0` sets `+0x14` and
`+0x18`). Each byte pattern matches once in its exe. Every rel32 lies inside one cache line.

The worker retries the byte checks for up to 100 ticks (~12 s), then patches both calls with
`patch_call`. Log:

```
stability: layer storage leak fix: array clear hooked, single removal hooked
stability: layer storage: N saved entity state(s) freed so far (M MB) that the game would have kept
```

The second line comes at most every 30 s while the count grows, and once more at the DLL detach.
`0` turns the fix off; without the addresses (an unknown exe) `build_gate` turns it off.

## Finding the next one: `[diagnostics] mem_trace`

The census that found this leak was taken from outside the process. `[diagnostics] mem_trace=N`
(`src/enginefix.c`, section "memory trace") takes the same census inside the game every N
minutes, the first after 2, on a low-priority thread. It only reads: `VirtualQuery` over the
address space, and every aligned dword of committed private read/write memory that is a vtable
of witcher2.exe is counted in an 8 MB open-addressing table (its own `VirtualAlloc`, skipped by
the scan). The vtables are mapped once before the first census, one bit per dword of the image:
those whose RTTI locator checks out (signature 0, type descriptor name `.?AV`/`.?AU`). A page
that goes away during the scan ends that region (SEH).

The first build counted every value inside the image and checked RTTI afterwards. In Flotsam at
2.1 GB private (2026-11-06) texture and sound data gave more distinct "addresses" than the table
has slots; with the table full every add walked all of it, and the first census never ended
(no `memtrace: #1` line after 18 minutes, the game itself unaffected). Only vtables are keys
now, and a probe stops after 4096 slots.

* Log, per census: `memtrace: #N at T min: private, free (largest); 64 KB pages: state, havok,
  small, other; K classes; the six classes that grew most since the first census (ms)`.
* `witcher2_enginefix_memtrace.tsv` next to the log, one row per value (`census, seconds, kind,
  key, value`; classes with 20 or more objects). The session before is kept as `.prev.tsv`.
* `python -I tools/memtrace-report.py <tsv> [top]` prints the series and the classes that grew
  from the first census to the last, with how many steps they rose and fell; a class that rose in
  three quarters of the steps and never fell is listed as a leak candidate.

Counts include freed memory that still holds a vtable, and a class with several bases has one
row per vtable, so a row is "objects plus remnants"; only steady growth means something. The
census found here: ~2.6 s from outside, for ~1.5 GB of private memory.

In game (Steam, DXVK, Flotsam, 2026-11-06 17:20-17:29): the vtable map took 484 ms (4 675
vtables), census #1 3.9 s and #2 6.8 s on the low-priority thread, with no stutter reported.
Between minute 2 and 7, walking through doors with no save load, private memory rose from 1 484
to 1 956 MB while the engine's own 64 KB pages grew by ~240 (15 MB): most of that growth lies
outside the engine's page allocator. Classes that grew most: `CCurve` +10 263, `CFXTrack` +2 160,
`CJobImmediateUpdateTransform` +1 979, `SBehaviorGraphInstanceSlot` +1 654, the other `CFX*`
effect classes, `CDebugAnimation` +1 215 and `CVisualDebug` +537. One step proves no leak; a
longer session decides.

## Risks and what to check in game

* A state still used after its record is gone would now be freed memory. Nothing in the code reads
  `+0x14` outside the record's own functions, and in the running game nothing pointed at any of
  the 122 699 orphans, across many generations of loads.
* Check: load a save several times in a row (the same area) and watch the free address space in
  the `savelist:` or `preload:` lines stay flat instead of falling ~20-150 MB per load; the freed
  line should grow by roughly the world's stored states each time. NPCs that were changed (moved,
  killed, looted) before a save must still be in that state after loading it twice.
* Not explained by this leak: the rest of the growth (~1.1 GB that evening), including +415 MB in
  two minutes in Binduga without a load, and the Havok animation pages.
