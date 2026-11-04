WITCHER 2 ENGINE FIX
Draw-distance, save-preview, interior-map and quality-of-life fixes for The Witcher 2: Assassins of Kings -
Enhanced Edition (3.5.0.26 on Steam, 3.5.0.26g on GOG)
https://github.com/MariusTBjercke/witcher2-enginefix
=========================================================================

A small DLL that the game loads by itself and that adjusts specific engine
values and bugs in memory while the game runs. witcher2.exe, the game's data
files and your saves are never modified.

What it does:
- Save previews: new saves show their screenshot in the Load menu again
  instead of a purple/blue gradient. Old saves cannot get their picture back.
- Save list leak: every Load/Save menu open leaked ~0.9 MB per save until
  you quit (hundreds of MB with many saves). Fixed; no save is deleted.
- Faster save list: the list unpacks only the few parts of each save it
  reads, in memory, instead of every save in full (~2.5x faster with 120
  saves). Loading a save is unchanged.
- Seamless interiors: houses, shops and inns without a fade. The outdoors stays loaded
  (visible through the windows), the area's interiors load with the area, so
  open doors show the real rooms, and a door takes a few hundredths of a
  second without a fade instead of ~2.2 s. The lighting blends over 1 s
  instead of jumping. Doors that change area (the gate into Flotsam) keep a
  short fade.
- Display mode in Options > Graphics: full screen, borderless window or
  windowed, switched at once without a restart.
- Snipping Tool and Windows keys work in game (Win+Shift+S, Print Screen,
  Win+D); a Windows key tapped on its own still does not open Start.
- Crash guard: a physics ray cast against an object that was just unloaded
  (seen once leaving the Flotsam inn) no longer closes the game.
- Clean quit: Quit in the pause menu no longer crashes the game while it
  shuts down (nobody saw it, but each quit left a crash dump).
- 32-thread CPUs: on a CPU with 32 or more logical processors the game now
  starts; its own CPU count never ended there.
- Crash capture: a crash is logged with a small dump next to the log, so it
  can be reported.
- Graphics presets: Ctrl+F10 cycles baseline / high / ultra / extreme. Each
  level sets mesh LOD, foliage distance, mesh visibility and mesh-shadow
  distance together, shown on screen. Starts at extreme. The same levels are
  a Draw distance row in Options > Graphics; the game remembers that choice.
- The LOD values stay put when the options menu rewrites them.
- Ctrl/Alt+F9/F10 never trigger a quick load. Plain F9 still quick-loads.
- Interior maps: 25 interiors (temples, inns, Loredo's house, towers) had a
  black map screen (M) with one huge pin, because the game has no big map for
  them. Loose files under CookedPC\globals\ give each one an upscaled copy of
  its own minimap, so the map shows and zooms the right way.
- Faster start ([startup] fast_start=1, on by default): removes most of an idle
  wait of about 11 s while the sound system loads. Measured on one machine: main
  menu after 6.5 s instead of 16 s. Your gain depends on your disk and system.
- Skip intro ([startup] skip_intro=1, off by default): starts at the main
  menu without the logo videos and the intro movie (a short black moment
  remains).
- Book marker: the "already read" overlay on a book normally only means
  "you carry a copy". It now also shows for books Geralt has read (the game
  records each book read from the inventory). Books get "Read", "Unread" or
  "Nothing new" (all its bestiary knowledge and journal entries are known
  already) after their type, and the tooltip lists what is still new. This
  works in Geralt's own inventory (with the tooltip; a book you read there
  updates at once), at merchants and in the storage chest (on both sides, so
  also for the books you are about to sell or store) and in the loot window -
  the loot window has no tooltip, so there "(Nothing new)" comes after the
  item name. There and in the small preview next to a container a read book
  shows its icon with the tick on it. Texts follow the game's language
  (English, Polish, German, French, Spanish, Italian, Russian, Czech,
  Hungarian, Turkish, Japanese, Chinese).
- Continue: the main menu gets a Continue button that loads the newest save.
- Quit in the pause menu: quits the game after a confirmation, without
  going back to the main menu first.
- Medallion on the move: Geralt keeps moving when you use the medallion, and
  it is ready again after 1 second instead of 2.

The last four change the game's scripts and UI screens. The DLL does that
at game start without touching the game's files: the first start builds
patched copies of the files your game uses (base_scripts.dzip, also Project
Mersey's; the trade and storage screens, also Witcher 2 HD UI AI Upscale's;
the read-book icons) into bin\witcher2_enginefix_cache\ and the game reads
those instead. Later starts reuse them until a game file changes. A screen
another UI mod changed in a way the patch does not recognize is left alone.
Mods installed as user content (Documents\Witcher 2\UserContent) can bring
their own copies of scripts, which the game uses instead; the patches go
into those copies too. If a copy is too different for a patch, that feature
stays off and the game starts without it.

Upgrading from 0.4 or 0.5 with the old script patches installed: just
extract this version over it. The first start puts the original files back
once (from Witcher2EngineFix_Optional\ScriptPatches\backup) and from then on
the DLL does the patching. After that start you can delete the
Witcher2EngineFix_Optional folder.

Antivirus warnings:
Some scanners flag this mod. That is a false positive, common for mods
like this. dinput8.dll is a proxy DLL that the game loads by itself, reads
the hotkey state and patches values in the game's memory - the same things
heuristics look for. Nothing is downloaded or sent anywhere. The full source
is on GitHub (link above) if you prefer to build the DLL yourself.

Install:
Extract the zip into the game folder (the one with bin\ and CookedPC\), so
that dinput8.dll ends up in bin\ next to witcher2.exe, e.g.
  ...\steamapps\common\the witcher 2\bin\dinput8.dll              (Steam)
  ...\GOG Galaxy\Games\The Witcher 2\bin\dinput8.dll              (GOG)
Start the game as usual. bin\witcher2_enginefix.log should begin with
"witcher2_enginefix x.y.z loaded" (x.y.z is this version), then a "build:"
line naming Steam 3.5.0.26 or GOG 3.5.0.26g.

DXVK (strongly recommended, at 4K a must):
The game is 32-bit (4 GB at most), and with its own Direct3D 9 every
texture also takes room there. At 4K it runs out of memory and crashes
soon after loading a save in Flotsam, with or without this mod. DXVK keeps
the textures on the graphics card.
1. Download DXVK from https://github.com/doitsujin/dxvk/releases
2. Copy x32\d3d9.dll (the 32-bit one) into bin\ next to witcher2.exe.
3. If the game then hangs after a while (GPU reset, "device lost"), create
   bin\dxvk.conf with the line: dxvk.enableDescriptorHeap = False
Delete bin\d3d9.dll to go back to the game's own Direct3D 9.

Settings: bin\witcher2_enginefix.ini (read at startup, documented inside).
  [presets] default=extreme      startup level
  [saves]   fix_thumbnails=1     0 turns the save-preview fix off
  [saves]   fix_list_leak=1      0 turns the save-list leak fix off
  [saves]   fast_list=1          0 unpacks every save in full for the list
  [loading] interiors=2          1 loads each interior on its first visit,
                                 0 restores the game's doors
  [loading] fast_doors=1         0 restores the game's door timing
  [loading] door_light_blend=1.0 0 switches the lighting at once at doors
  [loading] preload_memory_mb=1280, door_memory_mb=1024
                                 memory guard for the preloaded interiors
  [hotkeys] windows_keys=1       0 blocks the Windows keys in game again
  [window]  borderless=0         start-up fallback; Options owns it after
  [startup] skip_intro=0         1 starts at the main menu (no logos, no intro)
  [startup] fast_start=1         0 keeps the game's own idle wait while starting
  [stability] raycast_guard=1, cpu32_guard=1, quit_guard=1
                                 0 turns that crash or hang guard off
  [scriptpatches] book_marker=1, continue=1, pause_quit=1, window_mode=1,
                  medallion=1
                                 0 turns that one off
  [general] enabled=1            0 turns off everything the DLL does

Known issue: the pause-menu Quit confirmation needs one click before it
responds to hover. The Quit button still works.

Uninstall: delete bin\dinput8.dll, bin\witcher2_enginefix.ini, the logs and
any crash dumps (bin\witcher2_enginefix*.log / *.dmp), the folder
bin\witcher2_enginefix_cache, CookedPC\globals\map_scales.csv and the
*_2048x2048.dds files this zip put in CookedPC\globals\gui\maps\ (the game's
own maps are inside pack0.dzip). Nothing else needs undoing.
A mod that ships its own globals\map_scales.csv conflicts with the interior
maps; the one copied last wins.

Tested on Steam 3.5.0.26 and GOG 3.5.0.26g, Windows 11, with and without
DXVK. The seamless interiors stop preloading when less than 1280 MB is free
([loading] preload_memory_mb); see DXVK above.
GOG: every feature works there too. Save previews go next to the save in
Documents\Witcher 2\gamesaves, and the save list has no leak to fix. The
log ends with "exit: CorExitProcess(0)" instead of "unloaded": the .NET
runtime in the process (most likely the Galaxy overlay) ends the game
first. That is a normal quit.
License: MIT (LICENSE_Witcher2EngineFix.txt; it also lists the third-party
tinf inflate library inside the DLL). Not affiliated with CD PROJEKT RED.
