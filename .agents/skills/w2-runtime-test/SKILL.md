---
name: w2-runtime-test
description: Use when a Witcher 2 Engine Fix change has to be seen in the real game - installing a build, asking the owner for a start test or an in-game check, reading witcher2_enginefix.log or a crash dump afterwards, or storing the result as validation evidence. Not for offline tests or static analysis (w2-game-re, w2-script-patch).
---

# Testing in the running game

The game cannot be started by an agent, and a wrong hook can kill it silently. So each test has to
be cheap for the human, and the log has to be able to answer without them.

Read first: `AGENTS.md` ("Paths", "Build, install, release", "Rules"), `docs/fixes/crash-capture.md`
for a crash, and the `docs/fixes/` entry of the feature.

## Method

1. **Build and install.** Run `build.bat`. Check that the game and launcher are closed, or the DLL
   is locked. Copy `build\dinput8.dll` to `<game>\bin`. Copy the ini only when a change needs it,
   and say so: it replaces the owner's settings.
2. **Prepare the log.** Before asking for a run, make sure the behaviour logs a line that proves
   it. Passive logging beats a breakpoint; a live CDB session on the game is not allowed.
3. **Ask for one short test.** Name what to do, in steps, and what to look for. A new hook, vtable
   or import signature, or a `suppress_mod_keys` change, gets a start test before any other work.
4. **Read the result yourself.** `<game>\bin\witcher2_enginefix.log`, and `.prev.log` for the run
   before. A crash leaves a `.dmp` next to it; inspect dumps offline only. Compare with what the
   change predicted, including log lines that should not appear.
5. **Keep the evidence.** Interesting logs go to `docs/validation/<topic>-<date>`, with the install
   path replaced by `<game>`. Name the setup: Steam or GOG, Project Mersey or vanilla, DXVK or not.
6. **Report honestly.** Say what was seen, by whom and on which setup. "Starts" does not mean the
   feature worked, and a fix aimed at hardware nobody here has stays unproven.

When the first test fails, read the failure as evidence: the message in the dialog usually names
the wrong assumption. Do not patch around it blind.

## Done when

- The owner's result and the log agree with what the change predicted.
- The `docs/fixes/` entry says what was seen in game and when.
- What could not be tested here is named, with who could test it.

## Delegating this pass

Spawn a subagent with: "You are the runtime log reader for Witcher 2 Engine Fix. Load the
`w2-runtime-test` skill, read <log> and compare it with <what the change predicted>. Report what
matched, what did not and what the log cannot show. Do not edit or commit."
