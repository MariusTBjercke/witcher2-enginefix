---
name: w2-game-re
description: Use when Witcher 2 Engine Fix needs something from inside witcher2.exe - finding or confirming a function, global, vtable slot, import slot or struct offset, explaining why a game behaviour happens, or porting an address from the Steam build to GOG. Not for changing the game's scripts or UI movies (w2-script-patch) or for testing in the running game (w2-runtime-test).
---

# Game reverse engineering

Every patch rests on an address and a contract someone proved. This role finds them, proves them
from the binary, and leaves them where the next session does not have to find them again.

Read first: `AGENTS.md` ("Research setup", "Rules", "Verified facts"), `docs/addresses.md` (what is
already mapped), `docs/research.md`, the `docs/fixes/` entry for the area (`docs/fixes/README.md`
lists them) and, for GOG, `docs/fixes/gog-build.md`. Do not repeat those here.

## Method

1. **Check what is known.** Search `docs/addresses.md`, `docs/fixes/` and `src/` for the function
   or the behaviour before opening Ghidra.
2. **Name the build.** Steam analysis uses the unpacked copy, never the shipped exe. Steam and GOG
   are different builds: an address from one means nothing on the other.
3. **Find an anchor.** In order of strength: an import slot's callers, a string (config keys and
   most engine text are UTF-16, so search both encodings), a vtable, a function already in
   `docs/addresses.md`. The config name pool has no static xrefs; go through the registration code.
4. **Read the contract.** Disassemble as well as decompile. Count stack arguments, check `RET`
   against `RET n`, and find the `this` register. Look at one or two callers. The decompiler hides
   things that matter: the 32-thread hang only made sense in the `SHL`/`TEST`/`JNZ` bytes.
   For a virtual call, read the slot out of the class's vtable (`docs/engine-classes.md` has the
   vtable addresses; `tools/dump-engine-classes.py`'s `build()` also reads the GOG exe) and check
   the callee's `RET n` against the call site's pushes. A function that merely fits the story is
   not the callee.
5. **Prove the bytes.** Note the original bytes a patch will check. Search the file to confirm a
   pattern matches once. Map file offsets with the formula in `AGENTS.md`.
6. **Port to GOG by evidence.** Use the same string, import or caller, then compare the logic.
   Identical bytes prove little on their own; check that the surrounding function is the same one.
7. **Say what is not proven.** "The code reads this way" is not "seen in game". Hand anything that
   needs the running game to `w2-runtime-test`.
8. **Record it.** A `docs/fixes/` entry with symptom, addresses, bytes, proof and how to turn it
   off. New addresses go in the code as `#define`s, then `python -I tools/address-table.py`.

No live breakpoints in the game process: prefer passive in-process logging.

## Ghidra

- The MCP bridge can connect and still leave its tools uncallable. The HTTP bridge on
  `http://127.0.0.1:8089` is then the way in: `list_open_programs`, `analysis_status`,
  `decompile_function?address=`, `disassemble_function?address=`, `get_xrefs_to?address=`.
- Always pass `program=` (URL-encoded project path). Several programs are open, and the current
  one may be the other build.

## Done when

- Each claim has an address, the bytes or calls behind it, and the build it holds on.
- What needs the running game is named as such, with the log line that would show it.
- Findings are written to the docs, or listed for the caller to record.

## Delegating this pass

Spawn a subagent with: "You are the reverse engineer for Witcher 2 Engine Fix. Load the
`w2-game-re` skill, then <question, naming the build>. Report per build: the address, the
convention and arguments or the offset, the bytes that prove it and what remains unproven. Do not
edit repository files and do not commit."
