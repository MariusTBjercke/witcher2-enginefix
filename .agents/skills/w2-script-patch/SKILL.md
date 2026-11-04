---
name: w2-script-patch
description: Use when Witcher 2 Engine Fix changes what the game's scripts or UI movies do - adding or editing rows in src/scriptpatches/edits.txt, the .ws fragments, new script entries, the patched base_scripts.dzip overlay, ui_trade/ui_storage SWF patches or the read icons - or when a player reports a "Script compilation error". Not for engine hooks in the DLL (w2-game-re) or for the in-game test itself (w2-runtime-test).
---

# Script and overlay patches

The DLL builds patched copies of the player's own files at startup. Those files differ between
installs, and a mistake here stops the game at the compile dialog, so this role protects two
things: the game always starts, and a feature is applied whole or not at all.

Read first: `docs/fixes/dll-script-patches.md`, `docs/research.md` section 9, the header of
`src/scriptpatches/edits.txt`, and the last section of `docs/fixes/shop-book-read-marker.md`.

## Method

1. **Work from the game's text.** Extract the script from `base_scripts.dzip` with `dzip_index`
   and `dzip_read` in `tools/build-interior-maps.py`. Check the anchor in both a vanilla install
   and one with Project Mersey: their scripts differ.
2. **Choose the row.** An anchor must match exactly `count` times. Prefer a short anchor that both
   script sets share; if a line differs between them, replace behaviour in a new function.
3. **Check every dependency between files.** A file can come from somewhere other than our archive
   (another mod, a user content package), and the compiler stops on an unknown name. A call must
   not reach into another patched file unless that is unavoidable. Shared helpers go in our own
   entry, `game\gui\w2ef_books.ws`. Private members force code into their own file: note that risk.
4. **Follow the game's format.** New scripts are CRLF: the game compiled a bare LF file to nothing,
   with no error. Every entry in `base_scripts.dzip` is compiled as a script, so no other file
   type goes in it; the carrier archive holds those.
5. **Keep each feature switchable.** A new feature gets a `[scriptpatches]` key, a status field and
   `sp_script_apply` in `src/sp_overlay.c`, and its off state must leave the text untouched.
6. **Bump `OV_CACHE_LAYOUT`** whenever the patched output changes without a version change, or a
   stale cache is served.
7. **Update the tests.** `test_script_real` in `tools/patchtest/patchtest.c` holds the expected
   block count per file, plus the checks on `w2ef_books.ws`.

Offline tests prove the edits apply and what the text says. Only the game's compiler proves it
compiles, so any script change ends with `w2-runtime-test`.

## Done when

- `tools\patchtest\build.bat`, then `build\sptest.exe test --game "<game>"`, gives 0 failures on
  every install at hand. `sptest build <game> <out>` shows the expected status.
- A player's install with a replaced script still starts, or the risk is written down.
- The owner has seen the game start.

## Delegating this pass

Spawn a subagent with: "You are the script patch reviewer for Witcher 2 Engine Fix. Load the
`w2-script-patch` skill, read the diff of src/scriptpatches and src/sp_*.c, and check anchors,
cross-file calls, line endings, OV_CACHE_LAYOUT and the tests. Report problems most severe first,
with file:line. Do not edit or commit."
