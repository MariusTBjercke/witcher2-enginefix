# Handoff: script patches applied by the DLL

## Goal

Ship 0.6.0 of `witcher2-enginefix` with the optional script patches (BookMarker, Continue,
PauseQuit) applied by `bin\dinput8.dll` itself: no `.bat`/`.ps1` in the release, no game file
written, uninstall = delete the DLL. All code is implemented and committed on branch
`dll-script-patches`; what is left is the owner's start test (Task 7 Steps 2-3), then Tasks 8-9
of `docs/plans/2026-10-31-dll-script-patches.md`. `main` holds the released 0.5.1 and stays
untouched until the in-game check passes.

## Source of truth

| Path | Purpose |
|---|---|
| `docs/plans/2026-10-31-dll-script-patches.md` | The plan. Tasks 1-6 and Task 7 Steps 1 and 4 are `[x]`; Task 7 Steps 2-3 wait for the owner; 8-9 open. |
| `docs/plans/progress.json` | Item state, `verify` and the evidence trail (`dll-script-patches`, owner `owner`). |
| `docs/fixes/dll-script-patches.md` | Research (Q1-Q6, IAT slots) plus the design of the edit engine, overlay and redirect. |
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

Not verified: the game itself has never run with this DLL. `bin\dinput8.dll` in the install is
still the released 0.5.1 build and `bin\witcher2_enginefix_cache` does not exist yet.

## Next item

Task 7 Steps 2-3, the owner start test and in-game check (hard stop; verify as in
`progress.json`). Give the owner exactly this:

1. Close the game and the launcher.
2. Run `<game>\Witcher2EngineFix_Optional\ScriptPatches\Uninstall.bat` once (restores the 0.5
   on-disk patches; the DLL refuses to patch a marked archive). Check `CookedPC\base_scripts.dzip`
   is back to ~1.67 MB and `CookedPC\globals\gui\icons\items\w2ef_read` is gone.
3. Copy `build\dinput8.dll` to `<game>\bin` (the existing ini is fine: `[scriptpatches]`
   defaults to 1, or copy `dist-files\witcher2_enginefix.ini`).
4. Start the game through Steam and press *Launch game*. Main menu = start test; then check
   BookMarker (inventory status/tooltip/refresh, trade + storage ticks, loot window, container
   preview), Continue, pause-Quit, then each ini key `0` (that feature gone, others intact),
   then delete the DLL for the vanilla check.
5. Send `<game>\bin\witcher2_enginefix.log`. Expected: `scriptpatches: book_marker=1
   continue=1 pause_quit=1`, `script patches: hooks installed after N ms (3/3 slots)`,
   `script overlay: read icons: 152 books, 152 icons`, `script patches: book_marker=on
   continue=on pause_quit=on cache=built N ms`, and one `script patches: CreateFileW
   "...\CookedPC\base_scripts.dzip" -> cached copy` (same for both movies).

A silent crash at startup means a wrong hook: delete the DLL, report, do not retry. After a
passing test, go to Task 8 (one-time restore of 0.5 on-disk patches, offline tests first); Task 9
is the release and needs the owner's go-ahead.

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
- The find-data redirect matches the entry name `base_scripts.dzip` (the game has one). If a
  listing ever carried that name for another file, its size would come from the cache too.
- Branch B: icons only exist as entries of the redirected `base_scripts.dzip`; making them
  loose files in the cache would hide them (the engine never opens a loose file it did not see
  in its startup walk).
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
