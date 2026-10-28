> **Superseded 2026-10-28:** solved. Root cause and fix are in `docs/fixes/save-previews.md`
> ("Trace result" and "Fix"). Kept only as a record of the earlier dead ends.

# Item: missing save previews

Goal: identify why new saves lack their 640x360 BMP sidecars and validate a fix
without losing saves or destabilizing the working graphics mod.

Status: investigation in progress, unassigned for the next session; no fix.
Source of truth: `docs/fixes/save-previews.md`, especially its opening table.
Owned files: that investigation note, this item, `tools/savepreviewprobe/`.
Depends on: no other plan item. NPC/sun-shadow/menu work remains deferred.

Next action: read-only review of capture/storage code and retained evidence;
identify an unanswered boundary and design a separately validated diagnostic.
Do not run the retired CDB software-breakpoint workflow, repeat the ini-only
backend comparison, or treat the standalone probe as an in-game reproduction.

Verify/acceptance: after a future fix, human launches through Steam/Launcher,
loads a save, creates a NEW manual slot, and sees the correct preview in Load.
Its matching `_640x360.bmp` must exist and decode correctly. Existing saves
must be retained; no new crash or change to the installed game exe is allowed.

Current installed state: original DXVK restored; original CloudStorage=true
User.ini and Rendering.ini read-only; enginefix unchanged. Eleven SAVs and one
stock BMP verified intact after the diagnostic crash; no debugger/helper active.
