# Handoff: script patches applied by the DLL

## Goal

Ship 0.6.0 of `witcher2-enginefix` with the optional script patches (BookMarker, Continue,
PauseQuit) applied by `bin\dinput8.dll` itself: no `.bat`/`.ps1` in the release, no game file
written, uninstall = delete the DLL. Work Tasks 6-9 of
`docs/plans/2026-10-31-dll-script-patches.md` on branch `dll-script-patches`; `main` holds the
released 0.5.1 and stays untouched until Task 7's in-game check passes.

## Source of truth

| Path | Purpose |
|---|---|
| `docs/plans/2026-10-31-dll-script-patches.md` | The plan. Tasks 1-5 are `[x]`, 6-9 open. |
| `tools/script-patches/ScriptPatches.ps1` | Reference implementation; authoritative for every byte until Task 9 deletes it. |
| `docs/fixes/dll-script-patches.md` | Task 1 research: Q1-Q6 answers, IAT slots, what Task 7 must redirect. |
| `docs/validation/file-trace-2026-10-31.md` | Sanitized log evidence for that research. |
| `src/scriptpatches/`, `src/scriptpatches.rc`, `src/sp_script_ids.h` | The patch data (`edits.txt` + `.ws` fragments, `book_texts.ws` generated once) and its RCDATA IDs; embedded in the DLL and the harness. |
| `src/sp_*.{c,h}`, `src/third_party/tinf/` | DZIP, SWF, DDS, the script edit engine and helpers; built into `build\sptest.exe` by `tools/patchtest/`. |
| `docs/plans/progress.json` | Item state and the evidence trail. |
| `AGENTS.md` | Paths, build/install, incident rules. |

## Current state

Works (verified 2026-10-31 on this machine):

- `cmd /c tools\patchtest\build.bat` -> `Built build\sptest.exe`, no warnings.
- `build\sptest.exe test` -> 0 failures (16 pass, 5 skipped without `--game`).
- `build\sptest.exe test --game "<game>"` -> 0 failures (21 tests); prints
  `23 archives, 31999 entries`, `12 archives, 612 entries extracted`, `4 SWFs checked`,
  `markers 2 1 6 6 2, 224019 bytes of scripts`.
- `cmd /c build.bat` -> `Built build\dinput8.dll` (the `sp_*` code is not linked into the DLL
  yet; the script-patch RCDATA is already embedded).
- Task 5's output equals the reference on the five real scripts byte for byte (224 019 bytes,
  checked locally with the `.ps1`'s own functions; Task 6 makes that a committed golden test).
- Game install is back at the released 0.5.1 state: original DLL and the owner's ini
  (`skip_intro=1`, no `file_trace` key). Backups in
  `build\file-trace-install-backup-20261031-180727\`.
- Branch commits: `6fc5c82` research, `f96527b` DZIP + harness, `eb6e135` SWF + inflate,
  `cd18318` DDS icons, `2bfa968` plan note, `8e56ada` AGENTS probe fix, then the edit engine.

Not implemented: Tasks 6-9. The harness `build` command is a stub (`not implemented yet (Task 6)`).

## Next item

Task 6 of the plan, "Overlay build + golden test against the reference":

- Create `src/sp_overlay.{c,h}` and `tools\patchtest\golden.py`; add the `build <game> <out>`
  command to the harness (it already links the RCDATA and `sp_script_apply`).
- `sp_overlay_build` mirrors the `.ps1` main flow: source of each target (loose file, else the
  `abetterui.dzip` entry), the read icons (`Get-ReadIcons`, needs the new file-backed DZIP
  reader for `pack0.dzip`), the script archive with the chosen features, the two SWFs only when
  ORIGINAL, the `cache.key`, and the redirect table.
- Verify: `python -I tools\patchtest\golden.py "<game>" "<scratch>"` -> `GOLDEN OK`; then a
  second `build` on unchanged inputs logs `cache: reused`.
- The reference inputs are the old patcher's backup (unpatched `base_scripts.dzip`,
  `ui_trade.swf`, `ui_storage.swf`); `<scratch>` must be on the game's volume for hard links.

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

- Task 6 needs a **file-backed DZIP reader**: `pack0.dzip` (10.5 GB) holds 1 099 item icons and
  63 `def_*.xml` (`z_emc_data.dzip` 64 more, `ZZ_UPatch.dzip` 16), and `sp_dzip_open` takes a
  buffer. The overlay `globals\gui\icons\tooltip\alreadyread_64x64.dds` is in `abetterui.dzip`
  (61 MB, fine for the buffer API). Noted in the plan.
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
```

Then read Task 6 of `docs/plans/2026-10-31-dll-script-patches.md` and lines ~1486-1620 of
`tools/script-patches/ScriptPatches.ps1` (the install flow and `Get-ReadIcons`).

Then read Task 5 of `docs/plans/2026-10-31-dll-script-patches.md` and lines ~424-1345 of
`tools/script-patches/ScriptPatches.ps1` (the fragments, `Add-Edit`, `Get-CollectBody`).
