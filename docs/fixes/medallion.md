# Medallion on the move

Status: shipped in 0.9.0, checked in game by the owner on 2026-11-03. Target: Steam Enhanced
Edition 3.5.0.26, with and without Project Mersey (the anchors exist in both `player.ws` versions).

## Symptom

Pressing the medallion key stops Geralt for the use animation, and the key does nothing for 2 s
after the pulse.

## Cause

All of it is script (`game\player\player.ws`, `game\player\states\extendedmovable.ws`):

* `GI_Medallion` -> `TriggerMedallion()` -> `PlayerCombatAction( PCA_UseMedalion )`, which raises the
  `CombatAction` force event. The behavior graphs (`exploration`, `combat_geralt`, `stealth`,
  `fistfight`) answer with `ex_medalion_use`, `steel_high/low_medalion_use` or
  `stealth_medalion_use`. That full-body animation is the stop.
* The animation's only job is its `medalion_fx` event: `OnAnimEvent` -> `TriggerMedalionFX()`, which
  spawns `medalionEntity`, runs the place-of-power and q214 logic and starts the cooldown
  (`AddTimer( 'OnEnableMedallion', 2.f, false )`).
* The pulse, the radial blur and the sound `witcher/magic/anim_medalion` are FX tracks in
  `fx\characters\witcher\medalion\witcher_medalion.w2ent` (class `CGameplayFXMedalion`), not
  animation events, so they do not need the animation.

## Patch

Feature `Medallion` in `src/scriptpatches/edits.txt`, fragment `medallion_use.ws`:

* new `CPlayer.W2EF_UseMedallion()`: the `IsNotGeralt` check, `TriggerMedalionFX()`,
  `UseTalismanGuide()`, with no behavior event;
* the key handler calls it instead of `TriggerMedallion()`. `QTriggerMedallion` (quests) still uses
  the original with the animation;
* cooldown 1 s instead of 2 s: `W2EF_UseMedallion` swaps the timer right after `TriggerMedalionFX()`
  (`RemoveTimer`, then `AddTimer` with 1 s). The game's own timer line cannot be an edit row: Mersey
  adds a trailing comment to it, and `Replace` needs an anchor that covers the whole line. A
  medallion started by a quest keeps 2 s.

`TriggerMedalionFX` keeps its `m_canUseMedallion` guard, so the key is harmless during the cooldown.
Quest input blocks (`IgnoreGameInput( 'GI_Medallion' )`) act before the handler and still apply.

## Why 1 s

`witcher_medalion.w2ent`: `beginRadius` 0.1, `endRadius` 12, `distPerSec` 12, `sustainTime` 10. The
ring needs 1 s to reach its end, so 1 s keeps two rings from running at once. Highlights already
overlapped at 2 s because they last 10 s. The native tick of `CGameplayFXMedalion` was not traced,
so what a shorter value would do to it is unknown.

## Behaviour changes to check

* The medallion now also works where the force event used to be refused, such as during an attack
  or a roll.
* Geralt no longer touches the medallion on screen.

## Verification

`build\sptest.exe test --game <game>` applies the rows to the installed scripts (`script_real`:
1 block in `player.ws`, 1 in `extendedmovable.ws`). This does not compile WitcherScript: only the
game does, at start, and a compile error stops it with a message box.

In-game result: `docs/validation/medallion-2026-11-03.md`.

## Turning it off

`[scriptpatches] medallion=0`. The script cache is rebuilt at the next start.
