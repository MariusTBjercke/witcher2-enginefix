> **Superseded 2026-10-28:** solved. Root cause and fix are in `docs/fixes/save-previews.md`
> ("Trace result" and "Fix"). Kept only as a record of the earlier dead ends.

# Handoff: missing save previews — investigation unresolved

## Goal

Find why The Witcher 2 Steam 3.5.0.26 creates SAVs without their 640x360 BMP
sidecars, then validate a fix while preserving the working mod and all saves.
Workspace: <repo>. No Git metadata exists.

## Source of truth

| File | Purpose |
|---|---|
| AGENTS.md | Paths, build rules, incident warning |
| docs/fixes/save-previews.md | Read opening results table FIRST; detailed evidence, dead ends, addresses, backups |
| docs/plans/progress.json | Current item save-previews, in_progress, unassigned |
| docs/plans/items/save-previews.md | Next action and acceptance check |
| docs/validation/save-preview-*-4k-2026-10-28.log | Isolated native/DXVK probe results |
| docs/validation/save-preview-capture-20261028-190954-UTC.log | Failed live trace; READY is not proof of captured data |
| docs/validation/save-preview-breakpoint-crash-2026-10-28.log | Offline confirmation of breakpoint-induced crash |

## Current state

Missing thumbnails remain UNFIXED. Native D3D9 and DXVK game tests both lack
BMPs; standalone 4K readback/encoding succeeds with both. CloudStorage=false
in locked User.ini did not select local storage: startup can enable Steam
storage in memory. Do not repeat that ini-only comparison.

The live CDB trace caused a separate crash at its FIRST breakpoint009B6BC5
(exception4000001F, CC in dump). No capture HRESULTs were collected. Its helper
throws before attach and command file quits before breakpoint installation.
No debugger/helper remains active. The tiny canaries did not validate sustained
breakpoint handling; debugger-lifecycle cause remains unproved. Do not reuse
this live software-breakpoint workflow.

Original DXVK, enginefix and User.ini restored/unchanged; CloudStorage=true,
User.ini and Rendering.ini read-only, 3840x2160, UberSampling=0. All11 existing
SAVs and the stock AutoSave_0000 BMP match pre-crash hashes. Latest save0009;
no new save was created in the crash. Local copied saves remain in Documents.
Backups/manifests: build/save-preview-local-test-20261028-183244-UTC/ and
build/save-preview-native-test-20261028-184254-UTC/. Incident dump/runtime log:
build/save-preview-breakpoint-incident-20261028/. See investigation for hashes.

## Next item

Continue save-previews with READ-ONLY review of the mapped capture/storage path.
Choose an unanswered boundary and a separately validated diagnostic before
another human test. Full configuration reset, lower-resolution game test and
controlled mod A/B were NOT performed; none is an established local fix.
Verify a future fix through a NEW manual save, readable matching BMP and correct
Load-menu image, preserving old saves and without a new crash (progress.json).

## Critical constraints

Never modify installed witcher2.exe. Do not delete/overwrite saves or reset the
hand-tuned configuration wholesale. Human must launch through Steam/Launcher.
Respect read-only User.ini/Rendering.ini. No new runtime thumbnail hook exists.
Do not confuse the diagnostic crash with the original thumbnail failure.

## Known risks and open questions

The first failing capture/storage boundary remains unknown. No failed capture
HRESULT was collected, and a genuine local-backend game comparison is still
unverified. The debugger crash has its own documented cause and must not be
used as evidence for the thumbnail bug. Review existing evidence before
designing any additional runtime instrumentation or asking for another test.

## Other project context

Graphics presets remain installed: Ctrl+F10, startup extreme, grouping the four
working distance/LOD controls. See docs/fixes/graphics-presets.md and its
validation record (57 checks previously passed; no rebuild for documentation).
Further NPC streaming, sun-shadow quality and menu-button work are deferred by
owner. Do not resume those as a substitute for the thumbnail investigation.

## First command

```powershell
Get-Content -LiteralPath AGENTS.md,docs/fixes/save-previews.md,docs/plans/progress.json
```
