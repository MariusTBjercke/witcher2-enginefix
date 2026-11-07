# Skip the logo videos and the intro movie

Status: shipped in 0.4.0, `[startup] skip_intro=1` (off by default, like witcher-enginefix
QoL 5). Confirmed in game 2026-10-31. Target: Steam Enhanced Edition 3.5.0.26.

## The startup video list

Before the main menu the game plays the videos listed in `CookedPC\globals\video_start.csv`
(string `globals\video_start.csv` at `00DC37B0`; the table is in `pack0.dzip`, a loose file
overrides it). UTF-16 LE with BOM, `;`-separated, one column per release (`theGame` "Release";
Steam is EFIGS), one row per video:

```
EFIGS;GOG;ATARI;ROW;PL;RU;CZ;HU;JA;TW;DD;ATARI_DD;AU;NA;JP
bumpers_EU;bumpers_GOG;bumpers_NA;bumpers_EU;bumpers_PL;...;cf;...
intro;intro;intro;intro;intro;intro;intro;intro;nvidia;...
;;;;;;;;middleware;;;;;;middleware
...
```

* `009B581C` (game start, once) creates the startup player object (`[game + 0x2338]`) and calls
  `009A2190` with it in EAX. That function reads the "Release" column and appends every
  non-empty cell to the list at `+0x9C` (count `+0xA0`).
* `009A211D` (the object's tick): while no video plays (`+0xAD == 0`), it plays entry `[+0xB0]`
  through `009A2005` and increments `+0xB0` while `+0xB0 < count`; when `+0xB0` reaches a
  **non-zero** count it sets `+0xAC` and calls the engine's "videos done" (vtable `+0xA8` of
  `[0160D0FC]`), which goes on to the main menu.
* A video ends through `009A1F92` (tick, video finished) or `009A1FC6` (Esc, mouse buttons,
  pad buttons) -> `009A1F2E`: `StopVideo`, `+0xAD = 0`; with count 0 it also sets "done".
* `ui_video.swf` reports `MovieStopped` only on `NetStream.Play.Stop`, not on a missing file.

So an empty list never finishes on its own: neither branch of `009A211D` runs and the screen
stays black until a key "stops" a video. Tried first (ScriptPatches `SkipLogos`, a loose csv
with one row of empty cells): exactly that black wait until Esc (owner, 2026-10-30). It is also
why the *Intro logo removal* mod (Nexus) keeps one video, `cdpr`, in every column.

## First DLL attempt: index = count (crashed)

The build call was redirected so that, right after the list was built, `+0xB0 = count`: the
first tick would take the "all videos played" branch. In game (2026-10-30 23:54) the process
died silently a few seconds after DirectInput came up, before any window, no WER entry; the log
showed the hook installed but never the "skipped" line. That first tick runs inside
`009B581C`'s own ten ticks, so "movies done" (vtable `+0xA8`) ran before the GUI existed.
Normally it only runs after a movie was shown, which is also why Esc during the black wait
(csv attempt) worked.

## Change: press Esc automatically

The game's own skip path is used, at a time when a movie is really playing:

* `startvideo_thread` (started from `DllMain` when `skip_intro=1`) polls every millisecond until
  SteamStub has decrypted `.text` and checks: the call site `009B588D` (`8B C6 E8 FC C8 FE FF`,
  `mov eax,esi; call 009A2190`), the list builder's prologue, the index/count test in `009A211D`
  (`8B 86 B0 00 00 00 8B 8E A0 00 00 00 3B C1 72 1B`), the stop function `009A1F2E`
  (`55 8B EC 83 E4 F8 83 EC 10 53 33 DB 57 68 1C 38`) and the player vtable slot `00ECAF78`
  (vtable `00ECAF04` + `0x74`) holding `009A211D`.
* The build call goes to `startvideo_build_hook`: `009A2190` as usual, then the player object is
  remembered ("armed"). Nothing in the list changes.
* The vtable slot gets `startvideo_tick`: the original tick, then, for the armed player only:
  once a movie has been playing (`+0xAD`) for 300 ms of real time it calls `009A1F2E` with
  ESI = player, exactly what Esc does (`009A1FC6`). The game then plays the next entry, which is
  stopped the same way, and after the last one takes its own "done" branch. The 300 ms keep the
  stop out of `009B581C`'s ten ticks, which run in one go. (Replaced on 2026-11-07: see "The stop
  at 300 ms broke the next movie" below.)
* Disarmed when `+0xAC` (done) is set, or after 60 s. Other movies (flashbacks, extras) use the
  same object and tick but are never armed.

The csv (vanilla or a mod's) is read as usual; no file is changed. Each startup movie may show
for about 0.3 s before it is stopped.

Log: `startup: skip_intro=1`, `startup: movie list and tick hooked after N ms`, `startup: movie
list built (K entries), skip armed`, one `startup: stopping startup movie i of K` per movie, then
`startup: K startup movie(s) stopped, main menu reached`. Any fingerprint mismatch logs and
leaves the game unchanged.

## GOG 3.5.0.26g

Found from `globals\video_start.csv` (`00D43DAC`), `Release`, `StopVideo` and `MovieStopped`, and
the player's class `CVideoInstance` (vtable `00D43F3C`, the same layout). Same offsets (+0x9C list,
+0xA0 count, +0xAC done, +0xAD playing, +0xB0 index). Differences the DLL handles per build:

* The list builder `00509DF5` and the stop `00509D26` are thiscall: the player is in ECX, not
  EAX/ESI. `startvideo_build_hook_gog` and `startvideo_stop_gog` pass it there.
* The builder is called from a wrapper, `00509EEB` (thiscall(bool), `RET 4`; at game start from
  `004E8BD7` with 1). Its `call 00509DF5` at `00509EF7` is the only call to the builder and runs
  only then. The check is `74 05 E8 F9 FE FF FF` at call-2. The wrapper keeps the player in ESI
  and writes `[ESI+0xAC]` after the call, so the hook leaves ESI alone.
* The tick `0050A01E` has a 4-byte shorter prologue: its fingerprint is at +0x1B, not +0x1F.
* The stop's check ends in a push of `StopVideo`'s address, which is relocated; only its low 16
  bits (`68 18 3E`) are checked, as they survive 64 KB load steps.

The startup list's column (`Release`) was not checked against GOG's csv, which also has a GOG
column (`bumpers_GOG`); the log's `movie list built (N entries)` shows what GOG plays.

## In-game test

Start the game: the main menu appears without the CD PROJEKT RED logo (or, without the intro
mod, the bumper and intro movie) and without a black wait; the log shows the three lines above.

## Suspected of a black screen at the end of Act 1, then refuted (2026-11-07)

**Refuted the same day:** with the new skip below (no startup movie started at all) the black
screen came back (16:50). The skip change stays, as it is simpler and shows no logo, but it does
not fix the black screen. What follows is the record of why it was suspected.

Owner, Steam, DXVK: at the end of Act 1 (Roche path), after the last cutscene, the screen went
black and silent for good. Two dumps of the hang:

* The main thread waits in the game start `FUN_00a9eed2` ("Attaching entities...", a `Sleep(1)`
  loop) while `[game + 0x2338]` is set: a `CVideoInstance` with `+0xAD` = 1, made by
  `FUN_009b56b0` (the video player for `load_a1_a2_r.usm`). It goes only when `ui_video.swf`
  reports `MovieStopped` (`FUN_009b353c` clears the slot).
* The loading screen thread (`FUN_009d1e96`, `ls_akt1.swf` and a `CLoadingScreenVideo`) spins at
  full speed, as it always does while a loading screen shows; DXVK, the job threads and FMOD
  were idle; nothing waited on a lock.
* A dump of the same transition with the video playing had two game threads more (10 against 8):
  the stream's threads, which never started in the black runs.

Runs, all from the same save: 6 of 7 black with `skip_intro=1` (in various `[loading]` and
diagnostics settings, which turned out not to matter), 0 of 3 black without it (`enabled=0`
twice, `skip_intro=0` once).

`009A2005` (play) is only the Flash call `PlayVideo`, which opens a `NetStream` on the `.usm`;
`009A1F2E` (stop) calls `StopVideo`, which closes it. `ui_video.swf` also has `MovieStarted`
(on `NetStream.Play.Start`), but the game registers no handler for it. The startup movie and the
Act 2 movie use the same player slot and the same `ui_video.swf`. The DLL stopped the first
startup movie 300 ms after `+0xAD` was set, which can be before its stream has opened. The
likely result is that the stream is left half open, so no later movie in that session starts.
This is not proven in the code of the video library (Scaleform video over CRI), only by the runs
above.

### Change: never start a startup movie

The wrapped tick now runs before the game's own tick for the armed player: if nothing plays
yet, it sets the index `+0xB0` to the count and `+0xAD` to 1. The game's tick then only ticks the
panel and never calls `009A2005`. After `STARTVIDEO_MIN_MS` (outside `009B581C`'s ten ticks) it
calls `009A1F2E` as Esc would; `StopVideo` closes a stream that was never opened, which the empty
list test of 2026-10-30 already did in game (Esc in the black wait). The next tick takes the
game's own branch for "all played" (`+0xAC`, "movies done"); with an empty list `009A1F2E` sets
done itself. No movie shows at all now, not even for 0.3 s.

Log: `startup: K startup movie(s) marked as played, none started`, then
`startup: K startup movie(s) skipped without starting them, main menu reached`.

Seen in game (Steam, 2026-11-07, two starts): the main menu after the new skip, both log lines.
The Act 1 -> Act 2 movie then started once and stayed black once.

### What the black runs have in common instead

A dump of a black run with the new skip (16:51) against one where the movie played (16:08,
`enabled=0`): the video player's control block holds the relative path
`globals\gui\load_a1_a2_r.usm` in both. In the run that played, the dword at block+0x24 is 3,
there are two buffer pointers and a `...\CookedPC\movies\load_a1_a2_r.usm` path elsewhere in
memory (the file was opened). In the black run that dword is 0, the buffers are 0 and no full
path exists: the movie got its name and never started opening the file. Why is open. Players
report the same black screen at this point without Engine Fix (GOG forum, "black screen after
end of act 1 cut scene", 2014), so it may be the game's own race; how often it happens here
with and without the mod is not settled (with the mod 3 of 10 runs played, without it 2 of 2).

## Confirmed in game, 2026-10-31

Owner (with the *Intro logo removal* mod's csv, one entry): the game starts at the main menu;
a short black moment remains before it ("short enough to live with"). That black time is the
video panel loading plus the 300 ms before the stop; lowering it further was not attempted.
