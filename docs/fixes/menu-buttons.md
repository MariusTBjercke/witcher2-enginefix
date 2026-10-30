# Menu buttons: Continue in the main menu, Quit in the pause menu

Status: implemented 2026-10-30 as the `Continue` and `PauseQuit` features of the opt-in
`tools/script-patches/ScriptPatches.ps1`. Confirmed in game by the owner the same day ("works
well"); no `witcher2.exe` fault in the Application log afterwards and the DLL log ends with a
clean `unloaded`. Script changes only, no DLL code. Target: Steam Enhanced Edition 3.5.0.26.

Same idea as witcher-enginefix QoL 7 (Continue) and QoL 2 (pause-menu exit), which were Lua
hooks there. Why TW2 needed another way, and what the menu code looks like, is in
`roadmap-shadows-characters-menus.md` section 3.

## The tool: ScriptPatches.ps1

Shipped as `Witcher2EngineFix_Optional\ScriptPatches\` (`ScriptPatches.ps1`, `Install.bat`,
`Uninstall.bat`). Grew out of the BookReadMarker patcher (`shop-book-read-marker.md`), which
holds the DZIP/swf details.

* Features: `BookMarker`, `Continue`, `PauseQuit`. `Install.bat` asks for each (Enter = yes);
  `-Features All` or `-Features Continue,PauseQuit` skips the questions. `-Action Check` lists
  each feature as installed or not and whether its anchors are found.
* Every Install rebuilds from the originals: the backup when the current file still has the hash
  we wrote, else the current file. So running Install again with another choice adds or removes
  features; a feature not chosen leaves its file untouched (swfs are restored, a created loose
  swf is deleted). Choosing nothing equals Uninstall.
* Each feature is a list of edits: code inserted Before or After the single line matching an
  anchor regex, as the first lines of a function Body, or at the End of the file. Code is
  written with 4-space indentation in the tool and gets tabs plus the surrounding indentation.
  Every block is wrapped in `//===witcher2-enginefix=== BEGIN <feature>` / `END <feature>`.
  An anchor that is missing or not unique refuses the whole install; nothing is written.
* Refuses if the original already contains our marker (for example a leftover BookReadMarker
  install: run that copy's Uninstall first), if the game from that folder is running, or if a
  patched file was changed by something else since.

## Continue (`game\gui\guimainmenu.ws`)

* `FillMainMenu`, before the Arena button: if `saves` is empty call `RefreshSaveData()`; if there
  is a save, `InsertMenuButton( AS_MenuItems, 9001, "CONTINUE", StrUpperUTF( saves[ 0 ].displayName ) )`.
  The save name is the button's description line. 9001 is outside `EMenuButtons`.
* `InputMainMenu`, first in the body: `if ( button == 9001 ) { InputLoadGame( 0 ); return; }`, the
  same path as picking the top entry of the Load list, including the refusal of a dead-player
  (Insane) save.
* `saves[0]` is taken as the newest save because the engine does the same: `009A515C` (main menu
  open) calls the list builder `009A49B9` and then picks the menu background from
  `saves[0].world`. To confirm in game.
* The label is the game's own `menuContinue` ("Continue", "Kontynuuj grę", "Spiel fortsetzen",
  "К игре", "繼續", ...), present in every text language (found by decoding the
  `.w2strings`, see `docs/research.md` section 9).

## Languages

Both buttons use the game's own texts, so they follow the text language with no table of ours:

| Lang | Continue (`menuContinue`) | Pause menu, game's own (`menuExitCurrentGame`) | Quit (`menuQuit`) | Question (`Exit game?`) |
|---|---|---|---|---|
| EN | Continue | EXIT TO MAIN MENU | QUIT | Quit game? |
| PL | Kontynuuj grę | WYJDŹ DO MENU GŁÓWNEGO | WYJDŹ Z GRY | Opuścić grę? |
| DE | Spiel fortsetzen | ZURÜCK ZUM HAUPTMENÜ | BEENDEN | Spiel beenden? |
| FR | Continuer partie | REVENIR AU MENU PRINCIPAL | QUITTER | Quitter la partie ? |
| ES | Continuar partida | SALIR AL MENÚ PRINCIPAL | SALIR | ¿Salir del juego? |
| IT | Continua la partita | TORNA AL MENU PRINCIPALE | ESCI | Abbandonare il gioco? |
| RU | К игре | ВЫХОД В ГЛАВНОЕ МЕНЮ | ВЫХОД | Выйти из игры? |
| CZ | Pokračovat ve hře | NÁVRAT DO HLAVNÍ NABÍDKY | KONEC | Ukončit hru? |
| HU | Játék folytatása | KILÉPÉS A FŐMENÜBE | KILÉPÉS | Kilépsz a játékból? |
| JP | 続行する | メインメニューに戻る | 終了 | ゲームを終了しますか？ |
| ZH | 繼續 | 回到主選單 | 離開 | 跳出遊戲？ |

TR exists too (its key table is not in Gibbed's list, so it was not dumped here; the game looks it
up the same way). Unlike W1 (where French "QUITTER" was both buttons), the game's pause-menu
button says "to the main menu" in every language, so the two buttons cannot be confused.

## PauseQuit (`game\gui\guimainmenu.ws`)

* `FillMainInGame`, after "Exit current game": the main menu's own Quit button
  (`MM_BTN_QUIT`, localized `menuQuit` / `menuQuit_t`).
* `InputMainInGame`, first in the body: `MM_BTN_QUIT` opens a confirmation box.
* `CGuiMainMenu` gets `W2EF_ConfirmQuit()` (creates `W2EFConfirmQuit in theHud.m_messages`,
  `ShowConfirmationBox`) and `W2EF_Quit()` (`QuitApplication()`, a private import, hence the
  callback), inserted before `InputMainMenu`.
* `class W2EFConfirmQuit extends CGuiConfirmationBox` at the end of the file: text
  `GetLocStringByKeyExt( "Exit game?" )` (the game's own, unused `CGuiConfirmGameExit` uses this
  key; falls back to "Quit the game?"), Yes calls `m_menu.W2EF_Quit()`, No does nothing.
* Focus: the first version opened the box with the pause menu still open. The box lives in
  `hud.swf` (`setConfirmationBox` on `theHud.m_hud.AS_hud`) and only reacted to hover after one
  click (owner, 2026-10-30). Now `W2EF_ConfirmQuit` calls `ClosePanel()` first, like the game's
  own boxes that open over the HUD, and No calls `theHud.ShowMainMenu()` to bring the pause
  menu back. Shipped in 0.4.0; owner tested 2026-10-31: **still needs one click** before the
  box reacts to hover. Known issue, left for now (minor). Likely cause: the menu's close (and
  its `EnableInput`) lands a frame after the box opened and turns the HUD's mouse input off
  again. A fix would open the box one frame later, which needs a timer on an entity (e.g. a
  player timer, i.e. patching `player.ws` too) and some in-game iterations.
* Risk carried over from W1: there, quitting with an area loaded crashed during shutdown (an
  invisible fault, but it filled the Event Log). Whether `QuitApplication()` with a world loaded
  is clean in TW2 is part of the in-game test: after quitting this way, Windows Event Log
  (Application, source "Application Error", `witcher2.exe`) must show no fault at that time.
  If it does, the fallback is to exit to the main menu first and quit when it opens.

## Verification so far (2026-10-30, copies of the owner's files)

* Copies with Mersey + HD UI and with vanilla scripts + abetterui only: Check; Install All;
  Install Continue (book patch and swfs removed again); Install BookMarker,PauseQuit through
  `cmd` (comma list as one argument); the interactive questions fed "Enter, n, j"; Uninstall.
  After Uninstall every file is byte-identical to its original and the created loose swfs are
  gone.
* The generated `guimainmenu.ws`: each block sits where intended (diff reviewed); every declared
  name checked against the compiler's keyword table (see `shop-book-read-marker.md`); braces,
  parentheses and quotes balanced; archive header hash valid.
* Owner's machine: the old BookReadMarker install was uninstalled (originals back), its folder
  removed, then ScriptPatches installed with all three features; backup byte-identical to the
  Mersey `base_scripts.dzip` and the HD UI swfs.

## In-game test

1. Start the game: it must reach the main menu (script compile).
2. Main menu: CONTINUE at the top with the newest save's name under it; clicking it loads the
   same save as the top entry of Load Game.
3. Pause menu (Esc): QUIT under "Exit current game"; No returns to the menu; Yes quits.
4. After quitting from the pause menu: no `witcher2.exe` fault in the Event Log.
5. The book marker still works at a merchant.
