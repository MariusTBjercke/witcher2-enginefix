# Startup time: launch to main menu

Status 2026-11-04: `[startup] fast_start=1` and `[saves] light_previews=1` (both on by default)
implemented and measured on Steam, not released yet. Measurements: `docs/validation/startup-time-2026-11-04.md`. Target: Steam Enhanced
Edition 3.5.0.26 and GOG 3.5.0.26g, which ship the same two FMOD files; one run seen on GOG.

## Symptom

A player on Nexus reported ~10 s from *Launch* to the main menu. On the owner's machine it took about
20 s, even with `skip_intro`. The logo videos were the first suspect, but skipping them costs only
0.3-0.4 s (`skip-intro.md`).

## How the game starts

`FUN_00932485` sets up the core ("Initializing system...", "Loading engine configuration...",
"Initializing user content manager...", "Scanning depot...", "Initializing scripts...",
"Initializing PathEngine..."). Then `FUN_00AA6E94` runs, in this order:

| Step | Call | Return address | Owner's machine |
|---|---|---|---|
| RTTI | | | |
| Scripts | `CompileScriptsFromSources` (compiles `base_scripts.dzip` from source at every start) | `00AA6FCA` | **2.0-2.1 s**, CPU |
| Havok | `FUN_00AA0FCC`, `FUN_00AA164F` | | small |
| Game | `FUN_00B12ADA` | `00AA7099` | ~0.15 s |
| Renderer | `[DAT_0160C6EC]+4`; `FUN_009DE0B3` is `Direct3DCreate9`, `shader.cache`, `staticShader.cache` | `00AA712C` | ~0.3 s |
| Sound | `FUN_00A9B469` | `00AA7179` | **10.9 s, of which almost all idle** |
| GUI | `FUN_00A7B263` | `00AA718E` | small |

The game window appears right after this, and the startup movies, the main menu movie and the save
list follow. The window, input, the movie list and `main menu reached` are all in the runtime log.

## The cause: FMOD polling on a coarse timer

From about +4 s to +15 s the whole process used **0 % CPU**. Nothing was being worked on. Stacks from
`tools/bootprofile`:

* Main thread: `FUN_00A9B469` -> `FUN_00A5A3CD` (under `getMusicSystem`, so presumably the music
  system) -> `FUN_00A9DC41` -> event group vtable `+0x24` with mode 1 (by FMOD's API
  `getEventByIndex` with `FMOD_EVENT_NONBLOCKING`) -> `fmod_event.dll` `10036261`:
  `push 0Ah; call 10001130` (`Sleep(10)`), while the list at `10046F50` is empty. That it is a list
  of free async slots is read from the code, not proven.
* FMOD's async thread: `fmodex.dll` `100140D1`: `push 0Ah; call 10002CFB` (`Sleep(10)`), in a loop
  while the file object's state at `+0x1A8` is `0x38`, that is until FMOD's file thread has done the
  read. **This one sets the pace**: each file read costs at least one sleep, however fast the read.
* The timer: until something raises it around the time the window opens, a `Sleep(1)` lasts 15.6 ms
  in this process (`load-trace-run2-doors-2026-11-02.log`: `sleep 30x (1:30) 465 ms` before the
  window; later `291x, 477 ms`, with `timer resolution 0.500 ms`). The game imports no
  `timeBeginPeriod`. `fmodex.dll` and `fmod_event.dll` import `Sleep` but not `timeBeginPeriod`.

So thousands of 10 ms waits each lasted up to 15.6 ms, for work that takes far less.

## The change

`fast_start_thread` in `src/enginefix.c`, started from `DllMain` when `fast_start=1`. It uses no game
address, so it needs no `k_gog` row and has no bytes to check.

1. `timeBeginPeriod(1)` through `LoadLibraryW(L"winmm.dll")`, within ~20 ms of the DLL loading, long
   before the sound step at ~+3 s. Held until the process ends: the running game already has a
   0.5 ms timer from a request that is not ours.
2. Import hooks on `kernel32!Sleep` in `fmodex.dll` and `fmod_event.dll`, found by name with
   `imports_walk` and placed with `patch_import_slot`, which leaves the slot alone unless it holds
   the real export. `Sleep(10)` becomes `Sleep(1)`: on any thread for `fmodex.dll`, on the main thread
   for `fmod_event.dll`. Other durations pass through.
3. When a `W2ViewportClass` window of this process appears (60 s cap), `g_startDone` ends the
   shortening. The hooks stay in place and pass everything through, so FMOD behaves as shipped for
   the rest of the session.

Off switch: `[startup] fast_start=0`. `[general] enabled=0` also turns it off.

Log:

```
startup: skip_intro=1 fast_start=1
startup: fast_start: timeBeginPeriod(1) = 0, Sleep(1) took 11.2 ms before, 1.9 ms after
startup: hooked kernel32.dll!Sleep                 (twice)
startup: fast_start: FMOD waits of 10 ms cut to 1 ms until the game window, after 5437 ms: fmodex 1673, fmod_event 835
```

## Result (owner, one run per row)

| Build | Window | `main menu reached` | Sound step | Busy until about |
|---|---|---|---|---|
| Original | +15.0 s | +16.1 s | 10.9 s | +21 s |
| 1 ms timer only | +8.75 s | +9.7 s | 5.0 s | +15 s |
| + `fmod_event.dll` | +8.3 s | +9.2 s | 4.4 s | +15 s |
| + `fmodex.dll` (the current code) | **+5.5 s** | **+6.5 s** | **1.7 s** | **+12.5 s** |

A later run of the current code without the sampler reached `main menu reached` after 6.3 s; the
original without it took 17.9 s the same evening.

The `fmod_event.dll` hook alone gave ~0.5 s and 2234 shortened waits: the main thread polled more
often, but slots did not free up any faster. It is worth keeping because the same loop is what the
main thread waits in once the file layer is fast (835 waits in the last run). That benefit was not
measured on its own.

## Not established

* **A long session.** The owner heard normal music and sound on the current code in the main menu and
  after loading a save, in a run of about 30 s that exited cleanly with no dump. The shortening has
  ended by then, so a longer session should change nothing, but none was played for this test.
* **A cold start or a slow disk.** All runs were on a warm file cache on the owner's machine. Real
  disk time stays, so the gain may be smaller there.
* **Why the Nexus player needed ~10 s.** Not measured. Their timer may already be finer, or they may
  have a faster main menu.
* **The gain on GOG.** `fmodex.dll` and `fmod_event.dll` are byte-identical (SHA-256) on the two
  builds and the code uses no game address. One run with both features was fine: main menu after 7.7 s,
  normal sound, right pictures in the Load menu. No GOG run without them was timed, so the gain there
  is not measured.

## What is left in the owner's startup

Measured on the last run:

* **Scripts, ~2 s of CPU.** Compiled from source at every start, on the main thread. A
  `compiledscripts.w2scripts` (2 MB) lies in `CookedPC`, but the exe contains neither that name nor
  `w2scripts`, in UTF-16 or ASCII, so this build does not seem to read it. Scripts are also what
  `src/scriptpatches` changes, so a cache of our own needs care.
* **Scripts, allocator.** About half of the script phase is the allocator's lock and statistics
  (`FUN_00AC8686`: `EnterCriticalSection` and four interlocked counters per allocation). Removing the
  counters might give ~0.5 s, but that allocator serves the whole game on two builds, so this was not
  tried.
* **Main menu, save list and GUI, ~3.5 s of CPU**, spread over the save list (`009A49B9`, already
  `fast_list`), `__VEC_memzero`, `FUN_00758470`, `FUN_0080DAB0`. No single target stands out.

## Save previews on the render thread

After the window the render thread ran at 100 % for ~2.5-3 s with the main thread waiting, in
`d3dx9_39.dll` under `0053FFF0` <- `FUN_009C47F8` <- `FUN_009DB889` (which drains a queue of images
loaded in the background). The call at `0054013C` is `D3DXCreateTextureFromFileInMemory` (import slot
`00CAD484`, 4 arguments, stdcall). A temporary log of each call on the owner's install:

```
165 calls, all on the render thread, all BMP 640x360 with one level
133 x A8R8G8B8 (921 656 bytes) and 32 x R8G8B8 (691 254 bytes), 17.0-17.8 ms each, 2 914 ms in total
```

165 is the number of `.sav` files, so these are the save previews: the cost is per save. By D3DX's
documentation the defaults of this call round the size up to a power of two (1024x512) and generate a
full mip chain, which fits the hot code; the finished texture was not read back.

`[saves] light_previews=1` (`hook_lpCreate`, `src/enginefix.c`) wraps that import, found by name. For
a BMP of exactly 640x360, read with `D3DXGetImageInfoFromFileInMemory`, it calls
`D3DXCreateTextureFromFileInMemoryEx` with `D3DX_DEFAULT_NONPOW2` for both sizes, 1 level, format from
the file, the managed pool that the default call uses, and no filters. Every other image, and any
failure of the Ex call, goes to the original. Off switch: `light_previews=0`.

Seen by the owner on DXVK: 150 previews in 259 ms, **~1.7 ms each**; the last one is done at +9.0 s
instead of +11.6 s. The pictures look right in the Load menu, and a save loaded and the game exited
cleanly. Log: `saves: light previews on (640x360 BMP at its own size, one level)` and `saves: first
preview made at its own size`. `[diagnostics] savelist_trace=1` adds memory per 50 previews.

Not established: the memory saved. By the numbers a preview goes from ~2.8 MB to ~0.9 MB of texture,
but no run compared the two, and how much of that sits in the 32-bit address space depends on the
renderer. Also not tried: native Direct3D 9, a low resolution where the picture is drawn much smaller
than 640x360 (no mip levels to fall back on), and a new quicksave picture.

On GOG the first preview was made when the Load menu opened (+14.1 s, the menu having been reached at
+7.7 s), not during the start as on Steam: its list keeps no previews at game start (`save-bloat.md`).
So on GOG `light_previews` shortens opening that menu, not the start.

## Tool

`tools/bootprofile/bootprofile.c` samples a process from outside: wait for a new `witcher2.exe`,
suspend the main thread and any thread that used CPU every ~10 ms, read EIP and scan the stack for
return addresses that follow a call, write everything to a text file. There is no debugger and no
change to the game or the DLL. `tools/bootprofile/report.py --ghidra` gives CPU per second, a timeline,
self and inclusive times, and `--children <addr>` splits a function by call site. It was checked first
on a test program with a known call chain and known CPU and sleep phases.
