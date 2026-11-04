# Script patches applied by the DLL

Status: Tasks 1-7 of the plan for this work implemented 2026-10-31 on
branch `dll-script-patches`. Task 1 traced file access passively; Task 5 added the offline script
edit engine; Task 6 builds the patched files into a cache folder and proves the result equal to
`ScriptPatches.ps1`; Task 7 hooks the game's file opens and serves the cache. The owner's start
test and in-game check (Task 7 Steps 2-3) are still pending. Target: Steam Enhanced Edition
3.5.0.26.

0.7.0 adds a fourth feature, `window_mode` (3 rows, `src/scriptpatches/window_*.ws`): the
"Full screen" and "Borderless window" rows in Options > Graphics. It is unrelated to the file
redirect described here; see [borderless-window.md](borderless-window.md). The edit table is now
17 rows and `OV_CACHE_LAYOUT` is 3.

## Why

Up to 0.5.1 the optional script patches (BookMarker, Continue, PauseQuit) are applied by
`tools/script-patches/ScriptPatches.ps1`, started from `Install.bat` with
`-ExecutionPolicy Bypass`; the script compiles ~380 lines of C# at run time, writes the game's
files and keeps a backup. Nexus quarantined 0.5.0/0.5.1: BitDefender's script heuristic
`Heur.BZC.PZQ.Boxter` on the PowerShell patcher.

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

Since 0.10.0 the DLL finds these slots by name in the running exe's import directory
(`k_imports` in `src/enginefix.c`), which gives the same slots on Steam and GOG's own on GOG.
The addresses below are Steam's.

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

## Script edit engine (Task 5)

`src/sp_script.c` is the C port of `ScriptPatches.ps1`'s `Add-Edit`, `ConvertTo-Tabs`,
`Get-CollectBody` and `New-ReadIconFunction`; the patch data moved out of the script into
`src/scriptpatches/` and is embedded as RCDATA, so neither `dinput8.dll` nor `build\sptest.exe`
needs a file next to it:

| Piece | Where | Notes |
|---|---|---|
| edit table, 14 rows, `$FeatureList` order | `src/scriptpatches/edits.txt` | tab-separated; a row is one `Add-Edit` call |
| fragments (`book_helpers.ws`, `continue_*.ws`, `quit_*.ws`, `inventory_refill.ws`) | `src/scriptpatches/` | the `.ps1` here-strings verbatim (four-space indentation, no trailing newline) |
| `book_texts.ws` | `src/scriptpatches/` | generated once by `New-TextFunction` from `$BookTexts` (12 languages), no trailing newline; it is inserted mid-block, so an added newline would change the game script |
| resource IDs | `src/sp_script_ids.h` | shared by the `.rc` and the C code; rc does not accept the file names themselves as resource names |

Behaviour, matching the reference:

* An anchor is the game's script text as a literal. Since `edits.txt` is not regex, every run
  of whitespace in the anchor matches any run of whitespace in the script - that covers the
  escaped literals plus the three regex anchors (`$PreviewIcon`'s tab-aligned line,
  `$InputMainMenu + "\s*\{"`, `$InputMainInGame + "\s*\{"`). Anchors must not begin or end with
  whitespace (the loader refuses that), because a leading run would make the reference's
  literal whitespace match more than it does.
* `Before`, `After`, `Body`, `Replace` and `End` do exactly what `Add-Edit` does, including the
  begin/end marker lines, the file's own CRLF or LF, indentation taken from the line the code
  lands at, and the blank-line handling that differs between `Replace` and the other
  positions.
* All or nothing per feature: a feature whose anchor is missing or matches the wrong number of
  times leaves every text unchanged and reports the first failing anchor; the other features
  still apply. A text that already carries that feature's marker is refused, so features can
  be applied one after another to the same file while a second run cannot double-patch. The
  `.ps1` refuses *any* marker in the archive before installing; the overlay will do that check
  once on the source archive (Task 6).
* `sp_script_collect_body` is `Get-CollectBody`: `CGuiInventory.AddBookKnowledge`'s body with
  `thePlayer.MaxKnowledgeAccumulator(`/`thePlayer.AddJournalEntry(` redirected, CRLF to LF,
  and the same refusals (found twice, touches `thePlayer` elsewhere, grants nothing known).

### Verification (2026-10-31)

`build\sptest.exe test --game "<game>"`: 20 tests, 0 failures. The new offline tests cover each
`where`, Replace with Count 2, blank lines, CRLF files, a tab-aligned anchor, the failure cases
(0 or 2 matches, mid-line anchor, anchor on the last line), the all-or-nothing restore, the two
features on one file and the double-apply guard, the text/read-icon substitutions, and
`Get-CollectBody` on a sample. Expected outputs were produced by the reference `Add-Edit` with
the same inputs (a scratch script, not committed).

`script_real` applies all three features to the five real scripts of the 3.5.0.26 install
(taken from the old patcher's unpatched backup) and checks the block count per entry
(`guiutils.ws` 2, `guishop.ws` 1, `guimainmenu.ws` 6, `container_class.ws` 6,
`guiinventory.ws` 2).

Byte equality with the reference was checked once on this machine: the same five entries,
patched by `ScriptPatches.ps1`'s own `Add-Edit`/`BookHelpers`/`New-TextFunction`/
`New-ReadIconFunction` with the same 14 rows and read-icon book list (Latin-1 in and out), are
identical to this port's output - 224 019 bytes over the five files. That is a local check, not
a committed test; Task 6's `golden.py` makes it a permanent one at archive level.

## Cache and overlay build (Task 6)

`src/sp_overlay.c` ports the install flow: it reads the game's own files and writes the patched
copies into `cache_dir` (decision 2: `<game>\bin\witcher2_enginefix_cache\`). Nothing under the
game directory is written.

| Cache file | What it is |
|---|---|
| `base_scripts.dzip` | the script archive with the 5 patched entries, nothing else |
| `dlc_finishers.dzip` (the carrier) | that DLC archive's own entry plus the 152 read-book icons and any patched movie that only exists inside `abetterui.dzip` |
| `ui_trade.swf`, `ui_storage.swf` | patched uncompressed (FWS) copies, only when the game has that movie loose |
| `cache.key` | the input key; written last, so an interrupted build is rebuilt next start |
| `cache.status` | the per-feature outcome, for the log line on a cache hit |

**Branch B is implemented:** Task 1 showed that the engine never opens a loose file it did not
see in its startup walk, so a file the DLL invents would be invisible. The generated icons
(`globals\gui\icons\items\w2ef_read\*.dds`) and a patched movie that exists only in
`abetterui.dzip` therefore become entries of a redirected archive; loose movies are served
through a `CreateFileW` redirect to the cached copy instead.

**Not in `base_scripts.dzip`.** The first build put the icons into the redirected
`base_scripts.dzip`. The owner's start test (2026-10-31) stopped at the game's *Script
compilation error* dialog, one line per icon:
`globals\gui\icons\items\w2ef_read\bookofarachases_64x64.dds[1]: parse error, near 'DDS'`.
The engine compiles **every** entry of
that archive as WitcherScript, whatever its extension. The golden test could not see it (it
compares bytes, not where they live), so it now fails when `base_scripts.dzip` holds any entry
the reference archive does not.

**The carrier.** The icons go into a cached copy of a tiny DLC archive that every Enhanced
Edition install has, redirected like `base_scripts.dzip`: the first present of
`dlc_finishers.dzip` (one 128-byte archive, one `.csv`), `alchemy_suit.dzip`,
`magical_suit.dzip`, `roche_jacket.dzip`, `swordsman_suit.dzip`. Its own entry keeps its bytes
and place; the icons are added at the end. All 23 archives are mounted by the same startup loop
(Task 1), so the carrier is a resource archive like any other, and the `w2ef_read` names are
unique, so archive precedence does not matter for them. A patched movie that exists only in
`abetterui.dzip` also goes there; `dlc_finishers` sorts after `abetterui`, so under the
reference's "last archive in name order wins" assumption ours is the one loaded (unverified
in game: the owner's install has both movies loose). No carrier present: no icons and no
read-icon list in the scripts, so they never name an icon that does not exist; the status
texts keep working.

**Source selection** matches `Get-Target`/`Get-ReadIcons`: a loose file under `CookedPC` wins,
then the last archive in case-insensitive name order that holds the entry. Def files are the
archives' `(^|\\)def_[^\\]*\.xml$` entries (66 on this install), sorted case-insensitively and
deduplicated; books are the items tagged `SortTypeBook` with a usable icon, in first-seen order,
so the generated `W2EF_HasReadIcon` lists them exactly as the reference does.

**Cache key:** DLL version, a cache layout number (`OV_CACHE_LAYOUT`, 2 since the carrier;
bump it whenever the cache's contents change, or a rebuilt DLL of the same version serves the
old cache), the three feature flags, then size and last-write time of every
`CookedPC\*.dzip` and of the two loose movies (or `missing`). A matching key reuses the files
and logs `cache: reused`; a cached file that has gone missing is not redirected (the game gets
the originals) rather than serving a path that does not exist.

**Guard:** when any of the five script entries already carries
`//===witcher2-enginefix===`, the overlay logs `script patches off - run the old Uninstall.bat
once` and serves nothing; Task 8 replaces that with the automatic restore. Movies are patched
only when BookMarker's script edits applied, so the UI never gets the tick no-op without the
script side.

**File-backed DZIP reader:** `sp_dzip_file_open`/`sp_dzip_file_extract` (Task 6) read the header
and the entry table from disk and one entry's 64 KiB blocks on demand; nothing loads
`pack0.dzip` (10.5 GB). Both the icons and the tests use it; `build\sptest.exe test` now also
compares the file-backed decoder with the in-memory one over all 23 archives (`dzip_file`).

### Golden test

**Removed in 0.6.0 together with the reference** (`ScriptPatches.ps1`, `Install.bat`,
`Uninstall.bat`; plan Task 9). It last passed at commit `397b0dc` (Task 8): `GOLDEN OK` with the
migration check. To run it again, restore `tools/patchtest/golden.py` and
`tools/script-patches/ScriptPatches.ps1` from that commit (`git show 397b0dc:<path>`). What it
did:

`tools/patchtest/golden.py "<game>" "<scratch>"` makes two identical scratch game layouts (the
old patcher's unpatched backup for `base_scripts.dzip` and the loose movies, hard links for the
other 22 archives, so `<scratch>` must be on the game's volume), runs the reference
`ScriptPatches.ps1 -Action Install -Features All` on one and `build\sptest.exe build` on the
other, then compares with an independent Python DZIP/LZF reader (table hash verified):

* every entry of `base_scripts.dzip` decompressed: **460 entries identical**;
* `base_scripts.dzip` holds no entry the reference's has not (the start-test regression);
* the read-book icons: **152 identical**, name and bytes (the carrier's entries against the
  reference's loose files, both directions);
* `ui_trade.swf` (2 207 423 bytes) and `ui_storage.swf` (1 451 496 bytes): **identical**;
* a second build on unchanged inputs: `cache: reused`, cache files untouched (sizes and
  timestamps).

Result on 2026-10-31: `GOLDEN OK` on the owner's install. The reference inputs were the old
patcher's backup, i.e. **Project Mersey's `base_scripts.dzip` plus HD UI's two movies**; no
vanilla `base_scripts.dzip` was at hand on this machine, so the vanilla case still rests on the
dry runs recorded in `docs/fixes/shop-book-read-marker.md`. Overlay build time on this machine:
~110 ms with all three features (the archive open reads 23 tables including `pack0.dzip`'s), and
16 ms when the cache is reused.

## Redirect in the DLL (Task 7)

`src/sp_hooks.c` holds the redirect state and the build trigger; the IAT plumbing stays in
`src/enginefix.c`, whose existing `hook_CreateFileW` / `hook_FindFirstFileW` /
`hook_FindNextFileW` chain save_trace, file_trace and this redirect in one slot each (one slot
must never be patched by two owners).

* **ini.** `[scriptpatches] book_marker`, `continue`, `pause_quit`, each default 1 (decision 1);
  `[general] enabled=0` keeps turning everything off. Written to
  `dist-files\witcher2_enginefix.ini` next to the other sections; an existing user ini without
  the section gets the defaults.
* **Install.** `scripthooks_thread` starts from `DllMain` when a feature is on, polls every 1 ms
  until the `CreateFileW` slot holds the kernel32 export (the Task 1 pattern), pins the module
  with `GetModuleHandleExA(..._PIN)` like the save hooks, and patches three slots:
  `CreateFileW` (`00CAD320`), `FindFirstFileW` (`00CAD26C`), `FindNextFileW` (`00CAD264`).
  `patch_iat` refuses a slot that does not hold the expected export, so a wrong constant fails
  loudly instead of mispatching. If `CreateFileW` cannot be hooked, the features switch
  themselves off.
* **Redirect.** `hook_CreateFileW` normalises the path (`GetFullPathNameW`; absolute paths that
  do not contain `CookedPC` are skipped without a call), asks `sp_hooks_path` and opens the
  cached copy instead - same access/share/flags, so still read-only. A cache open that fails
  falls back to the original path and logs why. Every redirect is logged (first 8):
  `script patches: CreateFileW "<game>\CookedPC\base_scripts.dzip" -> cached copy`.
* **Build trigger.** The overlay is built on the first hit of a target, under a critical
  section, once per session: normally the game's startup `FindFirstFileW
  "<game>\CookedPC\*.dzip"` (its find data carries the archives' sizes), or the first
  `CreateFileW` of a `CookedPC\*.dzip` or a loose movie if that listing never happens. The game waits
  for that one build (~110 ms; later starts reuse the cache in ~16 ms).
* **Find data.** For an entry named like a redirected archive (`base_scripts.dzip`, the
  carrier) `FindFirstFileW`/`FindNextFileW` return the cached file's attributes, sizes and
  times, name kept: the engine must see the archive it is
  about to open, and its "have the scripts changed?" check reads that timestamp. No attribute
  or size query needs a hook - Task 1 showed `GetFileAttributesW` carries no size and
  `GetFileSize` follows the handle.
* **Log line.** Once per session, from the build: `script patches: book_marker=on
  continue=on pause_quit=on cache=built 110 ms` (`skipped: <why>` per feature, `cache=reused`
  on later starts). `cache.status` in the cache folder remembers the per-feature outcome so a
  cache hit reports the same states instead of guessing.

Offline verification (2026-10-31): `cmd /c build.bat` builds the DLL with the script code,
`build\sptest.exe test --game` still passes (22 tests) and `golden.py` still prints `GOLDEN
OK`. A local harness (not committed) linked `src/sp_*.c` against the golden scratch layout and
confirmed: the `*.dzip` glob triggers the build, the `base_scripts.dzip` find data reports the
cached archive's size (4 508 633 bytes), the archive and both loose movies resolve to their
cached copies, other archives and files outside `CookedPC` stay untouched, matching is
case-insensitive, and the second session reuses the cache.

**Start test 1 (2026-10-31, owner).** Hooks in after 0 ms (3/3 slots), overlay built in 125 ms
with all three features `on`, both opens of `base_scripts.dzip` served from the cache, archive
accepted with its new size - then the script compilation error above (icons in
`base_scripts.dzip`). Fixed with the carrier; offline after the fix: 22 tests pass, `GOLDEN OK`
(460 entries, 152 icons in `dlc_finishers.dzip`, both movies), `base_scripts.dzip` holds only
the 5 `.ws` changes, the carrier keeps its `.csv` byte for byte.

**Start test 2 (2026-10-31, owner, commit `d1dcb26`)** passed: main menu, cache built in
109 ms, all three features on, `base_scripts.dzip`, `dlc_finishers.dzip` and `ui_trade.swf`
served from the cache; book markers, menu buttons and saving checked in game. The rest of
Step 3 (storage, each ini key `0`, deleting the DLL) is bundled with Task 8's owner check just
before the release.

## Migration from the old on-disk patches (Task 8)

Owner decision 3: the DLL cleans up a 0.4/0.5 install itself, so a user who once ran the old
`Install.bat` does not have to run `Uninstall.bat` first (the overlay refuses an archive that
already carries our markers, so without this they would get the old patches forever).

`src/sp_migrate.c` ports `Read-Manifest` and `Restore-Leaf` from `ScriptPatches.ps1`. The
manifest (`<game>\Witcher2EngineFix_Optional\ScriptPatches\backup\manifest.txt`, the same
`leaf<TAB>mode<TAB>sha256` format in 0.4 and 0.5) decides what is written and deleted in the
game folder, so only the four leaves that patcher ever wrote are accepted:

| Leaf | Mode | Path under `CookedPC` |
|---|---|---|
| `base_scripts.dzip` | replaced | `base_scripts.dzip` |
| `ui_trade.swf`, `ui_storage.swf` | replaced or created | `globals\gui\<leaf>` |
| `w2ef_read` | folder | `globals\gui\icons\items\w2ef_read` |

Per leaf, as `Restore-Leaf`: a missing file is skipped; the icon folder loses its files and is
removed; a file whose SHA-256 still equals the manifest's is restored from the backup
(`sp_write_file_atomic`) or, when the old patcher created it, deleted. A file that equals its
backup copy already counts as restored (a retry after a failed rename). Anything else - a file
another mod or an update changed since, a leaf outside the table - is left as it is and logged.

Differences from the reference: the backup is renamed to `backup.restored-by-dll` (a numbered
variant if that exists) instead of deleted, so unrestored originals stay available and the
migration runs once; an I/O error keeps the backup so the next start retries. SHA-256 comes from
`BCryptCreateHash`/`BCryptHashData`/`BCryptFinishHash` (Windows 7 has no `BCryptHash`).

**When it runs.** In `sp_hooks.c`'s `build_once`, before the overlay build, under the build
lock. `hook_FindFirstFileW` calls `sp_hooks_prepare` before the original `FindFirstFileW`, so
for the game's first `CookedPC` access (the `*.dzip` listing at ~+235 ms, Task 1) the files are
restored and the overlay is built before the game lists or opens anything. Restored but
partial, or failed: `script patches: off for this start - ...`, no redirects at all (a changed
file may still hold our markers; the overlay's guard would refuse it too). The next start finds
no manifest and builds normally. When every feature is `0` the hooks are not installed and
nothing is migrated.

Log on a full migration:

```
migrate: old script patches found (<game>\Witcher2EngineFix_Optional\ScriptPatches\backup, 4 entries)
migrate: restored base_scripts.dzip
migrate: restored ui_trade.swf
migrate: restored ui_storage.swf
migrate: removed the read-book icons (152 files)
migrate: restored 3 files, removed w2ef_read
migrate: old backup kept as <game>\Witcher2EngineFix_Optional\ScriptPatches\backup.restored-by-dll
```

Offline verification (2026-10-31): `sptest` test `migrate` (full restore, a second old install,
already restored, a changed file, a leaf outside the table, no manifest, SHA-256 test vector);
23 tests pass. `golden.py` now also runs `sptest migrate` on the reference's patched layout with
the reference's own backup: the three files equal the originals byte for byte, the 152 icons and
their folder are gone, the backup is renamed - `GOLDEN OK`. The owner check on the real install
(plan Task 8 Step 5) is pending.

## User content packages (0.9.0)

A player got "Script compilation error" naming `W2EF_LootName` and `W2EF_ReadIcon` in our patched
`container_class.ws`: the `guiutils.ws` the game compiled was not ours. The overlay only looked at
`CookedPC`, so this section maps every place the game takes a script from. Read statically from
the Steam 3.5.0.26 unpacked exe (GOG not checked); names below are the ones given in the Ghidra
program.

### Where scripts come from

`CompileScriptsFromSources` (`00AA69A0`) takes the release branch (`DAT_01608613 == 0`, never
written, and `DAT_01608659 == 1`, set at `00AC8B72` by the game's main before engine init):

1. `<game>\CookedPC\base_scripts.dzip`, by exact name (`00AA69E4`). No other `CookedPC` archive
   and no loose `.ws` is a script source.
2. Then, for each mounted user content package in mount order, `<package>\scripts.dzip` if
   `GetFileAttributesW` finds it (`GetUserPackageScriptsSource`, `00ABED95`; the string at
   `00E5BAF8`).

`ListScriptSourcesFiles` (`00414C6E`) lists every name once (lowercased A-Z, add-unique), and
`ReadScriptFromLastSource` (`00414CC9`) reads it from the **last** source that has it: it walks
the list backwards (`MOV ESI,[EDI+8]` ... `CALL [EAX+0xC]` ... `DEC ESI; JNZ`). So the last
package in mount order wins, then earlier packages, then `base_scripts.dzip`. Inside one archive
the name map keeps the **last** duplicate (`0041495C`, `004149E7: MOV ECX,[EBP+8]; MOV
[EAX+0x10],ECX` overwrites the stored index). A package entry with another prefix or `/`
separators is a different name, so both copies would compile.

### Which packages are mounted

`MountUserContentPackages` (`00ABF177`) runs at "Initializing user content manager...", before
the depot scan:

* The folder is `<Documents>\Witcher 2\UserContent\` (`SHGetFolderPathW(CSIDL_PERSONAL)` at
  `00932589`, `\UserContent\` at `00E5BBA4`); the game's string has a doubled backslash,
  `...\Witcher 2\\UserContent\<name>\`.
* The list is `<Documents>\Witcher 2\Config\UserContent.ini`, section `[Packages]`, every
  `Mount=` value in file order (`ReadUserContentMountList`, `00407854`). The launcher's
  `bin\userContentManager.exe` writes it (`_wfopen(..., L"w")`, text mode, so ANSI).
* How the game reads that ini, which decides what is mounted:
  * Decoding (`00AC6AAE`): UTF-16LE only when the file is at least 3 bytes and starts with
    `FF FE`; anything else through `setlocale(LC_CTYPE, "")` and `mbstowcs`, i.e. the ANSI code
    page, up to the first NUL (so UTF-16 without a BOM reads as `[`).
  * Parsing (`00AC1D74`): between entries every character outside 0x21..0x7F is skipped (blanks,
    line ends of any kind, a UTF-8 BOM read as ANSI). `[` starts a section that ends at `]`; a
    character below 0x21 inside it, or an empty name, is a parse error. Otherwise a key runs to
    the first `=` and the value to the first character below 0x20 (a tab ends it); nothing is
    trimmed, so `Mount = x` is the key `Mount ` and `Mount= x` the value ` x`. A line without
    `=` is a key with an empty value (so `;` lines are keys, not comments). An empty key or a
    key before any section is a parse error.
  * A parse error stops reading, and what was read before it stays (`FUN_00ac2020` ignores the
    result). Section and key names compare with a plain `wcscmp` (`0040190C`): `[packages]` or
    `mount=` do not count.
  * `FUN_00ac2020` first reads a "base" copy of the same file from a path with a `base` part
    added; neither install has such a folder. Not followed further.
* A listed package mounts only when `<folder>\pack0.dzip` exists (`FileExistsW`, `00AC0F87`). A
  folder that is not listed is never looked at: the game does not list `UserContent`.
* Files of a package: `pack0.dzip` (required; resources, scanned for a `.w2w` level),
  `scripts.dzip` (optional), `config.ini` (`[Configuration]` `Name=`/`Description=`),
  `screen.png`.

A package's `pack0.dzip` is mounted after every `CookedPC` archive with the override flag
(`MountDepotArchives`, `00B1B1B9`; `AddDepotDirFile`, `00B1A1FC` replaces an entry only with flag
1), so its resources also win over `CookedPC`, and between `CookedPC` archives the later mounted
one wins. That answers Q5's archive-vs-archive question.

The dialog text "Maybe some selected user packages are incompatible?" (`00E45F98`, shown at
`00AA6B81`) is fixed: it appears on every script compile failure, with or without packages.

### File I/O

Both archives are opened with CRT `_wopen` (`OpenDzipHandle`, `0068E5B0`), which ends in
`CreateFileW` through the exe's IAT slot `00CAD320`, and read with `_read` on that handle. No
`FindFirstFile*` on package folders and no file mapping, so the existing `CreateFileW` redirect
covers a package's `scripts.dzip`. It is opened at compile time (~+1 s), after the overlay
build (triggered by the `CookedPC\*.dzip` listing at ~+235 ms).

### What the overlay does

* `read_packages` reads the same ini with the game's rules above (`sp_user_content_mounts`,
  tested case by case in `sptest`'s `usercontent_ini`) and keeps each `Mount=` whose
  `pack0.dzip` exists, in order. A more lenient reader would patch a copy the game does not
  compile, which brings back exactly the compile error this fixes. `sp_user_root` gets the
  Documents folder the way the game does.
* A mounted package the overlay cannot check (more than 16, or a path too long) may hold the
  copy that wins, so the script patches are off for that start instead of guessing.
* For each script entry it patches, the text is taken from the copy the game compiles: the last
  package whose `scripts.dzip` holds the name, else `base_scripts.dzip`; inside an archive the
  last entry of that name (`sp_dzip_find_last`). The edits are applied to that text, so a feature
  still applies all or nothing across the copies actually compiled.
* A patched entry from a package goes into a cached copy of that package's `scripts.dzip`
  (`user<k>_scripts.dzip`, k = position among mounted packages), redirected like
  `base_scripts.dzip`. `sp_dzip_write` now writes a replaced entry into every entry of that
  name, so a duplicate the game would use is patched too.
* The cache key lists each mounted package with its `scripts.dzip` size and time, so adding,
  removing or changing one rebuilds the cache (`OV_CACHE_LAYOUT` 9).
* A package `scripts.dzip` that cannot be read as DZIP v2 turns all script features off for that
  start, because what it replaces is unknown.
* When a package's `pack0.dzip` holds `ui_trade.swf` or `ui_storage.swf`, its copy is the one
  shown and the log says so; the scripts still work.
* The DLL's `CreateFileW` hook also passes absolute paths containing `\UserContent\` to the
  redirect, and the redirect compares paths with repeated backslashes collapsed
  (`sp_normalize_path`).
* The cached archives belong together: a patched `extendedmovable.ws` in `base_scripts.dzip`
  calls into a patched `player.ws` that may sit in a package's copy. If a cached copy does not
  open, the hook retries four times 50 ms apart (a scanner reading the new file) before it
  serves the original; that one fallback could still mix patched and unpatched files.

Log lines with a package mounted:

```
script overlay: user content 'w2ef_test': mounted, has scripts.dzip
script overlay: game\gui\guiutils.ws: the game compiles the copy in user content 'w2ef_test' - patching that one
script overlay: user content 'w2ef_test': scripts.dzip served as user0_scripts.dzip
```

Without an ini: `script overlay: user content: no Config\UserContent.ini, none mounted`.

### Verification

Offline (2026-11-04, Steam and GOG installs): `sptest` tests `dzip_duplicates` (last duplicate
found, every duplicate replaced), `normalize_path`, `usercontent_ini` (21 inputs against the
game's reader: spaces around `=`, case, CR-only lines, tab, text after `]`, parse errors, UTF-8
and UTF-16 with and without BOM), and `user_content`: a fake Documents folder whose ini mounts
two packages holding `guiutils.ws` (the second also `player.ws`), one without `pack0.dzip`, and
names a complete package as ` Mount = w2ef_unlisted `, which the game does not mount. Only the
last package's copies are patched and redirected, the spaced one is ignored, a second build
reuses the cache, the second package's `player.ws` without the Medallion anchor drops the whole
Medallion feature (no `W2EF_UseMedallion` call left in `extendedmovable.ws`), with the ini gone
`guiutils.ws` is patched in `base_scripts.dzip` again, and 17 mounted packages switch the script
patches off with nothing redirected.

In game: `tools\usercontent-test.ps1 -Game "<game>"` adds a package `w2ef_test` with the install's
own `guiutils.ws` and `player.ws` (one comment line added) and mounts it; `-Remove` takes it away.
`sptest pack` and `sptest extract` build it. Result (2026-11-04, owner): the build from before the
change stops at "Script compilation error" naming `W2EF_UseMedallion` in `extendedmovable.ws`
(the package's `player.ws` won); the new build starts, patches and serves the package's copies,
and the read status and the medallion work. See `docs/validation/usercontent-2026-11-04.md`.
