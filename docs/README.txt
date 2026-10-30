WITCHER 2 ENGINE FIX
Draw-distance, save-preview, interior-map and quality-of-life fixes for The Witcher 2: Assassins of Kings -
Enhanced Edition (3.5.0.26)
https://github.com/MariusTBjercke/witcher2-enginefix
=========================================================================

A small DLL that the game loads by itself and that adjusts specific engine
values and bugs in memory while the game runs. witcher2.exe and your saves are
never modified.

What it does:
- Save previews: new saves show their screenshot in the Load menu again
  instead of a purple/blue gradient. Old saves cannot get their picture back.
- Save list leak: every Load/Save menu open leaked ~0.9 MB per save until
  you quit (hundreds of MB with many saves). Fixed; no save is deleted.
- Graphics presets: Ctrl+F10 cycles baseline / high / ultra / extreme. Each
  level sets mesh LOD, foliage distance, mesh visibility and mesh-shadow
  distance together, shown on screen. Starts at extreme.
- The LOD values stay put when the options menu rewrites them.
- Ctrl/Alt+F9/F10 never trigger a quick load. Plain F9 still quick-loads.
- Interior maps: 25 interiors (temples, inns, Loredo's house, towers) had a
  black map screen (M) with one huge pin, because the game has no big map for
  them. Loose files under CookedPC\globals\ give each one an upscaled copy of
  its own minimap, so the map shows and zooms the right way.
- Skip intro ([startup] skip_intro=1, off by default): starts at the main
  menu without the logo videos and the intro movie (a short black moment
  remains).

Optional, off until you run it - script patches:
  Witcher2EngineFix_Optional\ScriptPatches\Install.bat asks for each one:
  - Shop book marker: at merchants and in the storage chest the "already
    read" overlay on a book normally only means "you carry a copy". It also
    shows for books Geralt has read (the game records each book read from the
    inventory) or whose bestiary knowledge and journal entries he already has.
    Books get "Read", "Unread" or "Nothing new" after their type in the list,
    and the tooltip lists what is still new.
  - Continue: the main menu gets a Continue button that loads the newest save.
  - Quit in the pause menu: quits the game after a confirmation, without
    going back to the main menu first.
  It patches the files your game uses: CookedPC\base_scripts.dzip, for
  the book marker the trade/storage screens (works with or without Witcher 2
  HD UI AI Upscale and Project Mersey). Texts follow the game's language
  (English, Polish, German, French, Spanish, Italian, Russian, Czech,
  Hungarian, Turkish, Japanese, Chinese). Close the game first. Run Install.bat
  again to change the choice. Originals are kept in its backup\ folder;
  Uninstall.bat restores them. If another mod later replaces one of these
  files, run Install.bat again.

Install:
Extract the zip into the game folder (the one with bin\ and CookedPC\), so
that dinput8.dll ends up in bin\ next to witcher2.exe, e.g.
  ...\steamapps\common\the witcher 2\bin\dinput8.dll
Start the game as usual. bin\witcher2_enginefix.log should begin with
"witcher2_enginefix 0.4.0 loaded".

Settings: bin\witcher2_enginefix.ini (read at startup, documented inside).
  [presets] default=extreme      startup level
  [saves]   fix_thumbnails=1     0 turns the save-preview fix off
  [saves]   fix_list_leak=1      0 turns the save-list leak fix off
  [startup] skip_intro=0         1 starts at the main menu (no logos, no intro)
  [general] enabled=1            0 turns everything off

Uninstall: delete bin\dinput8.dll, bin\witcher2_enginefix.ini,
CookedPC\globals\map_scales.csv and the *_2048x2048.dds files this zip put in
CookedPC\globals\gui\maps\ (the game's own maps are inside pack0.dzip).
A mod that ships its own globals\map_scales.csv conflicts with the interior
maps; the one copied last wins.

Tested on Steam 3.5.0.26, Windows 11, with and without DXVK.
License: MIT (LICENSE_Witcher2EngineFix.txt). Not affiliated with CD PROJEKT RED.
