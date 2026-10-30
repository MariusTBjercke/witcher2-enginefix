# Roadmap: shadows, character distance, menu buttons

Written 2026-10-28 after the LOD multipliers worked. These are the three open asks,
with what is already known and what has to be found.

## 1. Shadow draw distance

**Not an ini setting.** The full settings key pool in `.rdata` contains only quality
and count keys for shadows:

`ShadowQuality`, `ShadowedLights`, `MaxCubeShadowSize`, `MaxCubeShadowCount`,
`MaxSpotShadowSize`, `MaxSpotShadowCount`, `m_shadowQuality`, `DF_CastShadows`,
`DF_CastShadowsWhenNotVisible`, `DF_ForceNoShadowLOD`.

The distance itself lives on the light/caster data:

| Property | Kind |
|---|---|
| `m_shadowRenderingDistance` | per-light float in `.w2l` / `.w2ent` |
| `m_overrideMeshShadowRenderingDistance` | per-mesh override |
| `m_shadowDistanceScale` | per-mesh scale |
| `EShadowRenderingDistance` | enum, shadow-distance mode |
| `m_sunShadowDepth`, `EGlobalShadowMode`, `m_shadowWithScale` | sun/global mode |
| `CEnvShadowsParameters` | level environment block |

All of these are read through the engine's property system, whose names live in the
CName pool — no static xrefs, so they have to be reached from the consumer side, not
by chasing the string.

Next step: find where `m_shadowRenderingDistance` is consumed (the light setup /
shadow-map pass), then either scale the value where it is applied or patch the field
on each light when the level loads.

## 2. Characters disappearing at distance

Three mechanisms stack:

1. `CharacterLodGameplayDownscale` (bool, settings `+0x38`) — **already forced to 0**
   by the DLL. This only removes the extra gameplay-time reduction.
2. Per-mesh LOD switch distances baked into the character `.w2mesh` — the global
   `MeshDistanceScale` multiplier now sits at 4.0 and visibly helped, which proves
   characters do read `+0x6C`.
3. A separate render-distance cutoff or actor streaming can make a character
   disappear entirely. `m_maxRenderDistance` is a candidate property name; it has
   not yet been mapped to the affected NPC's cutoff.

Update: a mesh-proxy cutoff has been found in `FUN_009bade3`, comparing squared
distance to proxy `+0xF0`. The constructor is `FUN_009bc8ed`. See the
[investigation](quality-audit-2026-10-28.md) for verified assembly, producers and
limitations. Next: trace an affected NPC into this test, distinguish renderer
culling from actor unloading, and verify character classification before designing
a multiplier.

## 3. "Continue" on the main menu / "Exit game" in the pause menu

**The Witcher 1 implementation does not port.** There, both buttons were added from
`enginefix_hook.lua`: the W1 engine exposes its UI as Lua scripts, so a hook could
wrap `CMainMenuPanel.new` and `CGuiNewSystemPanel.new` and inject a copied button
definition. TW2 has no Lua - its UI and gameplay logic are compiled scripts inside
`CookedPC\base_scripts.dzip` (proprietary `DZIP` container, not a zip; magic `DZIP`,
7-Zip cannot open it) plus native GUI code.

So the options are:

| Path | Work | Risk |
|---|---|---|
| Script edit via REDkit | unpack/patch `base_scripts.dzip` with the official editor, add the button in the menu script, repack | REDkit version must match 3.5.0.26; touches the modded `base_scripts.dzip` we already replaced |
| Native GUI hook in the DLL | find the menu panel construction, inject a control, rewire the layout | large RE effort, high regression risk on a UI that was never meant to be extended |

**Update 2026-10-30: a script path exists after all**, now implemented, see
`docs/fixes/menu-buttons.md`. The script patcher
(`docs/fixes/shop-book-read-marker.md`) can rewrite one entry of the installed
`base_scripts.dzip` in place, and the game compiles that archive at startup. Both menus are
`CGuiMainMenu` (`game\gui\guimainmenu.ws`, `ui_mainmenu.swf`), filled from script:
`FillMainMenu` / `FillMainInGame` call `InsertMenuButton( items, id, text, description )`,
clicks go to `InputMainMenu` / `InputMainInGame` by button id (`EMenuButtons`). So:

* Continue: `saves` (`array< SGuiGameSaveInfo >`) is already filled when the main menu opens:
  `009A515C` calls the list builder `009A49B9` and then treats `saves[0]` as the newest save
  (it picks the menu background from its world). A button before New Game whose handler does
  what `InputLoadGame( 0 )` does (`ClosePanel(); theGame.ResetTutorialSettings(); LoadGame( 0 );`,
  refusing a dead-player save) needs no extra list build. The save's `displayName` can be the
  button description. No localized "Continue" key known yet.
* Exit from the pause menu: `MM_BTN_QUIT` / `QuitApplication()` exist but are only offered by
  `FillMainMenu`; `FillMainInGame` ends with "Exit current game" (`theGame.ExitGame()`). Adding
  the Quit button there reuses the localized `menuQuit` text. Whether `QuitApplication()` with
  a world loaded shuts down cleanly is unknown (see the W1 precedent below); fallback: exit to
  the main menu first and quit when it opens.

**Important precedent from W1:** quitting with an area loaded crashed the engine during
shutdown (`CAurObject` destructor dispatching a `Dying` event into a dead script
system). W1 shipped the pause-menu exit anyway, but it is the reason the game itself
only offers "Exit to main menu" during play. Expect the same class of problem in TW2
and plan for "return to main menu, then exit" if a direct exit cannot be made clean.
