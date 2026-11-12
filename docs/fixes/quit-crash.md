# Crash while quitting with an area loaded

Found 2026-11-04 in the owner's crash dumps: three dumps that day (00:21, 08:54, 10:03), all at
the same place. The 10:03 one followed the pause menu's Quit (`pause_quit`, see
`docs/fixes/menu-buttons.md`; the owner is fairly sure, and the menu movie had just loaded with
the area still in); the earlier two are most likely the same. The game was closing anyway, so
nothing showed on screen, but
`crash_dump=1` (default) wrote a 1.2 MB dump and a `crash:` block into the log every time.

## Cause

```text
crash: unhandled exception 0xC0000005 at 0x00998724 (witcher2.EXE+0x598724), thread <main>
crash: access violation reading 0x00000008
crash: eax=00000000 ...
```

`00998724` is in `FUN_009986bd`, the script native `FactsAdd` (registered in `FUN_00998ae9`;
the name is UTF-16 at `00DC049C`). It reads its four arguments (fact name, value 1, valid for
-1, time) and takes the default of the time argument from the running world:

```
00998719  A1 50 49 40 01        mov eax,[01404950]       ; the game
0099871E  8B 80 90 00 00 00     mov eax,[eax+90h]        ; NULL during shutdown
00998724  8B 40 08              mov eax,[eax+8]          ; <- fault, read of 0x8
00998727  89 45 F8              mov [ebp-8],eax
```

The stack runs from an interaction component being switched off (`FUN_00ad84cb`, the code with
"Interaction '%s' was not activated before by entity '%s'") through the script event call
`FUN_005167ea` into a script's `OnInteractionDeactivated` handler that ends in `FactsAdd`.

Why only on Quit: the pause menu's Quit calls `QuitApplication()` (native `009A25AE` ->
`00AA5F3A`), which only sets the engine's exit flag (`[0160C6DC]+0x20 = 1`). The engine then
shuts down with the area still loaded, and the world pointer is gone before the entities have
had their last events. The game's own "Exit current game" goes through `theGame.ExitGame()`
(`00AA8E6C` -> `00AA8945`): it sets a flag on the game and raises `RequestGameEnd`, which ends
the session in order. The main menu's own Quit runs with no area loaded. The same shutdown
probably follows Alt+F4 in an area; not tested.

The risk was noted when Quit was added (W1 crashed the same way), and the check then was the
Windows Event Log. The game's own top-level exception filter swallows the crash, so the Event
Log never showed it; the crash capture (`docs/fixes/crash-capture.md`) did.

## Fix

`[stability] quit_guard=1` (default). The nine bytes at `0099871E` (checked together with the
three after them) become `call facts_time_stub` and four NOPs. The stub does what the two loads
did when the game and its `+0x90` pointer are there, and returns 0 when either is NULL, so the
fact is recorded with time 0 instead of the game crashing. Only EAX and the flags change; the
code after it reads no flags. Written behind the patch barrier (`patch_barrier.h`) from the
worker thread, once the code is unpacked.

A fact recorded during shutdown is never saved, so the time does not matter. During play the
pointer is set and nothing changes.

At exit the log says how often the stub saw no world, before `unloaded`:

```text
stability: quit guard installed (FactsAdd time default at 0099871E)
...
stability: quit guard: FactsAdd ran without a world 1 time(s), time 0 used
unloaded
```

A clean exit ends with `unloaded`; the crashing ones did not.

Not done: a later step of the same shutdown could still fault somewhere else. Nine other places
read `[[01404950]+0x90]+8` without a NULL check (among them `004BA18B`, `009613DF`, `009B3E76`,
`00AE1BD2`, `00B35B57`); whether any of them runs during this shutdown was not checked. The
in-game run below reached `unloaded`, so none did there. The fallback is the one
`menu-buttons.md` already names: Quit goes to the main menu first and quits from there.

### GOG 3.5.0.26g

FactsAdd is `FUN_0050b044` (registered with the same five other facts natives in `FUN_0050b0ff`;
name `L"FactsAdd"` at `00D441D4`). The site is `0050B0A9`, after `mov eax,[01475740]` (GOG's game
global): `8B 80 90 00 00 00 8B 40 08 89 45 FC`, the same two instructions with the same offsets,
storing to `[ebp-4]` instead of `[ebp-8]`, so the 12-byte check is per build
(`quit_site_bytes()`). The code after the 9 replaced bytes reads no flags; no relocation falls
inside the checked bytes. Since the guard is in on GOG, `pause_quit` is on there too.

## Turning it off

`[stability] quit_guard=0`.

## Verification

Static: the bytes at `0099871E` match the unpacked Steam exe, and that byte pattern occurs once in
the file. Build clean.

In game (2026-11-04, owner): same save, storage chest opened and closed, Esc, Quit, Yes. The
game closed, visibly faster than before (no dump to write and no hand-off to Steam's crash
handler), and no dump was written. Log:

```text
10:16:20.312 stability: quit guard installed (FactsAdd time default at 0099871E)
10:18:11.329 stability: quit guard: FactsAdd ran without a world 2 time(s), time 0 used
10:18:11.329 unloaded
```

Both `FactsAdd` calls that would have faulted were caught, and the shutdown finished.

## The next fault on the same way out: a cutscene actor (2026-11-06)

What "Not done" above warned about. Three dumps on Steam, each after the pause menu's Quit with a
prologue save loaded in `all\Dream` (the interrogation), also with `interiors=0` and every graphics
setting at Vanilla:

```text
crash: unhandled exception 0xC0000005 at 0x00A517CC (witcher2.EXE+0x6517CC), thread <main>
crash: access violation reading 0x00000038
crash: eax=00000000 ... ecx=01614CF0
```

`00A517CC` is the first instruction of `FUN_00a517cc` (`mov edx,[eax+38h]`): it searches an
array at `+0x34`/`+0x38` of the object in EAX for an element whose `+0x30` is the CName in ECX.
Its caller `FUN_00a47fdc` walks a cutscene's actors and passes `entity+0x130`, which is already
NULL ("Actor '%s' has been stolen from cutscene '%s'" is the game's message when the search finds
nothing; a NULL object it does not expect; `FUN_00a48226` next to it does check `+0x130`). The
stack runs from the engine's shutdown (`00AD9BE9`) through the world holder's destructor
`FUN_00958dd5` -> `FUN_00a8d58a` -> `FUN_00a8d0ed`, which stops the running cutscenes. Same cause
as FactsAdd: the engine tears the world down with the session still running.

### Fix: `[stability] clean_quit` (default 1)

Instead of a guard per fault, Quit with an area loaded now does what the game's own "Exit current
game" does first. The QuitApplication native's call to the exit flag is redirected to
`quit_stub`; with a world at `game+0x40` it calls ExitGame's body (sets `game+0x4E`, raises
`RequestGameEnd`), which ends the session in order and opens the main menu. Once the world has
been gone for 1.5 s, the exit flag is set from the next frame on the game thread (`ko_frame`, the
viewport tick; the worker if that tick is not hooked), the state the main menu's own Quit runs in.
If the session has not ended after 30 s the flag is set anyway. With no area loaded nothing
changes. `quit_guard` stays as a second line.

| | Steam 3.5.0.26 | GOG 3.5.0.26g |
|---|---|---|
| QuitApplication native | `009A25AE`: `55 8B EC 8B 45 08 FF 40 18`, call at `009A25B7` | `00504E78`: the same 9 bytes, `8B 0D` + engine, call at `00504E87` (registered in `00508C2D`, `"QuitApplication"` at `00D43C20`) |
| Exit flag setter | `00AA5F3A`: `A1 <engine> 80 78 20 00 75 04 C6 40 20 01 C3` | `00AFA032`: `80 79 20 00 75 04 C6 41 20 01 C3`, ECX = engine |
| Engine object | `[0160C6DC]`, exit flag `+0x20` | `[0167D4CC]` |
| ExitGame body | `00AA8945`: `55 8B EC 83 EC 14 53 56 57 33 DB 8B F0`, EAX = game (native `00AA8E6C`) | `00AFC92C`: `55 8B EC 83 EC 10 56 57 33 FF 8B F1`, ECX = game (native `00AFCE86`) |

`cq_call_exit` passes the game in both EAX and ECX, so one stub serves both builds. The flag
setter is matched once in each exe; the native's prologue is common on Steam, so the call is only
patched when its rel32 points at the setter. Off: `[stability] clean_quit=0`.

Log on a quit from an area:

```text
quit: QuitApplication hooked - with an area loaded it ends the game session first (clean_quit)
quit: an area is loaded - ending the game session first
quit: the game session ended N ms after Quit
quit: quitting from the main menu, M ms after Quit
unloaded
```

In game (Steam, owner, 2026-11-06), Esc, Quit, Yes each time:

| Save | Session ended | Exit flag set | End of the log |
|---|---|---|---|
| Prologue interrogation (`all\Dream`), crashed three times before | 1 015 ms | 2 578 ms | `ExitProcess(0)` on the main thread, `unloaded`, no dump |
| Flotsam, 18 minutes of play | 1 078 ms | 2 656 ms | the same |
| GOG, Flotsam after four loads | 1 093 ms | 2 609 ms | `CorExitProcess(0)` on the main thread (GOG's usual end, see `gog-build.md`), no dump |

On Steam the game closed before the main menu was drawn; on GOG the owner saw the menu for a
moment. The GOG run confirms the ECX convention of its exit flag setter and ExitGame.

## A loading-screen movie still alive at the GUI shutdown (2026-11-11, open)

One dump on Steam (DXVK), after the pause menu's Quit with `clean_quit` on. The session ended
and the exit flag was set as usual (`the game session ended 1062 ms after Quit`, `quitting from
the main menu, 2625 ms after Quit`); a second later:

```text
crash: access violation executing 0x4906E000
crash: eax=4906E000 ebx=00002000 ecx=476FF018 edx=3BF13C10 esi=473AC920 edi=3BF13C80
```

Stack: `00AC8C3B` -> `00AA62E6` -> the engine's GUI shutdown `FUN_00a7a475` (`DAT_01404bcc`) ->
`FUN_006d1700` -> `FUN_006dfc30`, which destroys Scaleform's root memory heap (`DAT_013e7cb8`,
vtable `00D578C0` slot `0x74` = `FUN_006e0650`). That walks the child heaps still alive and
destroys each one the same way; the child's segment loop `FUN_0077f810` frees its segments one by
one through `FUN_0077e970(engine, segment)`, which first calls
`engine[0x89]->vtbl[2](heap, size)` - the heap's limit handler, `OnFreeSegment`.

What the dump shows (read offline with `cdb -z`; the minidump holds 1 KB around each register):

* The child heap is `EDX = 3BF13C10`, named `MovieView "ls_akt2_vergen.swf"` - the Act 2 Vergen
  loading screen's movie. Its refcount is 1 and its auto-release pointer (`heap+0x18`) is
  `476FF000`.
* `FUN_006ed3d0` is Scaleform's CreateMemoryContext: it creates the `MovieView "<file>"` heap
  (flags 3, limit `0x20000`, as in the dump), allocates the 0x30-byte memory context *inside that
  heap*, sets the heap's limit handler (heap vtable slot `0x14` = `FUN_006dfd10`, which writes
  `engine+0x224`) to `context+0x18` (handler vtable `00D5AFF8`: `OnExceedLimit` `00728030`,
  `OnFreeSegment` `00725CB0`), and makes the context the heap's auto-release pointer
  (`FUN_006cfb40`, `heap+0x18`). The string manager (`FUN_006e9a40`, vtable `00D5AE84`) and the
  GC object hang off it. So `476FF000` is the memory context and `ECX = 476FF018` its handler.
* The context's destructor `FUN_006ed0d0` clears the handler first (`heap->SetLimitHandler(0)`
  at `006ED0D3..006ED0E3`). The handler is still set, so the context was never destroyed: the
  loading-screen movie was still alive when the engine shut the GUI down.
* The context's own bytes no longer hold it: `+0x00/+0x04` read `40DEC000`, `+0x18` (the
  handler's vtable) reads `476C0000`, `+0x08` (its heap) is 0. The string manager right after it
  (`476FF030`, vtable `00D5AE84`, heap `3BF13C10`) is intact. The segment being freed at the crash
  (`473AC920`: `476FD000`, `0x2000`) is the last one in the list (both links point at the list
  head `engine+0x1AC`), so the segment holding the context had already been freed in an earlier
  pass of the same loop, and the page allocator's bookkeeping now sits where the handler's vtable
  was. Calling `OnFreeSegment` for the next segment jumps through it.

So the crash is Scaleform's shutdown destroying a movie heap whose limit handler lives inside that
heap; it only happens when a movie view is still alive at the GUI shutdown, and only when the
segment holding its memory context is not the last one freed (which is why a live movie does not
always crash).

Not proven: why the loading screen was still alive. Two candidates, both consistent with the log:

1. The loading screen of the in-game load at 12:00:29 (Load menu -> save; the `thumbnail: menu
   reloads` lines at 12:00:27 and 12:03:58 come from that Load menu having been opened in the
   session, not from a save made in it) was never released. The session before it (same save,
   same Quit, clean exit) loaded only once, from the main menu, and never opened the Load menu.
2. Ending the session shows a loading screen, and `clean_quit` sets the exit flag 1.5 s after the
   world is gone while it is still up.

The mod's code does not create, hold or release Scaleform movies: the image registry hook and
`light_previews` act on the Load menu's save previews (a different movie and heap), and
`clean_quit` sets the same exit flag the main menu's own Quit sets. Candidate 2 would still make
`clean_quit`'s timing the trigger.

Diagnostic (0.12.1): the `quit:` line that sets the exit flag now says whether the render thread
holds a loading screen (`+0x268` movie, `+0x26C` screen object, see `load-times.md`), e.g.
`quit: quitting from the main menu, 2625 ms after Quit, no loading screen up`; the main menu's own
Quit logs `quit: no area loaded - quitting at once, ...`. `LOADING SCREEN UP` there points at
candidate 2, and the fix would be to wait in `cq_poll` until it is gone (still bounded by
`CQ_GIVEUP_MS`). `no loading screen up` followed by the same crash points at candidate 1, a game
leak that a later quit cannot avoid.

First run with the diagnostic (owner, 2026-11-11 13:35-13:39, same setup,
`docs/validation/flotsam-regression-2026-11-11.log`): three area loads, two of them from the
in-game Load menu (Vergen, then Flotsam twice), then the pause menu's Quit with the same timing
as the crash run (`the game session ended 1047 ms after Quit`, `quitting from the main menu,
2609 ms after Quit, no loading screen up`) and a clean exit one second later (`ExitProcess(0)`,
`unloaded`, no dump). So an in-game load before the quit does not crash every time, and in this
run no loading screen was up when the flag was set. It does not say which candidate applied in
the crash run: the crash did not come back. Two differences from that run, neither shown to
matter: its last load was a Vergen save (the movie in the dump is Vergen's loading screen) and
the menu reloaded `QuickSave_640x360.bmp` 66 ms after the exit flag; here the last two loads
were Flotsam saves and no picture was reloaded after the flag.

GOG the same day (`docs/validation/gog-0.12.1-2026-11-11.log`): the session ended 1 063 ms
after Quit; the owner saw the main menu and pressed its Quit 1 470 ms later, 30 ms before the
1.5 s wait was over (`quit: no area loaded - quitting at once, no loading screen up`), and the
game closed normally. On GOG the menu is drawn during that wait, on Steam it is not.

To reproduce: load a save, open the Load menu in game and load a save (one from the same area is
enough), play a little, Esc, Quit. With the segment-order condition it may take a few tries; the
crash is silent (the game closes) and leaves a dump plus a `crash:` block in the log.

### How a loading screen ends, and `[stability] loading_screen_cleanup` (2026-11-12; seen working in game once, Steam)

Two more sessions on 2026-11-11 ended with `quit: ... LOADING SCREEN UP (movie 00000000, screen
0C210768)`, one of them with this crash (`docs/validation/preload-areas-v1-2026-11-11.log`; both
had an experimental extra layer load right after the area load, `interior-preload.md`,
"`[loading] preload_areas`"). In both the object had been there since the load, minutes before
the Quit: `release_kept` logged `a loading screen is up` 28-36 s after it. So candidate 1 is
real: a loading screen can end and leave its movie alive. Static analysis, Steam 3.5.0.26:

| What | Where | Contract |
|---|---|---|
| Show a loading screen | game vtable `+0x108` = `00AA8B61` (`CWitcherGame` vtable `00EC6F74`) | thiscall (reason, String* screen); posts the render command below with the viewport `game+0x44` and the screen's name on every call; pauses the game for `LoadingScreen` when the count was 0; `InterlockedIncrement(game+0x68)` |
| End it | `+0x10C` = `009B49CF` -> `00AA8984` (thiscall, char force) | decrements `game+0x68` (force: sets it to 0); only when it reaches 0 posts the same command with viewport 0 and an empty name |
| The command | vtable `00E7C054`, ctor `00AA25A0` (ESI = memory, stdcall (viewport, String*)), execute slot `00E7C060` -> `009DC408` -> `009D1C46` (EAX = viewport, ECX = name) | render thread `+0x268` = viewport. With a viewport: a non-empty name is stored at `+0x46`; if the stored name is not empty the screen object at `+0x26C` is destroyed and a new one built from it (`FUN_009c3271`, class vtable `00EC6A20`, a Scaleform movie at object `+0x08`). With viewport 0: object slot 5 (`009C2F6C`) invokes the movie's `FadeOut`, `+0x274` = 3; the object stays |
| Freeing the object | `FUN_009d1a0e` (called at `00B13883` in the frame render `FUN_00b137eb`) and `FUN_009d1d61` (the loading loop) | destroy it and clear `+0x26C` when its "shown" byte `+0x10` is 0. Only the movie clears that byte: `Init` (slot 3, `009C32B2`) installs an fscommand handler (`00EC6A8C`) that calls slot 4 (`009C2F84`: `mov byte [ecx+0x10],0`) |
| The loop | `FUN_009d1e96` | calls `FUN_009d1d61` (advance, at most 30 times a second) while `+0x268` is set and no command waits, and before every command while the object is shown |
| The video loading screen | command vtable `00E7C068`, ctor `00AA1D2F` (EAX = memory, stdcall (viewport, movie)), execute `009DC3FC` -> `009D1B9F` | sets `+0x268`; with no viewport or no movie it destroys the object at `+0x26C` and clears the pointer; else it builds a video screen (class `00EC6A5C`). Only `009B56B0` (game vtable `+0xA4`, `.usm`) builds this command |

(An earlier note here and in `interior-preload.md` took `009B56B0` for vtable `+0x108`; it is slot
41, `+0xA4`.)

So a nested "show" rebuilds the movie, the last "end" asks the movie to fade out, and the object
only goes when the movie answers. A movie that never answers stays: `+0x268` is 0, `+0x26C` still
holds the object with "shown" 1, and the render thread advances it 30 times a second for the rest
of the session. At Quit it is the movie the GUI shutdown trips over. **Not proven: why a movie
does not answer.** One way that fits the code is a "show" shortly before the last "end" (the new
movie is asked to fade out before it has run a frame); no such call was found in the two
sessions' paths.

The fix does not depend on the cause:

* Once the game has no loading screen up (`game+0x68` is 0, `+0x268` is 0, no switch in progress)
  and the same object has been at `+0x26C` for 8 s, the DLL posts the video loading screen's
  command with no viewport and no movie from the game thread (`lscr_frame`, viewport tick):
  `FUN_00aa2685(0x10)` for the queue memory, the constructor at once, vtable `+4` to commit, as the
  game does at `009B57DC`. The render thread then destroys the object the way every new loading
  screen destroys the one before it. Log: `loading: a loading-screen object is still alive N s
  after its loading screen ended (object ..., shown 1) - asked the render thread to free it`, then
  `loading: the left-over loading-screen object is gone`.
* At Quit, when the exit flag is due (`cq_poll`): a loading screen that is still up gets up to 3 s
  to end; one that ended and left its object is freed the same way first. Log: `quit: a loading
  screen is still up|alive ... - waiting for it before the game closes`.
* Passive: the command's execute slot is wrapped, so every show and end is in the log with the
  object before and after (`loading: screen shown|ended on the render thread ('<name>'), screen
  object A -> B`). That is the evidence for the open question above.
* GOG 3.5.0.26g: the same code (count at `game+0x68`, show `00AFCB72`, end `00AFC972`); queue
  memory `00AF662F`, constructor `00AF5D05` (memory in ECX), command slot `00F1F098` -> `00A2DA11`.
  Seen in game on 2026-11-12: both loads left the object there too, it was freed 8 s after each
  (`... is gone (499 ms after asking)`), and the Quit logged `no loading screen up`
  (`docs/validation/gog-area-preload-2026-11-12.log`).
* Off: `[stability] loading_screen_cleanup=0`.

Seen in game (owner, 2026-11-12, Steam, DXVK, `docs/validation/preload-areas-v3-2026-11-12.log`):
two loads of an Act 2 save from the main menu. Both left the object, the first one a plain load
with nothing added by the mod's area preload, so the extra load of 2026-11-11 was not the cause:

```text
13:52:43.044 loading: screen shown on the render thread (''), screen object 00000000 -> 00000000
13:52:43.564 loading: screen shown on the render thread ('ls_akt2_vergen.swf'), screen object 00000000 -> 0C210768
13:52:49.334 loading: screen ended on the render thread (''), screen object 0C210768 -> 0C210768
13:53:05.462 loading: a loading-screen object is still alive 16 s after its loading screen ended (object 0C210768, shown 1) - asked the render thread to free it
...
13:53:17.409 loading: a loading-screen object is still alive 8 s after its loading screen ended (object 0D1E1E78, shown 1) - asked the render thread to free it
13:53:17.919 loading: the left-over loading-screen object is gone (499 ms after asking)
```

The second "show" of each load comes 0.5 s after the first and builds the movie; the "end" comes
3.5-5.8 s later, so the movie had run for seconds when it was asked to fade out, and the late
"show" guess above does not fit. The movie simply had not answered 8 s later. Whether it would
have answered later is not known: on 2026-11-11 one session's object was gone three minutes after
the load, two others' were still there after four. The Quit after it logged `no loading screen
up` and the game closed cleanly.
