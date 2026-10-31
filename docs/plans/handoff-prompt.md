# Handoff: script patches applied by the DLL

## Goal

Ship 0.6.0 of `witcher2-enginefix` with the optional script patches (BookMarker, Continue,
PauseQuit) applied by `bin\dinput8.dll` itself: no `.bat`/`.ps1` in the release, no game file
written, uninstall = delete the DLL. Finish Task 7 Steps 2-3 (owner test) and then Tasks 8-9 of
`docs/plans/2026-10-31-dll-script-patches.md` on branch `dll-script-patches`; `main` holds the
released 0.5.1 and stays untouched until Task 7's in-game check passes.

## Source of truth

| Path | Purpose |
|---|---|
| `docs/plans/2026-10-31-dll-script-patches.md` | The plan. Tasks 1-6 and Task 7 Step 1 are `[x]`; Task 7 Steps 2-3 wait for the owner; 8-9 open. |
| `tools/script-patches/ScriptPatches.ps1` | Reference implementation; authoritative for every byte until Task 9 deletes it. |
| `docs/fixes/dll-script-patches.md` | Task 1 research: Q1-Q6 answers, IAT slots, what Task 7 must redirect. |
| `docs/validation/file-trace-2026-10-31.md` | Sanitized log evidence for that research. |
| `src/scriptpatches/`, `src/scriptpatches.rc`, `src/sp_script_ids.h` | The patch data (`edits.txt` + `.ws` fragments, `book_texts.ws` generated once) and its RCDATA IDs; embedded in the DLL and the harness. |
| `src/sp_*.{c,h}`, `src/third_party/tinf/` | DZIP (buffer and file-backed), SWF, DDS, the script edit engine and the overlay build; built into `build\sptest.exe` by `tools/patchtest/`. |
| `tools/patchtest/golden.py` | The golden test: reference `ScriptPatches.ps1` vs the overlay build on two scratch layouts. |
| `docs/plans/progress.json` | Item state and the evidence trail. |
| `AGENTS.md` | Paths, build/install, incident rules. |

## Current state

Works (verified 2026-10-31 on this machine):

- `cmd /c tools\patchtest\build.bat` -> `Built build\sptest.exe`, no warnings.
- `build\sptest.exe test --game "<game>"` -> 0 failures (22 tests); prints
  `23 archives, 31999 entries`, `12 archives, 612 entries extracted`,
  `23 archives, 612 entries compared with the file reader, 11 large archive(s)`,
  `4 SWFs checked`, `markers 2 1 6 6 2, 224019 bytes of scripts`.
- `cmd /c build.bat` -> `Built build\dinput8.dll`, now with `src\sp_*.c` + tinf linked in
  (only the pre-existing `LNK4104` export warnings).
- `python -I tools\patchtest\golden.py "<game>" "<scratch>"` -> `GOLDEN OK`: 460
  `base_scripts.dzip` entries, 152 read-book icons and both movies byte-identical to the
  reference, and the second build reuses the cache without touching a file. Mersey + HD UI
  inputs only (the old patcher's backup); no vanilla archive was at hand.
- Overlay build on this machine: ~110 ms with all three features, 16 ms when the cache is
  reused. The cache is `<cache>\base_scripts.dzip`, `<cache>\ui_trade.swf`,
  `<cache>\ui_storage.swf`, `cache.key`, `cache.status`.
- Task 7 Step 1 is implemented and verified offline: `src/sp_hooks.{c,h}` and the enginefix
  wiring install `CreateFileW`/`FindFirstFileW`/`FindNextFileW` from a 1 ms-poll early thread,
  build the overlay on the startup `<game>\CookedPC\*.dzip` listing (or the first target open),
  redirect the cached archive and the two loose movies, and report the cached find data for
  `base_scripts.dzip`. A local hooks harness on the golden scratch layout passed all checks
  (build trigger, find size 4 508 633 = cached archive, 3 redirects, unrelated paths untouched,
  case-insensitive, cache reuse). `[scriptpatches]` keys default 1 and are documented in
  `dist-files\witcher2_enginefix.ini`.
- Game install is back at the released 0.5.1 state: original DLL and the owner's ini
  (`skip_intro=1`, no `file_trace` key). Backups in
  `build\file-trace-install-backup-20261031-180727\`.
- Branch commits: `6fc5c82` research, `f96527b` DZIP + harness, `eb6e135` SWF + inflate,
  `cd18318` DDS icons, `2bfa968` plan note, `8e56ada` AGENTS probe fix, `bdd1f6a` edit engine,
  `873d337` overlay + golden, then the DLL redirect.

## Next item

Task 7 Steps 2-3, the owner start test and in-game check (hard stop). Give the owner this:

1. Close the game and the launcher.
2. Run `<game>\Witcher2EngineFix_Optional\ScriptPatches\Uninstall.bat` once. It restores the
   0.5 on-disk patches (the DLL refuses to patch an already marked archive; Task 8 automates
   this for users). Check that `CookedPC\base_scripts.dzip` is back to ~1.67 MB and the
   `CookedPC\globals\gui\icons\items\w2ef_read` folder is gone.
3. Copy `build\dinput8.dll` to `<game>\bin`. The installed ini may stay as it is (the new
   `[scriptpatches]` keys default to 1) or take `dist-files\witcher2_enginefix.ini`.
4. Start the game through Steam and press *Launch game*. Reaching the main menu is the start
   test; then the in-game check: inventory statuses/tooltip/refresh after reading, trade and
   storage ticks, loot window icon + `(Nothing new)`, container preview icon, Continue,
   pause-menu Quit. Then each ini key `0` (feature gone, others intact) and finally delete
   `bin\dinput8.dll` for the vanilla check.
5. Send `<game>\bin\witcher2_enginefix.log`. Expected: `scriptpatches: book_marker=1
   continue=1 pause_quit=1`, `script patches: hooks installed after N ms (3/3 slots)`,
   `script overlay: read icons: 152 books, 152 icons`, `script patches: book_marker=on
   continue=on pause_quit=on cache=built N ms`, one `script patches: CreateFileW
   "<game>\CookedPC\base_scripts.dzip" -> cached copy` and the same for `ui_trade.swf` and
   `ui_storage.swf`.

A silent crash at startup means a wrong hook and needs a rollback (delete the DLL); report it
instead of retrying. Steps 4 (commit) and Tasks 8-9 wait for this.

## Critical constraints

- Never write under `<game>`; Task 8's one-time restore is the only exception and runs from
  the DLL. Never touch `<game>\bin\witcher2.exe`.
- `ScriptPatches.ps1` stays the byte-for-byte reference until Task 9; Task 6's golden test decides.
- Every new IAT hook needs an owner start test; a wrong signature kills the game silently
  (`docs/research.md` §6). No live software breakpoints.
- `Documents\Witcher 2\Config\User.ini` and `Rendering.ini` are read-only by design.
- No machine paths or Steam IDs in tracked files (`<game>`, `<scratch>`, `%USERPROFILE%`).
- Third-party code: only `src/third_party/tinf`, unaltered, license kept.
- English in code, docs and commits; Norwegian with the owner.

## Known risks and open questions

- The golden test needs `<scratch>` on the game's volume (the 22 other dzips are hard links) and
  wipes `ref\`, `dll\`, `out\`, `backup\` and `ref-patch.ps1` under it. It runs a copy of
  `ScriptPatches.ps1`, so the repo never gets the patcher's `backup` folder.
- The find-data redirect matches the entry name `base_scripts.dzip` in the archive listing; the
  game has exactly one such file, so no directory bookkeeping is kept. If a second archive
  listing ever carried that name, its size would be reported from the cache too.
- The redirect only fires for paths the DLL recognises as absolute (or `CookedPC` paths it can
  resolve); Task 1 saw absolute backslash paths for every target. If the start test shows no
  `-> cached copy` line, log with `[diagnostics] file_trace=1` and compare the path form.
- The cache is keyed on the DLL version, the three features and the size/mtime of every
  `CookedPC\*.dzip` and loose movie: bump `ENGINEFIX_VERSION` (Task 9) to force a rebuild for
  users, and delete `bin\witcher2_enginefix_cache` when testing by hand.
- Branch B is what makes the read icons work: they only exist as entries of the redirected
  `base_scripts.dzip`, because the engine never opens a loose file it did not see in its
  startup walk. Do not turn the icons into loose files in the DLL cache.
- The harness is `build\sptest.exe`, not `patchtest.exe`: Windows' UAC installer detection
  elevates a manifest-less 32-bit exe whose name contains "patch", so an unattended run blocks on
  `consent.exe`. Do not rename it back.
- `dlc_finishers.dzip` has a 2-byte entry whose LZF stream carries a 16-byte literal run; the
  decoder clamps it (the reference's `Buffer.BlockCopy` would throw). Do not turn that into a failure.
- `ui_storage.swf` was not opened in the owner's trace (only `ui_trade.swf`); the same loader is
  assumed, and Task 7's in-game check covers the storage tick.
- Task 7's start test needs the on-disk 0.5 patches removed first: the owner runs the old
  `Uninstall.bat` once (Task 8 automates that for users).
- `tools\inputprobe\probe.exe` loads `System32\dinput8.dll` by bare name, so running it from
  `tools\inputprobe` proves nothing about the proxy - copy it into `<game>\bin`. Its F9 checks
  also need console focus.

## First command

```powershell
git log --oneline -6; cmd /c build.bat; cmd /c tools\patchtest\build.bat
build\sptest.exe test --game "<game>"
```

Then hand the owner the start-test steps above, or read Task 7 Steps 2-3 of
`docs/plans/2026-10-31-dll-script-patches.md` and "Redirect in the DLL" in
`docs/fixes/dll-script-patches.md`. After the owner reports back, Task 7 Step 4 is the commit
and Task 8 (migration) follows; it has its own owner check at Step 5.

