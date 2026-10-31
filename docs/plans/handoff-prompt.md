# Handoff: script patches applied by the DLL

## Goal

Ship 0.6.0 of `witcher2-enginefix` with the optional script patches (BookMarker, Continue,
PauseQuit) applied by `bin\dinput8.dll` itself: no `.bat`/`.ps1` in the release, no game file
written, uninstall = delete the DLL. Code for Tasks 1-8 is committed on branch
`dll-script-patches` and start test 2 passed; what is left is Task 9 and the owner's bundled
in-game check (Task 7 Step 3, Task 8 Step 5) of `docs/plans/2026-10-31-dll-script-patches.md`.
`main` holds the released 0.5.1 and stays untouched until the in-game check passes.

## Source of truth

| Path | Purpose |
|---|---|
| `docs/plans/2026-10-31-dll-script-patches.md` | The plan. Tasks 1-6, Task 7 Steps 1, 2, 4 and Task 8 Steps 1-4 are `[x]`; Task 7 Step 3 and Task 8 Step 5 wait for the owner; Task 9 open. |
| `docs/plans/progress.json` | Item state, `verify` and the evidence trail (`dll-script-patches`, owner `owner`). |
| `docs/fixes/dll-script-patches.md` | Research (Q1-Q6, IAT slots) plus the design of the edit engine, overlay, redirect and migration. |
| `tools/script-patches/ScriptPatches.ps1` | Byte-for-byte reference until Task 9 deletes it. |
| `src/scriptpatches/`, `src/scriptpatches.rc`, `src/sp_script_ids.h` | Patch data (14-row `edits.txt` + `.ws` fragments) and its RCDATA numeric IDs. |
| `src/sp_*.{c,h}`, `src/third_party/tinf/` | DZIP (buffer + file-backed), SWF, DDS, script edits, overlay build, redirects. |
| `tools/patchtest/golden.py` | Golden test: reference `ScriptPatches.ps1` vs `build\sptest.exe build` on two scratch layouts. |
| `AGENTS.md` | Paths, build/install, incident rules. |

## Current state

Works (verified 2026-10-31 on this machine; commits `bdd1f6a`, `873d337`, `ff69149`):

- `cmd /c build.bat` -> `Built build\dinput8.dll` (only the pre-existing `LNK4104` warnings).
- `cmd /c tools\patchtest\build.bat` -> `Built build\sptest.exe`, no warnings.
- `build\sptest.exe test --game "<game>"` -> **22 tests, 0 failures**.
- `python -I tools\patchtest\golden.py "<game>" "<scratch>"` -> **GOLDEN OK**: 460
  `base_scripts.dzip` entries, 152 read-book icons and both movies byte-identical to the
  reference; the second build logs `cache: reused` and changes no file. Inputs: the old
  patcher's backup (Project Mersey + HD UI); no vanilla archive at hand.
- Task 7 offline: a local hooks harness on the golden scratch layout proved the build trigger,
  the redirected find size (4 508 633 = the cached archive), the three redirects, untouched
  unrelated paths, case-insensitive matching and cache reuse.

Start test 1 (2026-10-31, owner, 0.5 on-disk patches uninstalled with the old `Uninstall.bat`):
hooks in after 0 ms (3/3), overlay built in 125 ms, both `base_scripts.dzip` opens served from
the cache - then the game's *Script compilation error*, one `parse error, near 'DDS'` per read
icon. The engine compiles **every** entry of `base_scripts.dzip`, so the icons cannot live there.
Fixed: icons and an abetterui-only patched movie now go into a cached copy of the carrier
`dlc_finishers.dzip` (fallbacks in `k_carriers`, `src/sp_overlay.c`); `sp_hooks_find` covers
any redirected `CookedPC\*.dzip`; the cache key has `OV_CACHE_LAYOUT 2`; `golden.py` fails on any non-reference entry in `base_scripts.dzip`.
Offline after the fix: 22 tests, `GOLDEN OK` (460 entries, 152 icons in `dlc_finishers.dzip`).

Start test 2 (commit `d1dcb26`) passed: main menu, cache built in 109 ms, all three features
on; book markers, menu buttons and saving checked in game.

Task 8 (offline, commit below this handoff): `src/sp_migrate.{c,h}` restores 0.4/0.5 on-disk
patches once from the old backup; `hook_FindFirstFileW` calls `sp_hooks_prepare` before the
original call so restore and build happen before the game's `*.dzip` listing. `sptest`: 23
tests; `golden.py` also migrates the reference's install back - `GOLDEN OK`.

The install now has: clean `CookedPC` (0.5 patches restored by `Uninstall.bat`, backup folder
deleted), the d1dcb26 DLL and its cache. The Task 8 DLL is not installed yet: it changes the
FindFirstFileW hook body, so the bundled check is also its start test.

## Next item

Task 9 prep (release packaging without scripts; see the plan), then the owner's bundled
in-game check right before the release (hard stop; verify as in `progress.json`):

1. Task 7 Step 3 rest: storage chest ticks (`ui_storage.swf` has not been opened in game yet),
   each `[scriptpatches]` key `0` (that feature gone, the others intact), then delete
   `bin\dinput8.dll` for the vanilla check.
2. Task 8 Step 5: the owner keeps manual copies of `CookedPC\base_scripts.dzip` and the two
   movies, reinstalls 0.5's patches with the old `Install.bat` (the 0.5 `Witcher2EngineFix_Optional`
   folder is still in the install), starts with the new DLL. Expected log: `migrate: restored 3
   files, removed w2ef_read`, `migrate: old backup kept as ...backup.restored-by-dll`, then the
   normal `script patches: ... cache=built` line; the game works with the DLL patches.

A silent crash at startup means a wrong hook: delete the DLL, report, do not retry. No push or
release without the owner's go-ahead.

## Critical constraints

- Never write under `<game>`; Task 8's one-time restore is the only exception and runs from
  the DLL. Never touch `<game>\bin\witcher2.exe`.
- `ScriptPatches.ps1` stays the byte-for-byte reference until Task 9.
- Hook changes need an owner start test; a wrong signature kills the game silently
  (`docs/research.md` §6). No live software breakpoints.
- `Documents\Witcher 2\Config\User.ini` and `Rendering.ini` are read-only by design.
- No machine paths or Steam IDs in tracked files (`<game>`, `<scratch>`, `%USERPROFILE%`).
- Only third-party code: `src/third_party/tinf`, unaltered, license kept.
- English in code, docs, commits; Norwegian with the owner.

## Known risks and open questions

- The redirect only fires for paths recognised as absolute (or resolvable relative) and
  containing `CookedPC`; Task 1 saw absolute backslash paths. No `-> cached copy` line in the
  log means the path form differs - then log with `[diagnostics] file_trace=1` and compare.
- The find-data redirect matches the entry name of a redirected `CookedPC\*.dzip`. If a
  listing elsewhere ever carried that name, its size would come from the cache too.
- Branch B: icons only exist as entries of the redirected carrier archive; making them loose
  files in the cache would hide them (the engine never opens a loose file it did not see in its
  startup walk), and putting them into `base_scripts.dzip` stops the game (script compiler).
- An abetterui-only patched movie in the carrier relies on "last archive in name order wins"
  (`dlc_finishers` > `abetterui`); unverified in game, the owner's movies are loose.
- Cache key = DLL version + the three features + size/mtime of every `CookedPC\*.dzip` and
  loose movie. Bump `ENGINEFIX_VERSION` in Task 9; delete `bin\witcher2_enginefix_cache` when
  testing by hand.
- `<scratch>` for `golden.py` must be on the game's volume (hard links) and is wiped; the test
  runs a copy of `ScriptPatches.ps1`, so the repo never gets the patcher's `backup` folder.
- The harness is `build\sptest.exe`, not `patchtest.exe` (UAC installer detection would block
  unattended runs on `consent.exe`). Do not rename it.
- `ui_storage.swf` was never opened in the owner's trace; Task 7's in-game check covers it.

## First command

```powershell
git log --oneline -5; cmd /c build.bat; cmd /c tools\patchtest\build.bat
build\sptest.exe test --game "<game>"
```

Then hand the owner the start-test steps above, or read Task 7 Steps 2-3 of
`docs/plans/2026-10-31-dll-script-patches.md` and "Redirect in the DLL" in
`docs/fixes/dll-script-patches.md`.
