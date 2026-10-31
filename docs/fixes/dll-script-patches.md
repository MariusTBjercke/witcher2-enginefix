# Script patches applied by the DLL

Status: Task 1 (research spike) of
`docs/plans/2026-10-31-dll-script-patches.md`, done 2026-10-31 on branch `dll-script-patches`.
Passive tracing only; nothing is redirected yet. Target: Steam Enhanced Edition 3.5.0.26.

## Why

Up to 0.5.1 the optional script patches (BookMarker, Continue, PauseQuit) are applied by
`tools/script-patches/ScriptPatches.ps1`, started from `Install.bat` with
`-ExecutionPolicy Bypass`; the script compiles ~380 lines of C# at run time, writes the game's
files and keeps a backup. Nexus quarantined 0.5.0/0.5.1: BitDefender's script heuristic
`Heur.BZC.PZQ.Boxter` on the PowerShell patcher (see `docs/local/nexus.md`).

The plan moves the work into `dinput8.dll`: it builds the patched archives once into
`bin\witcher2_enginefix_cache\` and redirects the game's file opens to the cached copies. No
game file is written, the release ships no `.bat`/`.ps1`, uninstalling is deleting the DLL.

## Questions this spike answers

* **Q1** - Does the game open `CookedPC\*.dzip` and loose files with `CreateFileW` from
  `witcher2.exe`'s own IAT, and with which path form (absolute, relative, `\` or `/`)?
* **Q2** - Can the hook be in place before the first open of `base_scripts.dzip`? (The
  SteamStub fills the IAT after `DllMain`, so the worker's fixed 1.5 s delay is too late.)
* **Q3** - Which file APIs report sizes and times? A redirected file has a different size, so
  every call that reports one for a target has to be redirected too.
* **Q4** - Does the game discover loose files under `CookedPC` by enumeration
  (`FindFirstFile*`) or by trying `CreateFileW` per lookup? That decides whether generated
  icons can be new files (per-lookup, *branch A*) or must become entries of a redirected
  archive (*branch B*).
* **Q5** (branch B only) - When two archives hold the same entry name, which one wins?
* **Q6** - Are the dzips memory-mapped through `CreateFileMappingW` on the opened handle?

## IAT slots

Computed from the import table of the unpacked 3.5.0.26 binary and re-checked against
`tools/ghidra-inputs/witcher2.exe.unpacked.exe` on 2026-10-31: every address below holds
exactly the named export in the `KERNEL32.dll` descriptor, and `CreateFileW`, `CopyFileW` and
`DeleteFileW` match the constants already in use for `save_trace`.

| Slot | Export | Used for |
|---|---|---|
| `00CAD234` | `CreateFileA` | Q1 (the game imports the A variants too) |
| `00CAD320` | `CreateFileW` | Q1, Q2 (shared with `save_trace`) |
| `00CAD0B4` | `CreateFileMappingW` | Q6 |
| `00CAD0BC` | `GetFileAttributesExW` | Q3 |
| `00CAD1D8` | `GetFileAttributesA` | Q3 |
| `00CAD318` | `GetFileAttributesW` | Q3 |
| `00CAD0A8` | `GetFileInformationByHandle` | Q3 (size and times of an open handle) |
| `00CAD1DC` | `FindFirstFileA` | Q4 |
| `00CAD26C` | `FindFirstFileW` | Q4 |
| `00CAD198` | `FindFirstFileExW` | Q4 |
| `00CAD1E0` | `FindNextFileA` | Q4 |
| `00CAD264` | `FindNextFileW` | Q4 |
| `00CAD280` | `GetFileSize` | Q3 |
| `00CAD290` | `GetFileSizeEx` | Q3 |

The plan listed only the `W` variants; the unpacked import table also has `CreateFileA`,
`GetFileAttributesA` and `FindFirstFileA`/`FindNextFileA` and `GetFileInformationByHandle`, so
those are traced as well - otherwise "the game does not open X" could be a false negative.

## Trace design (`[diagnostics] file_trace=1`)

`src/enginefix.c`, section "file trace". Passive: every hook calls the real function first and
returns its result; `GetLastError` is saved and restored around the logging.

* **Install.** `filetrace_thread` is started from `DllMain` (the `skip_intro` pattern: create
  the thread there, poll in the thread). It polls every 1 ms until the `CreateFileW` slot holds
  `GetProcAddress(kernel32, "CreateFileW")`, then patches the 14 slots with `patch_iat` (which
  refuses any slot that does not hold the expected export) and logs
  `file-trace: hooked after N ms (K/14 slots), current directory "..."`. It keeps watching the
  slots for 60 s: if something rewrites them, it logs
  `K/14 slots still hooked after N ms - re-hooking` and patches again, so Q2 cannot fail
  silently.
* **`CreateFileW` shares one slot with `save_trace`.** `hook_CreateFileW` does both
  behaviours; the slot is patched once whichever thread installs it first.
* **Filter.** A path is logged when it contains `CookedPC` (any case, either separator) or
  ends in `.dzip`, `.swf` or `.dds`. Relative names also log the full path the process would
  open (Q1). Directory-listing entries (`FindNextFileW`) are logged only for names ending in
  `.dzip` or `.swf`: the install holds ~4 000 loose `.dds` and ~1 900 `.xbm` files and each
  directory is listed twice (`*.` and `*.*`), which would drown the session.
* **Handle correlation.** Successful `CreateFileW` and `FindFirstFile*` handles are remembered
  (256-entry ring) so `GetFileSize`, `GetFileSizeEx`, `CreateFileMappingW` and
  `FindNextFileW` can name the file they refer to (Q3, Q6).
* **Log line.** `HH:MM:SS.mmm file-trace: t=+Nms tid=T <api> ...`: milliseconds since
  `DllMain` make the hook-vs-first-open order (Q2) directly readable. Capped at 100 000 lines,
  with one `file-trace: N lines reached - further lines suppressed` marker. (The plan said
  5 000; the startup loose-file scan alone needs ~4 000 of those before the game reaches the
  main menu, see run 1.)

The ini key is documented in `dist-files/witcher2_enginefix.ini` (`[diagnostics] file_trace`,
default 0).

## Run 1 (2026-10-31 18:17, owner)

The first run hit the 5 000-line cap at t=+704 ms, inside the startup loose-file scan, so it
covers only the first second. It answers the startup half:

* `file-trace: thread started, CreateFileW slot holds 7710EA50` and
  `file-trace: hooked after 0 ms (9/9 slots), current directory "<game>"` (run 1 used the
  nine-slot build, before the A variants were added): the slot already held the kernel32
  export when `DllMain` ran, every slot held the expected export, and the current directory is
  the **game root**, not `bin`.
* First access at `t=+235ms`: `FindFirstFileW "<game>\CookedPC\*.dzip"` -> 23 entries
  (`abetterui.dzip`, `alchemy_suit.dzip`, ... `ZZ_UPatch.dzip`, `z_emc_data.dzip`) with their
  sizes, then `FindFirstFileW "...\CookedPC\*.dlc"` -> fails (no `.dlc` files).
* `GetFileAttributesW "...\CookedPC\pack0.dzip"` -> `00000020`, then
  `CreateFileW "...\CookedPC\pack0.dzip" access=80000000 share=3 disp=3 flags=80 -> ok
  size=10515200574`; at `t=+610ms` `GetFileAttributesW` for `pack1`..`pack9` (all
  `INVALID_FILE_ATTRIBUTES`) and then one `CreateFileW` per `.dzip` in enumeration order.
* At `t=+235ms` the game starts a **recursive enumeration of every directory under
  `CookedPC`**, each one listed twice, `\*.` and `\*.*` (1 337 `FindFirstFileW` calls in the
  first 700 ms; the install has 1 756 directories and 6 795 files there). Run 2 shows what that
  walk is for; see Q4.
* Path form: absolute, backslashes, no `\\?\` prefix, `full=""` (nothing relative).
  `CreateFileW` uses `GENERIC_READ`, `FILE_SHARE_READ|WRITE`, `OPEN_EXISTING`,
  `FILE_ATTRIBUTE_NORMAL`.
* No `GetFileAttributesExW`, `FindFirstFileExW`, `GetFileSize`, `GetFileSizeEx` or
  `CreateFileMappingW` call in that first second; the `.dzip` sizes came from the find data.

## Run 2 (2026-10-31 18:25, owner)

A whole session (load save, inventory, merchant, inventory, quit), 14 760 lines, no cap. All
file calls come from one game thread (`tid=25300`); the loading screens use `tid=22300`.

* **Archives.** `FindFirstFileW "<game>\CookedPC\*.dzip"` at `t=+266ms` lists the 23 archives
  with their sizes (`base_scripts.dzip` 1 904 972). `pack0.dzip` is opened at once with
  `access=80000000 share=00000003 disp=3 flags=00000080` (GENERIC_READ, FILE_SHARE_READ|WRITE,
  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL); the other 22 follow at `t=+641ms` in name order
  (`abetterui.dzip`, `alchemy_suit.dzip`, ... `z_emc_data.dzip`). `base_scripts.dzip` is opened
  **a second time** at `t=+1047ms`, right after `GetFileAttributesW` on it returns `00000020`.
* **UI movies.** Every movie is preceded in the same millisecond by a *relative* existence
  probe that fails, then opened by absolute path:

  ```
  FindFirstFileExW "globals\gui\ui_trade.swf" -> FAILED (err 3, ERROR_PATH_NOT_FOUND)
  CreateFileW "<game>\CookedPC\globals\gui\ui_trade.swf" access=80000000 share=1 disp=3
              flags=08000000 -> ok size=2207423 (FILE_FLAG_SEQUENTIAL_SCAN)
  GetFileSize handle -> 2207423        (twice)
  ```

  The process CWD is the game root, so `globals\` does not exist there; the probe seems to be a
  separate override root and its failure is harmless. Movies that exist only inside an
  archive (`ui_panelbg.swf`, `CScroll.swf`, `clist.swf`, `cfilter.swf`, `fonts.swf`,
  `ui_video.swf`) get only that probe and **no `CreateFileW` at all**.
* **Loose-file walk.** One walk of the whole `CookedPC` tree at `t=+656ms` (top-level
  `CookedPC\*.*`, every directory listed with `*.` and `*.*`; 11 341 `FindFirstFileW` calls in
  the session) lists all 59 loose `.swf` files and the `w2ef_read\*.dds` folder. This walk is
  how the engine knows which files exist loose.
* **Icons.** `CreateFileW "<game>\CookedPC\globals\gui\icons\items\w2ef_read\glossaryscoiatael_64x64.dds"`
  (three times while the inventory was open), `icons\tooltip\waga_15x15.dds`/`orens_15x15.dds`
  and `icons\signs\aard_64x64.dds`, all with `flags=08000000` and a `GetFileSize` per open, and
  no probe before them.
* **Sizes/times.** `GetFileSize` is called twice per loose open (2 114 calls, mostly `.fsb`
  audio and `.xbm` texture streaming) and **never on an archive handle**. `GetFileSizeEx`,
  `GetFileAttributesExW`, `GetFileInformationByHandle` and `CreateFileMappingW` are never
  called for a target path. `GetFileAttributesW` appears only for `pack0`..`pack9` and
  `base_scripts.dzip`, and reports attributes, not sizes.
* **ANSI.** No `CreateFileA`, `GetFileAttributesA`, `FindFirstFileA` or `FindNextFileA` call
  for a target path; everything goes through the `W` variants.
* Session markers: menu `t=+17.2s`, loadingscreen `ls_akt1.swf` `t=+24.0s`, `ui_trade.swf`
  `t=+44.8s` (merchant), `ui_inventory.swf` `t=+51.4s`, `ui_mainmenu.swf` `t=+59.0s`,
  `unloaded` `t=+62.5s`. `ui_storage.swf` exists loose (1 451 496 bytes) and is listed by the
  walk, but was not opened in this run.

## Answers

* **Q1** - Yes: the game opens its data through its own IAT. Archive and file paths are
  absolute with backslashes and no `\\?\` prefix (`<game>\CookedPC\...`), and the CWD is the
  game root, not `bin`. Each resource type has its own access pattern:

  | Opened | access | share | disposition | flags |
  |---|---|---|---|---|
  | `.dzip` | `GENERIC_READ` | `READ\|WRITE` | `OPEN_EXISTING` | `FILE_ATTRIBUTE_NORMAL` |
  | UI movie, icon (`.swf`, `.dds`) | `GENERIC_READ` | `READ` | `OPEN_EXISTING` | `FILE_FLAG_SEQUENTIAL_SCAN` |

  Archives are found with `FindFirstFileW "<game>\CookedPC\*.dzip"` (+ a failing `*.dlc` glob).
  UI movies come with a *relative* `FindFirstFileExW "globals\gui\<name>.swf"` probe first;
  icons have no probe.
* **Q2** - Yes, with room to spare. The import directory of the shipped exe is not encrypted
  and is identical in the packed and the unpacked file: `KERNEL32.dll`'s thunk array is the
  original IAT at VA `0x00CAD070` (all 14 slots above sit inside it). The Windows loader
  therefore fills the original IAT before any `DllMain` notification runs, which is what the
  log shows: `thread started, CreateFileW slot holds 7710EA50`, all slots hooked at
  `t=+0ms`, first `CookedPC` access at `t=+235ms` (run 1) / `t=+266ms` (run 2). The hook does
  not depend on the SteamStub at all and is in place before any game code runs.
* **Q3** - Only two APIs report a size for a target, and only one of them for a path:
  * `FindFirstFileW`/`FindNextFileW` find data: the archive listing at startup, with each
    archive's size. A redirected `base_scripts.dzip` needs its find data redirected.
  * `GetFileSize` on the handle that `CreateFileW` returned, twice per open. This follows the
    redirect automatically; the engine allocates from it, so a redirect may change the size.
  * `GetFileAttributesW` is called for `pack0`..`pack9` and `base_scripts.dzip` but returns no
    size or time (only `FILE_ATTRIBUTE_ARCHIVE` here), so it does not need a size.
  * `GetFileSizeEx`, `GetFileAttributesExW` and `GetFileInformationByHandle` are never called
    for a target path. `SetFilePointer`/`SetFilePointerEx` are imported but were not traced;
    they do not report sizes.
* **Q4** - Both, in this order: the engine builds the loose-file set from the startup walk, and
  then opens a *known-loose* file by absolute path. A movie that is not loose is never looked
  up by path (only the relative probe appears), so a **new** file that exists only in the DLL
  cache would not be found - **Branch B**. Replacing a file that already exists loose
  (`base_scripts.dzip`, `ui_trade.swf`, `ui_storage.swf`, and the `w2ef_read\*.dds` icons while
  the 0.5 patches are still on disk) works through the `CreateFileW` redirect. Some resources
  do use a blind per-path open (`CreateFileW "...\CookedPC\EN_user.w2speech"` fails with
  error 2 and the engine falls back), so a redirect may also answer a path that does not
  exist - but the UI movie and icon loaders cannot be relied on for that.
* **Q5** - Loose beats archive: the 0.5 patcher's loose `ui_trade.swf` is the file the engine
  opens (2 207 423 bytes), and the patched trade tick works in game, so the loose copy is what
  is loaded even though `abetterui.dzip` holds a copy too. Between two archives the trace only
  shows the mount order (alphabetical, `abetterui.dzip` before `base_scripts.dzip`); that order
  matches `ScriptPatches.ps1`'s "last `.dzip` in name order wins" and the community `ZZ_`/`zzzz_`
  mod naming, but archive-vs-archive precedence is not observable from file I/O. Task 6's
  golden test and Task 7's in-game check validate the assumption for the files that matter.
* **Q6** - No: `CreateFileMappingW` is never called. The engine streams from open handles with
  `ReadFile` (`FILE_FLAG_SEQUENTIAL_SCAN`), so a redirect at `CreateFileW` is enough.

## What the next tasks take from this

* Task 7 needs `CreateFileW` (per-path redirect) and `FindFirstFileW`/`FindNextFileW` (find
  data for a redirected archive). `GetFileAttributesW` carries no size, `GetFileSize` follows
  the handle, there is no mapping API and no ANSI variant to cover.
* `base_scripts.dzip` is opened twice at startup; the redirect table matches the full path, so
  both opens are covered without extra work.
* Replacing `base_scripts.dzip`, `ui_trade.swf` and `ui_storage.swf` works through the path
  redirect because those loose files exist and the engine opens them by path.
* The ten generated read icons (which Task 8 deletes from disk) and any movie that lives only
  in `abetterui.dzip` must become entries of the redirected `base_scripts.dzip`, because the
  engine decides "loose or archive" from its startup walk.
* A patched SWF or archive has a different size than the original. That is safe for the movie
  loader (it asks the opened handle with `GetFileSize`), but the find data of a redirected
  archive should still report the cached size.
