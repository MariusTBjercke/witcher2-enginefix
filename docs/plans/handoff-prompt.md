# Handoff: script patches applied by the DLL

## Goal

Ship 0.6.0 of `witcher2-enginefix` with the optional script patches (BookMarker, Continue,
PauseQuit) applied by `bin\dinput8.dll` itself: no `.bat`/`.ps1` in the release, no game file
written, uninstall = delete the DLL. Work Tasks 7-9 of
`docs/plans/2026-10-31-dll-script-patches.md` on branch `dll-script-patches`; `main` holds the
released 0.5.1 and stays untouched until Task 7's in-game check passes.

## Source of truth

| Path | Purpose |
|---|---|
| `docs/plans/2026-10-31-dll-script-patches.md` | The plan. Tasks 1-6 are `[x]`, 7-9 open. |
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
- `cmd /c build.bat` -> `Built build\dinput8.dll` (the `sp_*` code is not linked into the DLL
  yet; the script-patch RCDATA is already embedded).
- `python -I tools\patchtest\golden.py "<game>" "<scratch>"` -> `GOLDEN OK`: 460
  `base_scripts.dzip` entries, 152 read-book icons and both movies byte-identical to the
  reference, and the second build reuses the cache without touching a file. Mersey + HD UI
  inputs only (the old patcher's backup); no vanilla archive was at hand.
- Overlay build on this machine: ~110 ms with all three features, 16 ms when the cache is
  reused (`build\sptest.exe build "<game>" <cache>`). The cache is
  `<cache>\base_scripts.dzip`, `<cache>\ui_trade.swf`, `<cache>\ui_storage.swf`, `cache.key`.
- Game install is back at the released 0.5.1 state: original DLL and the owner's ini
  (`skip_intro=1`, no `file_trace` key). Backups in
  `build\file-trace-install-backup-20261031-180727\`.
- Branch commits: `6fc5c82` research, `f96527b` DZIP + harness, `eb6e135` SWF + inflate,
  `cd18318` DDS icons, `2bfa968` plan note, `8e56ada` AGENTS probe fix, `bdd1f6a` edit engine,
  then the overlay build.

Not implemented: Tasks 7-9. `src/enginefix.c` still knows nothing about the script patches.

## Next item

Task 7 of the plan, "Redirect in the DLL (owner start test)":

- Create `src/sp_hooks.{c,h}`; modify `src/enginefix.c` (ini `[scriptpatches] book_marker`,
  `continue`, `pause_quit`, all default 1), `dist-files/witcher2_enginefix.ini`, `build.bat`
  (link `sp_*.c` + `bcrypt.lib` ready for Task 8).
- `CreateFileW(path)`: full path via `GetFullPathNameW`, compared case-insensitively with the
  redirect table. First hit of a target, under a critical section, runs `sp_overlay_build`
  once (synchronously; the game waits, first start only) from the same early-install thread
  pattern Task 1 added; the cache goes to `<game>\bin\witcher2_enginefix_cache`.
- Redirect `FindFirstFileW`/`FindNextFileW` find data for `CookedPC\*.dzip` to the cached
  size (Task 1 Q3); `GetFileSize` follows the handle. Log one line:
  `script patches: book_marker=on continue=on pause_quit=on cache=built|reused`.
- **Owner start test** (hook rule) after the owner runs the old `Uninstall.bat` once, then an
  in-game check of all three features and each ini key set to 0; then delete the DLL and check
  the vanilla game. Tasks 7 Steps 2-3 and everything after them are hard stops.

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
git log --oneline -6; cmd /c tools\patchtest\build.bat; build\sptest.exe test --game "<game>"
python -I tools\patchtest\golden.py "<game>" "<scratch>"
```

Then read Task 7 of `docs/plans/2026-10-31-dll-script-patches.md`, "Hook design" in
`docs/fixes/dll-script-patches.md` and the existing early-install thread and `patch_iat` in
`src/enginefix.c` (`filetrace_thread`, `hook_CreateFileW`). Task 7 Steps 2-3 need the owner.

