# Definition of done

The project is done when all of the following hold.

1. **Non-invasive.** `witcher2.exe` and every shipped file in `CookedPC` that the mods
   did not already replace are byte-identical to a clean Steam install. Reverting =
   delete `bin\dinput8.dll` + `bin\witcher2_enginefix.ini`, restore the two
   `Config\*.ini` from `_backup_premods`, run `restore-original.ps1`, then Steam
   *Verify integrity*. That path must stay true after every change.
2. **Stable.** The game reaches the main menu and a loaded save with the DLL installed,
   every session, with no new Event Log faults. Any feature that cannot meet this is
   disabled by default in the shipped ini.
3. **Draw distance.** Foliage, character meshes and shadows all stop visibly popping at
   the distances a modern GPU can afford. LOD multipliers are live-tunable and the
   active value is visible on screen.
4. **Documented.** Every fix has a `docs/fixes/*.md` entry containing: what the engine
   actually does (function addresses), what was changed, how it was verified, and how
   to turn it off.
5. **Honest about dead ends.** Anything found to be impossible or not worth it is
   recorded with the evidence, so the next session does not repeat it.

Explicitly **out of scope** unless the owner re-opens them: content changes (items,
characters, quests), anything that requires redistributing game files, and menu
buttons (deferred — see `docs/fixes/roadmap-shadows-characters-menus.md`).
