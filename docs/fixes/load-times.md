# Load times: save loading, area transitions and interiors

Status 2026-11-02: measured; `[loading] fast_doors` implemented and confirmed in game (runs 3-4).
`[diagnostics] load_trace` (off by default) exists, two in-game runs are in
`docs/validation/load-trace-run1-2026-11-02.log` and `load-trace-run2-doors-2026-11-02.log`.
Target: Steam Enhanced Edition 3.5.0.26. The ranked options and the fast-doors change are at the end.

## Question

Can loading a save, area transitions and entering/leaving interiors be made faster from the
`dinput8.dll` proxy (the Witcher 1 sibling managed it), and where does the time go?

## How the game loads (static analysis, unpacked 3.5.0.26)

* **Loading screen.** The render thread object `DAT_0160D0A4` (its loop is `FUN_009d1e96`) holds
  the loading-screen movie at `+0x268` and the screen object at `+0x26C` (`+0x10` byte = shown).
  The render command `009DC3FC -> FUN_009d1b9f` sets `+0x268` when a loading screen starts and
  clears it at the end. The screen classes' vtables are `00EC6A20` / `00EC6A44` / `00EC6A5C`
  (Render, Advance, Init `009C32B2`, Hide, FadeOut `009C2F6C`, LoadingComplete `009C2F54`,
  SetProgress `009C3038`).
* **The loading screen is capped at 30 fps and spins.** While `+0x268` is set and no render
  command is queued, the render thread calls `FUN_009d1d61` in a loop; it draws only when
  `>= 1/30 s` passed (QPC, clamped to 1/15 s) and otherwise returns at once (`PeekMessage`, no
  wait). One core is busy for the whole load (96 % of the render thread in run 1).
* **Archive reads.** Game archives and saves use one dzip library (`0068Dxxx`; the game's archive
  mount `FUN_00abae22` and the save reader `FUN_00a4e42e` both call `FUN_0068d430`). Per 64 KiB
  block `FUN_0068e890` does CRT `_lseeki64` + `_read` (`0068E8C1` / `0068E8D4`, unbuffered lowio,
  so one `ReadFile` per block) and unpacks with `FUN_0068f3b0` (cdecl `in, in_len, out, 0x10000`;
  FastLZ-style, level from the first byte's top bits, level 1 at `FUN_0068ec40`). Its only call is
  `0068E8E7`.
* **Imports.** The game imports `Sleep`, `WaitForSingleObject` and `WaitForSingleObjectEx` only
  (no `WaitForMultipleObjects`, `SleepEx`, `SwitchToThread`, `timeBeginPeriod`). Timer resolution
  during both runs was 0.5 ms (set by something else in the process).
* **Interiors have no loading screen: streaming doors.** A streaming door (constructor
  `FUN_0095de8e`, animations `door_streaming_open_from_inside/outside[_npc|_loop]` and
  `door_streaming_finish_loop`) starts a task in `FUN_0095dcc4`, keeps it at `door+0x228` and ticks
  it from its OnTimer method `FUN_0095db00` (door vtable slot `00EF01BC`, thiscall, `RET 8`;
  "StreamingDoorTimer", 1 ms, so once per frame) until the task's Tick (slot 1) returns done.
  Four task types (`door+0x8A*4` by request type):

  | Type | Vtable | Tick | What it does |
  |---|---|---|---|
  | 0 | `00EF03FC` | `00502996` | not seen in the runs |
  | 1 | `00EF0410` | `00502B18` | **every door in run 2**; see below |
  | 2 | `00EF0438` | `00502DC0` | waits until streaming is idle (`*(*(DAT_01404950+0x40)+0x30C) == 0`); fades to black (0.3 s) only if that takes 2.5 s |
  | 3 | `00EF0424` | `00502C7F` | starts on the anim event `start_async` (`00502C89`) |

* **Type 1, the house door, step by step.**
  1. Constructor `FUN_00502a7f`: render command fade to black over **0.5 s** (`00E7C374`, executes
     `009DC316 -> FUN_009d68e7(color, duration)`, immediate `0x3F000000` in the constructor) and
     render command `00E7C07C(1)`.
  2. Tick `00502B18`: once the fade is over (`FUN_00aa72de`, renderer vtable `+0x7C`), queue the
     interior switch `FUN_00aa7e18(this+4, 4, ...)` and set state 1. This is the loading burst.
  3. Animation event handler (slot 2, `00502B5F`): on the door animation's **`fade_out`** event,
     in state 1: render command `00E7C07C(0)` and **fade in over 0.5 s** (`00E7C388`, immediate
     `0x3F000000`), state 2. The next Tick returns done and `FUN_0095db81` runs.

  So a house door lasts until an animation event, not until the interior is loaded.

## The trace (`[diagnostics] load_trace=1`)

`src/enginefix.c`, section "load trace". Passive: every hook calls the real function first and
only counts. Installed by `loadtrace_thread` (started from `DllMain`, the `filetrace_thread`
pattern):

| Hook | Where | Counts |
|---|---|---|
| `ReadFile` | IAT `00CAD27C` | per thread: calls, bytes, time; split by how the handle was opened (`.dzip`, other `CookedPC` = loose, other) |
| `CreateFileW` | IAT `00CAD320` (shared `hook_CreateFileW`) | opens and their time; remembers the handle's kind |
| block unpack | call `0068E8E7` -> `FUN_0068f3b0` (fingerprint `55 8B EC 8B 4D 08 0F B6 01 C1 E8 05`, after the SteamStub decrypted `.text`) | calls, in/out bytes, time |
| `Sleep` | IAT `00CAD304` | calls, `Sleep(0)`, `Sleep(1)`, time slept |
| `WaitForSingleObject(Ex)` | IAT `00CAD2B0` / `00CAD07C` | calls, polls (0 ms), time |
| `IDirect3DDevice9::Present` | slot 17 of the device at `01626788` | calls, time, longest |
| streaming door OnTimer | `.rdata` slot `00EF01BC` -> `FUN_0095db00` | start/end of a door task, ticks, task vtable, faded to black |

Windows: a loading screen is `+0x268` going non-zero to zero (polled every 5 ms); a door window
is one door task from its first tick to the tick that clears `door+0x228`. Per window it logs a
line every 250 ms (all threads summed), then per thread the counters and the CPU time
(`GetThreadTimes`, start address via `NtQueryInformationThread` class 9), process CPU, timer
resolution and `PresentationInterval` / `[Rendering] VSync`. Outside a window, a 250 ms interval
that read or unpacked more than 4 MB is logged as `streaming burst`. Turn it off with
`load_trace=0` (the default).

Start test: both runs started and ran normally with all hooks in (`5/5 import slots`, unpack call
and Present hooked, door timer hooked).

## Measurements (owner's machine, 4K windowed/borderless, "extreme" preset, warm file cache)

Run 1 (17:52): start, load a save. Run 2 (18:00): start, load an older save, two houses in and
out outside Flotsam, into Flotsam through the gates, into the inn, crash on the way out (see
"Crash" below).

### Loading a save from the main menu

| | Run 1 | Run 2 |
|---|---|---|
| Loading screen | **3 329 ms** | **2 984 ms** |
| Process CPU | 6 000 ms (1.80 cores) | 5 844 ms (1.96 cores) |
| Main thread CPU | 2 562 ms (77 %) | 2 562 ms (86 %) |
| `.dzip` reads (main) | 12 283x, 323 MB, 87 ms | 12 283x, 323 MB, 87 ms |
| Loose reads (main) | 3 779x, 262 MB, 261 ms | 3 779x, 262 MB, 264 ms |
| Opens (main) | 513x, **346 ms** | 513x, **37 ms** |
| Block unpack (main) | 9 498x, 323 -> 471 MB, 424 ms | 9 498x, 323 -> 471 MB, 422 ms |
| Sleeps / waits (main) | 4 / 104, 7 ms | 3 / 86, 16 ms |
| Render thread | 96 % CPU, 98 presents | same |

* The load is **CPU-bound on the main thread**: file I/O and unpacking together are ~0.8-1.1 s of
  ~3 s; the remaining ~1.4-1.7 s of main-thread CPU is the engine building the world (not file
  access).
* **No artificial wait**: the main thread sleeps 3-4 times for 1 ms and waits < 20 ms in total.
  There is no minimum loading-screen duration in this path.
* **No vsync**: `PresentationInterval=80000000` (immediate), `[Rendering] VSync=0`. The 30 fps
  cap and the spin are on the render thread; the main thread never waited for it.
* The other two `0066F607` worker threads sleep in `Sleep(1)` loops (~1 000 calls/s each) - idle
  polling, not something the load waits for.
* Opening files cost 0.67 ms each in run 1 and 0.07 ms in run 2 (same 513 files): the difference
  is outside the game (most likely the antivirus scanning each file on first open after a
  restart). The cache was warm in both runs (650 MB read at ~1 GB/s); a cold first load after a
  reboot was not measured.

### Streaming doors (interiors), run 2

| Window | Transition | Length | Ticks | Loaded (all threads) | I/O + unpack time |
|---|---|---|---|---|---|
| 3 | house 1 in | **1 688 ms** | 115 | 9 MB dzip + 27 MB loose | ~55 ms |
| 4 | house 1 out | **1 672 ms** | 115 | 16 MB dzip + 11 MB loose | ~30 ms |
| 5 | house 2 in | **1 688 ms** | 115 | 10 MB dzip + 15 MB loose | ~30 ms |
| 6 | house 2 out | **1 687 ms** | 116 | 16 MB dzip + 11 MB loose | ~30 ms |
| 7 | Flotsam gate / inn | 2 485 ms | 114 | 131 MB dzip + 190 MB loose | ~390 ms |
| 8 | Flotsam gate / inn | 2 125 ms | 114 | 115 MB dzip + 168 MB loose | ~800 ms (two threads) |
| 9 | leaving the inn | crashed at +786 ms | | | |

All doors were type 1 (`00EF0410`); none faded through the type 2 path. Timeline of every house
door: **0-0.5 s nothing is loaded** (the fade to black), **0.5-0.75 s one burst** (the interior
switch, one 250 ms interval), **0.75-1.69 s nothing is loaded again**, then the task ends on the
`fade_out` animation event and the 0.5 s fade-in follows (not inside the window). The length is the
same 115 ticks every time; in windows 7/8 the main thread loaded ~1 s synchronously, frames dropped
(2-4 presents in a 250 ms interval) and the same ~114 ticks took longer in wall time.

So for a house the player sees ~0.5 s fade-out + ~1.2 s black + 0.5 s fade-in = **~2.2 s, of
which ~0.05 s is loading**. The owner's impression ("it doesn't seem to load much") is right.

Main-thread waits of ~1 s in windows 4 and 5 (117x, 1 049 ms / 943 ms) are per-frame waits for
the render thread (GPU-bound frames at 4K "extreme"), present outside doors as well; not loading.

### Open world (Flotsam border)

No stalls: `streaming burst` lines show 4-180 MB per 250 ms (mostly 4 MB loose texture reads,
~1 GB/s from cache) while the frame rate stays at ~72 fps (18 presents per 250 ms). Nothing to
gain there.

## Crash during the test (run 2, 18:02:55)

`0xC0000005` at `0x0084C7AF` (`MOVAPS xmm1, [esi+0x30]` in `FUN_0084c750`, a Havok-style ray cast
against a rigid body's transform), `esi=67091E3C`: a misaligned (not 16-byte) transform pointer, so
the reported address is `0xFFFFFFFF`. Thread 20280 is one of the game's `0066F607` worker threads
(it also did streaming reads). It happened 0.8 s into the door task for leaving the inn, while the
inn's layer was being switched: most likely a physics query against a body that was just freed. The
trace's hooks do not touch that memory (ReadFile/Sleep/unpack only count and pass through), so it is
unlikely to be caused by the logging; it is a separate issue. Dump:
`<game>\bin\witcher2_enginefix_crash_2026-11-02_18-02-55.dmp` (1.9 MB, not committed).

## Options, ranked by expected gain and risk

| # | Option | Expected gain | Risk |
|---|---|---|---|
| 1 | **Doors: start the fade-in as soon as the interior switch is done** instead of waiting for the door animation's `fade_out` event (wrap type 1 Tick `00EF0414`: in state 1, when the switch has finished, do what `00502B5F` does) | **~0.9-1.2 s per house door** (~2.2 s -> ~1.0-1.3 s) | Medium: needs the right "switch finished" condition (RE of `FUN_00aa7e18`'s queue, or the type 2 test `*(*(DAT_01404950+0x40)+0x30C) == 0`); Geralt may still be in the door animation when the picture comes back. Needs an in-game test. |
| 2 | **Doors: shorter fades** (the two `0x3F000000` immediates, 0.5 s -> e.g. 0.25 s, in `FUN_00502a7f` and `00502B5F`) | ~0.25 s per door alone (the fade-in); ~0.5 s together with 1 | Low: two constants, cosmetic. Fingerprinted code patch after the SteamStub. |
| 3 | Save load: opens are cheap in a warm session (37 ms) but 346 ms after a restart - tell users about an antivirus exclusion for the game folder | ~0.3 s on the first load after boot | None for the DLL (documentation) |
| 4 | Save load: faster block unpack (a tuned FastLZ level-1 decoder on the `0068E8E7` call) | <= 0.2 s of 3 s (unpack runs at ~1.1 GB/s already) | Medium (must be bit-exact) |
| 5 | Save load: read-ahead / parallel unpack on idle worker threads | <= 0.4-0.6 s of 3 s (I/O + unpack are ~1 s, the rest is engine CPU) | High (the dzip reader is synchronous per block; threading it is a rewrite) |
| 6 | Loading screen: vsync off / remove the 30 fps cap | ~0 (no vsync, main thread never waits for the render thread) | - |
| 7 | Remove artificial sleeps / minimum durations | 0 (none found in the save-load path) | - |
| 8 | Script compile at start | not part of a save or area load; startup loading screen was 0.4 s | not measured here |

**Recommendation.** Save loading is close to what the engine allows from outside: ~3 s, mostly
single-threaded engine work; options 3-5 together are worth at most ~0.5-0.8 s and 5 is high risk.
The visible win is the interiors: option 1 (+2) cuts a house transition from ~2.2 s to ~1 s
without touching any loading. The owner agreed on 2026-11-02; implemented below.

## Fast doors (`[loading] fast_doors=1`, `door_fade=0.25`) - implemented, confirmed in game

`src/enginefix.c`, section "fast doors". The streaming-door OnTimer slot `00EF01BC` is wrapped
(`hook_DoorTimer`, shared with the trace). After the game's own tick, for a type 1 task in state 1
(interior switch queued) it checks every door tick:

* the game object's world-request array is empty again (`*(DWORD*)(DAT_01404950 + 0xE8) == 0`,
  the count of the array `FUN_00aa7e18` appends to at `+0xE4`), and
* the world reports no pending streaming (`*(DWORD*)(*(DAT_01404950 + 0x40) + 0x30C) == 0`, the
  same test the interior task's Tick `00502DC0` uses).

After two such ticks in a row it calls the task's own animation-event handler `00502B5F` (thiscall
`(event, unused)`, `RET 8`) with a synthetic event named `fade_out`. The handler keeps its own copy
of that CName (`026ADE10`) but sets it only on the first event it receives, which made the first
door of test 3 fall back to the game's timing; the DLL now looks the name up itself with the game's
`FUN_00b28d76` (`__stdcall (DWORD* out, const wchar_t*)`, `RET 8`) on the game's own string
`00EF0050` (`L"fade_out"`). The game then fades in and sets state 2 itself, the next Tick finishes
the task, and the real `fade_out` event later finds state 2 and does nothing. Log line per door:
`doors: interior ready after N ms - picture back now (state 2)`.

`door_fade` repoints the two fades' `movss xmm0, [00E30444]` (`F3 0F 10 05 44 04 E3 00`, the shared
0.5 constant) at `00502ACD` (fade-out, `FUN_00502a7f`) and `00502BD9` (fade-in, `00502B5F`) to a
float in the DLL, after the SteamStub has decrypted `.text`; the constant itself is not changed.
0.5 leaves the code untouched.

Expected: house door ~0.25 s fade-out + ~0.1-0.3 s switch + 0.25 s fade-in, instead of ~2.2 s.
Turn off: `fast_doors=0` and `door_fade=0.5`.

### Run 3 (2026-11-02 18:25, owner): fast doors on

`docs/validation/load-trace-run3-fast-doors-2026-11-02.log`. Start test passed (`doors: fade-out
patched, fade-in patched, 0.25 s`, `stability: ray cast guard installed`). Owner: "much better".
No crash; the ray cast guard never fired.

| Window | Door | Interior ready after | Door task | Before (run 2) |
|---|---|---|---|---|
| 3 | first door of the session | (fell back: event name not set yet) | 1 672 ms | 1 688 ms |
| 4-10 | houses (7 doors) | 110-141 ms | **375-422 ms** | 1 672-1 688 ms |
| 11 | large interior (inn) | 937 ms | 1 235 ms | 2 485 ms |
| 12 | medium interior | 344 ms | 625 ms | 2 125 ms |

Plus the 0.25 s fade-in after the task (was 0.5 s): a house door is now ~0.65 s on screen instead
of ~2.2 s. The first-door fallback is fixed in the next build (name looked up by the DLL): run 4
(18:38, `docs/validation/load-trace-run4-first-door-2026-11-02.log`) shows the first door of the
session at 390 ms (ready after 110 ms) and the next at 375 ms; owner: "looked good". What is left in windows 11/12 is real loading on the main thread (window 11: 113 MB dzip +
104 MB loose, ~0.28 s read + unpack and ~0.7 s engine CPU building the interior); shortening that
would be options 4/5 above.
