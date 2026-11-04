# File access trace - 2026-10-31

Evidence for Task 1 of the plan for the DLL script patches; analysis in
`docs/fixes/dll-script-patches.md`. Game: Steam Enhanced Edition 3.5.0.26 with the owner's
Project Mersey + HD UI install, `bin\dinput8.dll` built from `dll-script-patches` and
`[diagnostics] file_trace=1`. Owner session: start, load a save, open the inventory, open a
merchant, open the inventory, quit.

The raw log is `<game>\bin\witcher2_enginefix.log` (3.0 MB, 14 760 lines) and is not committed:
it carries machine paths. Below the install path is replaced by `<game>`, timestamps are
dropped and `t=` is milliseconds since `DllMain`.

## Install (from `DllMain`)

```
file-trace: thread started, CreateFileW slot holds 7710EA50
file-trace: hooked kernel32.dll!CreateFileW            (one such line per slot, 14 in total)
file-trace: hooked after 0 ms (14/14 slots), current directory "<game>"
```

## Archives

```
t=+266ms  FindFirstFileW "<game>\CookedPC\*.dzip" -> ok handle=02D76758 name="abetterui.dzip" size=61353315 (err 2)
t=+266ms  FindNextFileW handle=02D76758 pattern="<game>\CookedPC\*.dzip" name="base_scripts.dzip" size=1904972 -> 1 (err 2)
t=+641ms  CreateFileW "<game>\CookedPC\base_scripts.dzip" access=80000000 share=00000003 disp=3 flags=00000080 -> ok handle=000002BC size=1904972 (err 0)
t=+1047ms GetFileAttributesW "<game>\CookedPC\base_scripts.dzip" -> 00000020 (err 0)
t=+1047ms CreateFileW "<game>\CookedPC\base_scripts.dzip" access=80000000 share=00000003 disp=3 flags=00000080 -> ok handle=000004F0 size=1904972 (err 0)
```

## A UI movie (merchant, t=+44.8 s)

```
t=+44797ms FindFirstFileExW "globals\gui\ui_trade.swf" level=0 search=0 flags=00000000 -> FAILED handle=FFFFFFFF name="" size=-1 (err 3)
t=+44797ms CreateFileW "<game>\CookedPC\globals\gui\ui_trade.swf" full="" access=80000000 share=00000001 disp=3 flags=08000000 -> ok handle=00001064 size=2207423 (err 0)
t=+44797ms GetFileSize handle=00001064 "<game>\CookedPC\globals\gui\ui_trade.swf" -> 2207423 high=0 (err 0)
t=+44797ms GetFileSize handle=00001064 "<game>\CookedPC\globals\gui\ui_trade.swf" -> 2207423 high=0 (err 0)
```

`err 3` is `ERROR_PATH_NOT_FOUND`: the probe uses a path relative to the CWD, which is the
game root, where no `globals\` exists. `flags=08000000` is `FILE_FLAG_SEQUENTIAL_SCAN`.

## An icon (inventory, t=+32.3 s)

```
t=+32250ms CreateFileW "<game>\CookedPC\globals\gui\icons\items\w2ef_read\glossaryscoiatael_64x64.dds" full="" access=80000000 share=00000001 disp=3 flags=08000000 -> ok handle=00000A98 size=16512 (err 0)
t=+32250ms GetFileSize handle=00000A98 "<game>\CookedPC\globals\gui\icons\items\w2ef_read\glossaryscoiatael_64x64.dds" -> 16512 high=0 (err 0)
```

## Calls in the session

| API | lines |
|---|---|
| `FindFirstFileW` | 11 341 |
| `GetFileSize` | 2 114 |
| `CreateFileW` | 1 089 |
| `FindNextFileW` (only `.dzip`/`.swf` names are logged) | 104 |
| `FindFirstFileExW` | 53 |
| `GetFileAttributesW` | 11 |

Never called for a target path: `CreateFileA`, `CreateFileMappingW`, `GetFileAttributesA`,
`GetFileAttributesExW`, `GetFileInformationByHandle`, `GetFileSizeEx`, `FindFirstFileA`,
`FindNextFileA`.

## Session

Main menu `t=+17.2s`; loadingscreen `CookedPC\globals\gui\loadingscreens\ls_akt1.swf`
`t=+24.0s`; `ui_trade.swf` `t=+44.8s`; `ui_inventory.swf` `t=+51.4s`; `ui_mainmenu.swf`
`t=+59.0s`; `unloaded` `t=+62.5s`. `ui_storage.swf` (1 451 496 bytes loose) is listed by the
startup walk but was not opened in this session.

## Reproduce

Copy `build\dinput8.dll` into `<game>\bin`, set `[diagnostics] file_trace=1` in
`<game>\bin\witcher2_enginefix.ini`, start the game, play, quit, read
`<game>\bin\witcher2_enginefix.log`. The trace is passive and capped at 100 000 lines. The
first run (18:17) hit an earlier 5 000-line cap during the startup walk; its startup half is
recorded in `docs/fixes/dll-script-patches.md`.
