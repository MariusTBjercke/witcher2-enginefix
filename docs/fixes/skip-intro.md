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
  stop out of `009B581C`'s ten ticks, which run in one go.
* Disarmed when `+0xAC` (done) is set, or after 60 s. Other movies (flashbacks, extras) use the
  same object and tick but are never armed.

The csv (vanilla or a mod's) is read as usual; no file is changed. Each startup movie may show
for about 0.3 s before it is stopped.

Log: `startup: skip_intro=1`, `startup: movie list and tick hooked after N ms`, `startup: movie
list built (K entries), skip armed`, one `startup: stopping startup movie i of K` per movie, then
`startup: K startup movie(s) stopped, main menu reached`. Any fingerprint mismatch logs and
leaves the game unchanged.

## In-game test

Start the game: the main menu appears without the CD PROJEKT RED logo (or, without the intro
mod, the bumper and intro movie) and without a black wait; the log shows the three lines above.

## Confirmed in game, 2026-10-31

Owner (with the *Intro logo removal* mod's csv, one entry): the game starts at the main menu;
a short black moment remains before it ("short enough to live with"). That black time is the
video panel loading plus the 300 ms before the stop; lowering it further was not attempted.
