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

The game closed before the main menu was drawn, so the player sees no menu. Not run on GOG,
where the exit flag setter and ExitGame take ECX: that is the part to check there.
