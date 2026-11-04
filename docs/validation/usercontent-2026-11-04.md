# Script patches with a user content package, 2026-11-04

Steam Enhanced Edition 3.5.0.26 with Project Mersey. Test package made by
`tools\usercontent-test.ps1 -Game <game>`: `<Documents>\Witcher 2\UserContent\w2ef_test\` with an
empty `pack0.dzip` and a `scripts.dzip` holding the install's own `guiutils.ws` and `player.ws`
(one comment line added), mounted by `<Documents>\Witcher 2\Config\UserContent.ini`
(`[Packages]` / `Mount=w2ef_test`, ASCII, CRLF). `[diagnostics] file_trace=1` for both runs.

## Run 1: the build from before the change (control)

The game stopped at start:

```text
Script compilation error
There were errors compiling scripts. Unable to run game.
Maybe some selected user packages are incompatible?

game\player\states\extendedmovable.ws[91]: 'W2EF_UseMedallion' is not a member of '#CPlayer'
```

So the game compiled the package's `player.ws` over the patched one in `base_scripts.dzip`, and
the medallion's call across files failed exactly as predicted. The file trace shows the package
being opened right after the second open of `base_scripts.dzip`, with the doubled backslash the
game's path has:

```text
t=+953ms  CreateFileW "<Documents>\Witcher 2\\UserContent\w2ef_test\pack0.dzip" ... -> ok size=32
script patches: CreateFileW "<game>\CookedPC\base_scripts.dzip" -> cached copy
t=+1062ms CreateFileW "<Documents>\Witcher 2\\UserContent\w2ef_test\scripts.dzip" ... -> ok size=296947
```

## Run 2: the new build (`OV_CACHE_LAYOUT` 9)

The game started. Owner: the read status shows in the inventory and the storage chest, and the
medallion works on the move.

```text
script overlay: user content 'w2ef_test': mounted, has scripts.dzip
script overlay: game\gui\guiutils.ws: the game compiles the copy in user content 'w2ef_test' - patching that one
script overlay: game\player\player.ws: the game compiles the copy in user content 'w2ef_test' - patching that one
script overlay: book_marker=on
script overlay: medallion=on
script overlay: base_scripts.dzip: 6 entries replaced or added
script overlay: user0_scripts.dzip: 2 entries replaced or added
script overlay: user content 'w2ef_test': scripts.dzip served as user0_scripts.dzip
script overlay: cache: built in 109 ms (5 redirects)
script patches: CreateFileW "<game>\CookedPC\base_scripts.dzip" -> cached copy
script patches: CreateFileW "<Documents>\Witcher 2\\UserContent\w2ef_test\scripts.dzip" -> cached copy
```

The package was removed again with `-Remove`, and `file_trace` set back to 0.

## Run 3: after the review fixes (game's own ini reader rules, proxy-only outside the game)

Without a package: start, pause-menu Quit, then the launcher's Configurator opened and cancelled.

```text
10:44:00.886 script overlay: user content: no Config\UserContent.ini, none mounted
10:44:00.886 script patches: book_marker=on continue=on pause_quit=on window_mode=on medallion=on cache=reused
10:44:02.132 stability: quit guard installed (FactsAdd time default at 0099871E)
10:44:57.725 stability: quit guard: FactsAdd ran without a world 2 time(s), time 0 used
10:44:57.725 unloaded
```

No dump. After the Configurator ran, `witcher2_enginefix.log` was still this game session's log;
with the earlier build the Configurator had rotated and rewritten both logs (10:26 and 10:27).
The reader change was not run in game with a package; `sptest` covers it.

## Not related, found in the same run

Leaving the game (the menu movie had just loaded with the world still in; most likely the pause
menu's Quit, not yet confirmed), it crashed during shutdown (silent; the crash filter wrote a
dump): access violation at `00998724` reading `0x8`, inside the `FactsAdd` native
(`FUN_009986bd`), whose default time argument reads `[01404950]+0x90` after the world is gone.
The same address is in the dumps of 2026-11-04 00:21 and 08:54, both from the build before this
change and without a package. Fixed by `[stability] quit_guard` and checked the same day; see
`docs/fixes/quit-crash.md`.
