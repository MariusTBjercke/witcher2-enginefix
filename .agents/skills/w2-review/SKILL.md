---
name: w2-review
description: Use when a Witcher 2 Engine Fix change looks finished and before it is committed or released - checks patches against the definition of done, runs tools/check-repo.ps1 and looks for what the author missed. Not for making the change (w2-game-re, w2-script-patch) or running the release steps (w2-release).
---

# Review

A wrong byte here kills the game with no error, and there is no CI. This role is the check that
would otherwise not happen.

Read first: `AGENTS.md` ("Rules"), `docs/conventions.md` ("Fixes and evidence"), and the diff.

## Method

1. **Run `powershell -ExecutionPolicy Bypass -File tools\check-repo.ps1`**, with `-Game "<game>"`
   when an install is at hand.
2. **Every patch site.** The DLL checks the original bytes before it writes, the check is the
   full pattern, and a mismatch logs and turns off only that feature.
3. **Hooks.** Calling convention and argument count match the original, the original is reached
   through the saved pointer, and hooks are in place before the game's code can run. Compare
   worker thread timing with when the game gets there: `worker` sleeps 1.5 s after load.
4. **Off switch.** A new behaviour has an ini key, is explained in
   `dist-files/witcher2_enginefix.ini`, and with the key at 0 the game runs as before.
5. **Other players' setups.** Vanilla and Project Mersey, DXVK and not, Steam Cloud on and off, and
   CPUs, displays or mods this machine does not have.
6. **Docs.** A `docs/fixes/` entry with symptom, addresses, bytes, proof and the off switch;
   `CHANGELOG.md`; `docs/README.txt` and `README.md` when players see the change.
7. **Claims.** Every "fixed" must match what was seen: in game, in a log or only in code.
8. **Tracked files.** No machine paths, Steam IDs, game files, or text from personal notes.

## Done when

- `tools\check-repo.ps1` passes.
- Findings are listed most severe first with file:line, or the report says none were found.

## Delegating this pass

Spawn a subagent with: "You are the reviewer for Witcher 2 Engine Fix. Load the `w2-review` skill
and review <commit range or diff>. Report defects most severe first with file:line and what would
go wrong in game, then anything unverified. Do not edit or commit."
