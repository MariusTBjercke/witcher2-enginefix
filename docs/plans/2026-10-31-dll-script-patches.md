# Script patches in the DLL - Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `dinput8.dll` applies the optional script patches (BookMarker, Continue, PauseQuit) by itself at game start, so the release ships no `.bat`/`.ps1`, no game file on disk is modified, and uninstalling is deleting the DLL.

**Architecture:** A *virtual overlay*. The DLL hooks the game's file-open imports (`CreateFileW` and the attribute/find calls that report sizes) through the existing IAT-slot mechanism. The first time the game opens a patch target (`CookedPC\base_scripts.dzip`, the trade/storage SWFs), the DLL builds the patched file once into a cache folder, `bin\witcher2_enginefix_cache\`, and from then on hands the game a handle to the cached copy. The originals on disk are never written. The patch logic is a C port of `tools/script-patches/ScriptPatches.ps1`, which remains the reference implementation until the port's output is byte-identical to it (golden tests).

**Tech Stack:** C (MSVC x86, `/W4 /MT`, as `build.bat`), Win32 only, one vendored inflate (zlib license), RC resources for the WitcherScript fragments, a console test harness built from the same sources, Python 3 for the golden comparison.

**Spec:** this plan plus the behaviour already specified and validated in
`docs/fixes/shop-book-read-marker.md` (BookMarker, final form) and
`docs/fixes/menu-buttons.md` (Continue, PauseQuit). `ScriptPatches.ps1` at commit `26237ca`
is the reference for every byte the port produces.

## Why

Nexus quarantined 0.5.0 and 0.5.1. VirusTotal: 7-8/68, almost all BitDefender's script heuristic
`Heur.BZC.PZQ.Boxter` (tags `macro-powershell`, `obfuscated`, `self-delete`). The sibling
Witcher 1 mod ships the same kind of `dinput8.dll` proxy without scripts and gets 1/66. The
difference is `ScriptPatches.ps1`: it compiles ~380 lines of C# at run time (`Add-Type`, which
drops and deletes a DLL in `%TEMP%`) and is started by a `.bat` with `-ExecutionPolicy Bypass`.
See `docs/local/nexus.md` (scan results).

## Global Constraints

- Never modify `<game>\bin\witcher2.exe`, and with this plan never write any file under `<game>\CookedPC\` either (except the one-time migration in Task 8, which only *restores* originals from the old patcher's backup).
- Every new or changed IAT hook needs an owner start test before more work is done (`AGENTS.md`, "Rules"). A wrong signature kills the process silently.
- No live debugger/breakpoints in the game process; passive in-process logging only.
- No machine paths or Steam IDs in tracked files (`<game>`, `<scratch>`, `%USERPROFILE%`).
- Patch only on an exact match: an anchor that is missing or found the wrong number of times skips that feature and logs why. Never serve a half-patched file. A script compile error stops the game, so "not patched" is always the safe fallback.
- Output equals the reference: for the same inputs, the decompressed entries, SWF bytes and icon bytes equal what `ScriptPatches.ps1 -Action Install -Features All` writes (entry FILETIMEs and LZF block layout may differ).
- English in code, comments and docs. Version 0.6.0 (`src/version.h` + `CHANGELOG.md`).
- Third-party code: only the vendored inflate, with its license header kept and a line in `LICENSE`/README.

## Execution notes (read first)

- **Branch:** work on `dll-script-patches`, not `main`; `main` holds the released 0.5.1. Merge only after Task 7's in-game check passes.
- **Hard stops, wait for the owner:** Task 1 Step 4 and Task 7 Steps 2-3 (start tests, a human must press *Launch game*), Task 8 Step 5, Task 9 Step 3 and any push/release. The three decisions below are answered.
- **If Task 1 shows the hook comes too late (Q2) or a size query cannot be redirected (Q3), stop** and report; do not improvise an on-disk patch.
- **Inputs for the golden test** are on the owner's machine: the unpatched originals are in `<game>\Witcher2EngineFix_Optional\ScriptPatches\backup\` (`base_scripts.dzip` = Project Mersey's, `ui_trade.swf`, `ui_storage.swf` = HD UI's), because the live `CookedPC` files carry the 0.5 patches. Never write to `<game>`; `<scratch>` must be on the same volume as `<game>` for hard links.
- The installed game currently has the 0.5 on-disk patches. They stay until Task 7 Step 2 (the owner runs the old `Uninstall.bat`).
- `ScriptPatches.ps1` stays untouched until Task 9; it is the reference.
- Leave the untracked `docs/fixes/world-map-readability.md` alone (unrelated work).
- Commit per task as in `docs/conventions.md` (plain English subject, no prefixes).

## Owner decisions (decided 2026-10-31)

1. **Defaults: all on.** `book_marker=1`, `continue=1`, `pause_quit=1` in the shipped ini (the user gets everything from the DLL; any one can be switched off).
2. **Cache folder:** `<game>\bin\witcher2_enginefix_cache\` (next to the DLL, removed together with it, no write outside the game folder).
3. **Old installs: the DLL cleans them up.** It restores 0.4/0.5 on-disk patches once from `Witcher2EngineFix_Optional\ScriptPatches\backup` (Task 8).

## File structure

| File | Responsibility |
|---|---|
| `src/sp_dzip.c`, `src/sp_dzip.h` | DZIP v2: read table, verify hash, extract entry (LZF), write archive with replaced/added entries (literal-run LZF) |
| `src/sp_swf.c`, `src/sp_swf.h` | SWF: CWS→FWS via inflate, wildcard pattern search, same-length patch, state original/patched/unknown |
| `src/third_party/tinf/` | vendored inflate (`tinflate.c`, `tinf.h`, license) |
| `src/sp_dds.c`, `src/sp_dds.h` | 64x64 icon: decode DXT1/3/5/BGRA, alpha-over, encode 32-bit BGRA DDS |
| `src/sp_script.c`, `src/sp_script.h` | WitcherScript text edits: anchors, Before/After/Body/Replace/End, markers, tabs, indentation, generated functions |
| `src/scriptpatches/*.ws`, `src/scriptpatches/edits.txt` | patch data moved out of the `.ps1`: code fragments and the edit table, embedded as `RCDATA` |
| `src/scriptpatches.rc` | RCDATA list (linked into the DLL and the test harness) |
| `src/sp_overlay.c`, `src/sp_overlay.h` | build step: game dir + features → cache files + redirect table; cache key |
| `src/sp_hooks.c`, `src/sp_hooks.h` | file-API IAT hooks, redirect lookup, early install |
| `src/sp_migrate.c`, `src/sp_migrate.h` | one-time restore of 0.4/0.5 on-disk patches |
| `src/enginefix.c` | ini keys, start-up wiring only |
| `tools/patchtest/patchtest.c`, `tools/patchtest/build.bat` | console harness: unit tests + `build <game> <out>` |
| `tools/patchtest/golden.py` | runs the reference `.ps1` on a scratch copy and compares with the harness output |
| `docs/fixes/dll-script-patches.md` | research results, design, validation (new) |

The `sp_*` files compile into both `dinput8.dll` and `build\sptest.exe`. They take an output
buffer and a log callback and never touch the game process, so everything except Task 1 and
Task 7 is testable offline.

```c
/* src/sp_common.h - shared by all sp_* files */
typedef struct { unsigned char* p; size_t n; } sp_buf;            /* owned, free with sp_free */
typedef void (*sp_log_fn)(const char* fmt, ...);
void   sp_free(sp_buf* b);
int    sp_read_file(const wchar_t* path, sp_buf* out);             /* 1 ok */
int    sp_write_file_atomic(const wchar_t* path, const sp_buf* b); /* tmp + MoveFileExW */
```

---

### Task 1: Research spike - how the game opens its data (owner start test)

The whole design depends on facts not yet known. This task adds passive logging only and
answers them; nothing is redirected yet.

**Files:**
- Modify: `src/enginefix.c` (new `[diagnostics] file_trace=1`, off by default)
- Create: `docs/fixes/dll-script-patches.md` (section "Research")

**Questions:**
- Q1: Does the game open `CookedPC\*.dzip` and loose files with `CreateFileW` from `witcher2.exe`'s IAT? With which path form (absolute, relative, `/` or `\`)?
- Q2: Can the hook be in place before the first open of `base_scripts.dzip`? (The SteamStub fills the IAT at start; the existing `worker` waits 1.5 s, too late.)
- Q3: Does the game learn file sizes/times through `GetFileAttributesExW`, `FindFirstFileW`/`FindFirstFileExW`/`FindNextFileW` or `GetFileSize(Ex)`? A redirected file has a different size, so every call that reports one for a target must be redirected too.
- Q4: Does it discover loose files under `CookedPC` by enumeration (`FindFirstFile*`) or by trying `CreateFileW` per lookup? This decides how the generated icons (new files) reach the game: Branch A (per-lookup: a redirect of a non-existent path works) or Branch B (enumeration: icons and an `abetterui.dzip`-only SWF are added as new entries to the redirected `base_scripts.dzip`, so Q5 matters).
- Q5 (only for Branch B): when two archives hold the same entry name, which wins? `ScriptPatches.ps1` assumes loose file, then the last `.dzip` in name order (`Get-ReadIcons`).
- Q6: Are dzips memory-mapped (`CreateFileMappingW` on the handle)? A redirect at `CreateFileW` covers that, but log it.

IAT slots (unpacked 3.5.0.26, computed from the import table; `CreateFileW`, `CopyFileW`, `DeleteFileW` match the existing constants):

```c
#define IAT_CREATEFILEMAPPINGW 0x00CAD0B4u
#define IAT_GETFILEATTREXW     0x00CAD0BCu
#define IAT_FINDFIRSTFILEEXW   0x00CAD198u
#define IAT_FINDNEXTFILEW      0x00CAD264u
#define IAT_FINDFIRSTFILEW     0x00CAD26Cu
#define IAT_GETFILESIZE        0x00CAD280u
#define IAT_GETFILESIZEEX      0x00CAD290u
#define IAT_GETFILEATTRW       0x00CAD318u
/* IAT_CREATEFILEW 0x00CAD320u already defined */
```

- [x] **Step 1: Early install thread.** Copy the `startvideo_thread` pattern: started from `DllMain` when `file_trace=1`, polls every 1 ms until the `CreateFileW` slot holds `GetProcAddress(kernel32, "CreateFileW")` (the stub has filled the IAT), then calls `patch_iat` for the slots above and logs `file-trace: hooked after N ms`. Reuse `patch_iat` (it already refuses a slot that does not hold the expected export). The existing `save_trace` `CreateFileW` hook must chain: one `hook_CreateFileW` that does both, so the slot is patched once.
- [x] **Step 2: Log hooks.** Each hook calls the original first, then logs one line for paths that contain `\CookedPC\` or end in `.dzip`/`.swf`/`.dds` (case-insensitive): API, path, result (handle/attributes/size), thread id, `GetTickCount()` relative to DLL load. Cap at 5 000 lines. Exact signatures (stdcall, same as the Win32 headers):

```c
typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL   (WINAPI *GetFileAttributesExW_t)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef DWORD  (WINAPI *GetFileAttributesW_t)(LPCWSTR);
typedef HANDLE (WINAPI *FindFirstFileW_t)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE (WINAPI *FindFirstFileExW_t)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
typedef BOOL   (WINAPI *FindNextFileW_t)(HANDLE, LPWIN32_FIND_DATAW);
typedef DWORD  (WINAPI *GetFileSize_t)(HANDLE, LPDWORD);
typedef BOOL   (WINAPI *GetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
typedef HANDLE (WINAPI *CreateFileMappingW_t)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
```

- [x] **Step 3: Build** with `.\build.bat`. Expected: `Built build\dinput8.dll`, no new warnings.
- [x] **Step 4: Owner start test** (required by the hook rule). Owner: install the DLL, set `[diagnostics] file_trace=1`, start the game, load a save, open a container with a book, open a merchant, quit. Expected: the game behaves as before; the log has `file-trace: hooked after N ms` before the first `base_scripts.dzip` line.
- [x] **Step 5: Write up** Q1-Q6 with log excerpts in `docs/fixes/dll-script-patches.md` and record **Branch A or B**. If Q2 fails (hook too late), stop and bring it to the owner: the fallback is to hook from `DirectInput8Create`/earlier or to patch on disk, which changes the plan.
- [x] **Step 6: Commit** `research: file access trace for DLL script patches`.

### Task 2: DZIP reader/writer + test harness

**Files:**
- Create: `src/sp_common.h`, `src/sp_common.c`, `src/sp_dzip.h`, `src/sp_dzip.c`
- Create: `tools/patchtest/patchtest.c`, `tools/patchtest/build.bat`

**Interfaces (produces):**

```c
typedef struct {
    char      name[260];        /* as stored, without the trailing NUL, '\' separators */
    long long time, usize, offset, csize;
} sp_dzip_entry;
typedef struct { sp_buf data; sp_dzip_entry* e; int count; long long table; } sp_dzip;

int  sp_dzip_open(const sp_buf* data, sp_dzip* out);              /* checks magic, version 2, table hash */
int  sp_dzip_find(const sp_dzip* z, const char* name);            /* index, case-insensitive, -1 if none */
int  sp_dzip_extract(const sp_dzip* z, int i, sp_buf* out);        /* LZF, 64 KiB blocks */
unsigned long long sp_dzip_hash(const sp_dzip_entry* e, int count);
/* Replace (name exists) or add (name new, only Branch B) entries; others keep their bytes. */
int  sp_dzip_write(const sp_dzip* z, int n, const char* const* names, const sp_buf* datas,
                   long long filetime, sp_buf* out);
void sp_dzip_close(sp_dzip* z);
```

Port `ReadTable`, `Hash`, `Extract`, `LzfDecompress`, `EntryData`, `ReplaceEntries` from
`ScriptPatches.ps1` lines 56-208 exactly (literal-run LZF encoder: control byte `n-1`, at most
32 bytes per run; the new table goes where the old one started; header +16 table offset,
+24 hash). `filetime` is a parameter (the overlay passes the cache build time) instead of
`DateTime.UtcNow`, so a build is reproducible.

`build\sptest.exe` runs `test` (all unit tests, prints `PASS name`/`FAIL name: why`, exit code =
failures) and later `build`. `tools/patchtest/build.bat` mirrors `build.bat`:
`cl /nologo /O2 /W4 /MT /Isrc tools\patchtest\patchtest.c src\sp_*.c /Fe:build\sptest.exe` (the exe is not called `patchtest.exe`: Windows' UAC installer detection elevates any 32-bit, manifest-less exe whose name contains "patch", so an unattended run blocks on a consent prompt); Task 3
adds `src\third_party\tinf\*.c` and Task 5 adds `build\scriptpatches.res` (plus `user32.lib` for
the resource API) to that line.

- [x] **Step 1: Failing tests** in `patchtest.c`:

```c
static void test_dzip_roundtrip(void) {
    /* Build a 2-entry archive with sp_dzip_write from an empty template, read it back. */
    sp_buf a = make_dzip2("a\\x.ws", "hello", "b\\y.ws", big_text(200000)); /* helper in the test */
    sp_dzip z; CHECK(sp_dzip_open(&a, &z));
    CHECK(z.count == 2 && sp_dzip_find(&z, "B\\Y.WS") == 1);
    sp_buf t; CHECK(sp_dzip_extract(&z, 1, &t) && t.n == 200000 && same(t, big_text(200000)));
}
static void test_dzip_real_hash(const wchar_t* game) {
    /* Every CookedPC\*.dzip of the owner's install opens (hash verified) - skipped without --game. */
}
static void test_lzf_backrefs(void) {
    /* A hand-made LZF block with a back-reference (c >= 32, len 7 + extra byte) decodes. */
}
```

- [x] **Step 2:** `tools\patchtest\build.bat` then `build\sptest.exe test` → FAIL (functions missing).
- [x] **Step 3:** Implement `sp_dzip.c` (port as above).
- [x] **Step 4:** `build\sptest.exe test --game "<game>"` → all PASS, including every real `.dzip` (pack0 included: only the table is read).
- [x] **Step 5: Commit** `script patches in DLL: DZIP reader/writer and test harness`.

### Task 3: SWF patching + vendored inflate

**Files:**
- Create: `src/third_party/tinf/` (jibsen/tinf, zlib license; pin the release tag in a `VERSION` line in the folder's `README`)
- Create: `src/sp_swf.h`, `src/sp_swf.c`

**Interfaces (produces):**

```c
typedef struct { const char* name; const short* orig; const short* patched; int len; } sp_swf_patch; /* -1 = wildcard */
enum { SP_SWF_ORIGINAL, SP_SWF_PATCHED, SP_SWF_UNKNOWN };
int sp_swf_uncompressed(const sp_buf* in, sp_buf* out);            /* FWS copy; CWS inflated */
int sp_swf_state(const sp_buf* fws, const sp_swf_patch* p, int n, int* at, char* why, size_t whylen);
int sp_swf_apply(const sp_buf* fws, const sp_swf_patch* p, int n, sp_buf* out); /* only from ORIGINAL */
extern const sp_swf_patch SP_SWF_COMPARE[2];                      /* "player side", "merchant side" */
```

`SP_SWF_COMPARE` is `$SwfCompare` (ScriptPatches.ps1, `$SwfCompare`/`$SwfOwnedName`) copied
byte for byte. The patched SWF is written uncompressed (FWS), as the `.ps1` does.

- [x] **Step 1: Failing tests:** state of a synthetic buffer holding each pattern once → ORIGINAL; after `sp_swf_apply` → PATCHED; pattern twice → UNKNOWN; with `--game`: the HD UI loose `ui_trade.swf`/`ui_storage.swf` from the old patcher's backup and the `abetterui.dzip` copies are ORIGINAL.
- [x] **Step 2:** run → FAIL. **Step 3:** implement. **Step 4:** run → PASS.
- [x] **Step 5: Commit** `script patches in DLL: SWF patch and inflate`.

### Task 4: Read-book icons (DDS)

**Files:** Create `src/sp_dds.h`, `src/sp_dds.c`

```c
int  sp_dds_decode64(const sp_buf* dds, unsigned* px);            /* 64*64 0xAARRGGBB; DXT1/3/5 or 32-bit */
void sp_dds_over(unsigned* base, const unsigned* top, double opacity);
int  sp_dds_encode64(const unsigned* px, sp_buf* out);            /* 32-bit BGRA DDS, no mipmaps */
```

Port `Dds.Decode64`, `Over`, `Encode64` (ScriptPatches.ps1 lines 299-420) with the same
rounding, so the bytes match.

- [x] **Step 1: Failing test:** decode a known 4x4-block DXT5 sample (hand-built in the test) to its expected pixels; `encode64(decode64(x))` of a 32-bit DDS returns `x`.
- [x] **Step 2-4:** run FAIL → implement → PASS.
- [x] **Step 5: Commit** `script patches in DLL: read-book icons`.

### Task 5: WitcherScript edit engine and patch data

**Files:**
- Create: `src/scriptpatches/` with one `.ws` file per multi-line code here-string of `ScriptPatches.ps1`, verbatim: `book_helpers.ws` (`$BookHelpers`, line 427, still holding the `__COLLECT_BODY__`/`__TEXT_FUNCTION__`/`__READ_ICONS__` placeholders), `continue_button.ws` (`$ContinueButton`), `continue_input.ws` (`$ContinueInput`), `quit_button.ws` (`$QuitButton`), `quit_input.ws` (`$QuitInput`), `quit_members.ws` (`$QuitMembers`), `quit_box.ws` (`$QuitBox`), `inventory_refill.ws` (`$InventoryRefill`); `book_texts.ws` (the output of `New-TextFunction`, generated once - it depends only on `$BookTexts`); and `edits.txt`. One-line codes (`$ShopMerchantPatched`, `$LootNamePatched`, `$LootIconPatched`, `$PreviewIconPatched`, `W2EF_MarkItem( inventory, itemId, AS_item, custom );`, `W2EF_RefillAfterReading();`) stay inline in `edits.txt`
- Create: `src/scriptpatches.rc`, `src/sp_script.h`, `src/sp_script.c`
- Modify: `build.bat` (compile `scriptpatches.rc` too)

`edits.txt`, one edit per line, tab-separated, in the `$FeatureList` order:

```
# feature	entry	where	count	anchor	code
BookMarker	game\gui\guiutils.ws	After	1	theHud.SetString( "DescFull", GetLocStringByKeyExt( "Tooltip" + itemName ) + descFull + "<br>" + fullDescTootlip, AS_item );	W2EF_MarkItem( inventory, itemId, AS_item, custom );
BookMarker	game\gui\guiutils.ws	End	0		@book_helpers.ws
Continue	game\gui\guimainmenu.ws	Body	1	private final function InputMainMenu( button : int ) {	@continue_input.ws
...
```

`@name` = fragment file, anything else = the code itself. The rows are the 14 entries of
`$FeatureList` (ScriptPatches.ps1 lines 1064-1107) in order, with each anchor un-escaped.

Anchor syntax: literal text; every run of whitespace in the anchor matches any run of
whitespace (`\s*`) in the script. This covers all current anchors (escaped literals plus the
three `\s+`/`\s*` patterns in `$PreviewIcon`, `$InputMainMenu + "\s*\{"`,
`$InputMainInGame + "\s*\{"`), and the golden test proves it.

```c
typedef struct { const char* feature; const char* entry; int where; int count; const char* anchor; const char* code; } sp_edit;
enum { SP_BEFORE, SP_AFTER, SP_BODY, SP_REPLACE, SP_END };
int sp_edits_load(sp_edit** out, int* n);                          /* parses the RCDATA edits.txt */
const char* sp_fragment(const char* name);                         /* RCDATA .ws by name */
/* Applies one feature's edits to the texts (Latin-1), all or nothing. why = first failing anchor. */
int sp_script_apply(const char* feature, sp_text* texts, int ntexts, const sp_ctx* ctx, char* why, size_t whylen);
/* ctx: collect body (Get-CollectBody of guiinventory.ws), read-icon book list (W2EF_HasReadIcon) */
```

Port `Add-Edit`, `ConvertTo-Tabs`, `Get-CollectBody`, `New-ReadIconFunction` and the
`__COLLECT_BODY__`/`__TEXT_FUNCTION__`/`__READ_ICONS__` substitution exactly (lines 1167-1346).
`all or nothing per feature`: a feature whose anchors do not all match leaves every text
unchanged and is reported, like the `.ps1`'s `throw`, but the other features still apply.
Also port the guard: a text that already contains `//===witcher2-enginefix===` is not patched.

- [x] **Step 1: Failing tests:** each `where` on a 5-line sample (indentation and CRLF/LF kept); Replace with Count 2; an anchor found 0 or 2 times for count 1 → feature skipped, texts unchanged; whitespace-flexible match of `$PreviewIcon`'s tab-aligned line.
- [x] **Step 2-4:** run FAIL → implement → PASS.
- [x] **Step 5: Commit** `script patches in DLL: script edit engine and patch data`.

  Done 2026-10-31: `src/scriptpatches/{edits.txt,*.ws}`, `src/scriptpatches.rc`,
  `src/sp_script_ids.h` (numeric RCDATA IDs), `src/sp_script.{c,h}`; `build.bat` and
  `tools\patchtest\build.bat` compile the `.rc`. 20 tests, 0 failures; `script_real` applies
  all 14 rows to the real 3.5.0.26 scripts. The port's output also equals the reference's on
  those five entries byte for byte (224 019 bytes, local check). Details in
  `docs/fixes/dll-script-patches.md`. Note: the resource API is in kernel32, so the harness
  needs no `user32.lib`.

### Task 6: Overlay build + golden test against the reference

**Files:**
- Create: `src/sp_overlay.h`, `src/sp_overlay.c`, `tools/patchtest/golden.py`
- Modify: `tools/patchtest/patchtest.c` (`build` command)

```c
typedef struct { wchar_t target[MAX_PATH]; wchar_t cached[MAX_PATH]; } sp_redirect;
typedef struct { int book_marker, cont, pause_quit; } sp_features;
/* Builds into cache_dir (reusing it when the key matches), fills the redirect table. */
int sp_overlay_build(const wchar_t* game, const wchar_t* cache_dir, sp_features f, long long filetime,
                     sp_redirect* r, int* nr, int maxr, sp_log_fn log);
```

Steps inside `sp_overlay_build`, mirroring the `.ps1` main flow (lines 1486-1620):
1. Source of each target: loose file if it exists, else (SWFs) the entry in `abetterui.dzip`.
2. Icons (BookMarker): `Get-ReadIcons` - `def_*.xml` entries in all archives (sorted by name, last wins), items tagged `SortTypeBook`, icon `globals\gui\icons\items\<name without spaces/apostrophes>_64x64.dds`, overlay `globals\gui\icons\tooltip\alreadyread_64x64.dds` at 0.9.
3. Scripts: extract the 5 entries, apply the chosen features, write the new `base_scripts.dzip` (Branch B: plus the icon and abetterui-only SWF entries under their game paths).
4. SWFs: only if ORIGINAL; a movie that is not (another UI mod) is left alone and logged, and only the tick on that screen is lost - the `$swfSkipped` behaviour of the `.ps1` (lines 1503-1520). BookMarker's script edits still apply.
5. Cache key file `cache.key`: DLL version, features, and size + last-write time of `base_scripts.dzip`, `abetterui.dzip`, every `CookedPC\*.dzip` (icons), the loose SWFs. Same key → reuse the files, no rebuild.

**Reading the archives:** `sp_dzip_open` takes the archive in memory, but `pack0.dzip` is
10.5 GB and holds 1 099 item icons and 63 `def_*.xml` (`z_emc_data.dzip` adds 64 more, `ZZ_UPatch.dzip`
16), so Task 6 needs a file-backed reader beside the buffer API - header and table from disk, then seek
and read one entry, like the `DzipFile` class in the `.ps1` - and must never load a whole large archive.
(`abetterui.dzip`, 61 MB, holds the `alreadyread_64x64.dds` overlay and can go through the buffer API.)

`golden.py <game> <scratch>`: makes `<scratch>\ref` and `<scratch>\dll` game layouts (copies of
`base_scripts.dzip` and the loose SWFs **from the old patcher's backup**, because the owner's
live files are patched; hard links for the other dzips, so `<scratch>` must be on the game's
volume), runs `powershell -File tools\script-patches\ScriptPatches.ps1 -Action Install
-Features All -GameDir <scratch>\ref`, runs `build\sptest.exe build <scratch>\dll <scratch>\out`,
then compares: every `base_scripts.dzip` entry decompressed (own Python LZF reader, an
independent check of the C one), each SWF byte for byte, the icon set name for name and byte
for byte. Prints `GOLDEN OK` or each difference.

- [x] **Step 1:** write `golden.py`; run it → fails (no `build` command yet).
- [x] **Step 2:** implement `sp_overlay.c` and `patchtest build`.
- [x] **Step 3:** `python -I tools\patchtest\golden.py "<game>" "<scratch>"` → `GOLDEN OK` on the owner's Mersey + HD UI install. If a vanilla `base_scripts.dzip` is at hand (the earlier dry runs used one), run it there too; otherwise note "Mersey only" in the fix doc.
- [x] **Step 4:** second `patchtest build` with unchanged inputs → log `cache: reused`, files untouched (timestamps equal).
- [x] **Step 5: Commit** `script patches in DLL: overlay build, golden test equal to ScriptPatches.ps1`.

  Done 2026-10-31: `src/sp_overlay.{c,h}` (cache key, Get-ReadIcons, Build-Scripts, Get-PatchedSwf,
  Branch B archive entries, redirect table), the file-backed DZIP reader in `sp_dzip.{c,h}`
  (`sp_dzip_file_*`, used for `pack0.dzip` and covered by the new `dzip_file` test), the
  harness `build <game> <out>` command and `tools\patchtest\golden.py`. `GOLDEN OK`: 460
  `base_scripts.dzip` entries, 152 icons and both movies byte-identical to the reference; the
  second build logs `cache: reused` and touches nothing. The reference inputs were the old
  patcher's backup (Project Mersey + HD UI), no vanilla archive at hand: "Mersey only" in the
  fix doc. `golden.py` was written together with the build command, so step 1's failing run did
  not happen in that order. Overlay build ~110 ms, cached start 16 ms.

### Task 7: Redirect in the DLL (owner start test)

**Files:**
- Create: `src/sp_hooks.h`, `src/sp_hooks.c`
- Modify: `src/enginefix.c` (ini, early thread, chain with the save hooks), `dist-files/witcher2_enginefix.ini`, `build.bat`

ini (defaults per decision 1):

```ini
[scriptpatches]
; Applied in memory at game start; the game's files are never changed. 0 = off.
book_marker=1
continue=1
pause_quit=1
```

Hook behaviour:
- `CreateFileW(path)`: full path via `GetFullPathNameW`, compared case-insensitively with the redirect table. First hit of any target, under a critical section: run `sp_overlay_build` once (synchronously - the game waits; first start only, later starts reuse the cache). Hit: call the original `CreateFileW` with the cached path (read-only access as requested). Miss or build failure: original path.
- Every call found in Task 1 Q3 to report a size/time for a target answers for the cached file (attributes/find data copied from the cached file, name kept).
- Branch A only: a `CreateFileW`/attribute query for `...\gui\icons\items\w2ef_read\*.dds` returns the cached icon.
- Install: the Task 1 early thread (now always on when any feature is on). `GetModuleHandleExA(..._PIN)` as the save hooks do.
- Log once: `script patches: book_marker=on (ok|skipped: why) continue=... pause_quit=... cache=reused|built N ms`.

- [ ] **Step 1:** implement; `.\build.bat`; `build\sptest.exe test` still PASS.
- [ ] **Step 2: Owner start test** (hook rule) with the on-disk 0.5 patches *uninstalled first* (owner runs the old `Uninstall.bat` once; Task 8 handles this for users). Expected: game reaches the main menu; log shows the line above with all three `ok` and `cache=built`.
- [ ] **Step 3: Owner in-game check** (same as `docs/fixes/shop-book-read-marker.md` "Final form" and `menu-buttons.md`): inventory statuses/tooltip/refresh after reading; trade and storage ticks both sides; loot window icon + `(Nothing new)`; container preview icon; Continue; pause-menu Quit. Then each ini key `0` → that feature gone, others still there. Then delete `bin\dinput8.dll` → vanilla game (uninstall check).
- [ ] **Step 4: Commit** `0.6.0 work: script patches applied by the DLL`.

### Task 8: Migration from 0.4/0.5 on-disk patches

**Files:** Create `src/sp_migrate.h`, `src/sp_migrate.c`; modify `src/enginefix.c`

On start, before hooks serve anything: if `<game>\Witcher2EngineFix_Optional\ScriptPatches\backup\manifest.txt` exists, port `Restore-Leaf`/Uninstall (ScriptPatches.ps1 lines 1396-1480): for each `replaced` leaf whose current file still has the manifest's SHA-256, copy the backup back (atomic); `created` leaves are deleted; the `w2ef_read` folder is removed; then rename `backup` to `backup.restored-by-dll`. A leaf that changed since (another mod) is left alone, logged, and script patches are switched off for this start (its text may contain our markers; the Task 5 guard also refuses it). SHA-256 via `BCryptHash` (bcrypt.lib), no own crypto.

- [ ] **Step 1: Failing test** in `patchtest` (`migrate` on a scratch layout made by running the old `Install.bat` logic, i.e. `ScriptPatches.ps1 -Action Install -Features All -GameDir <scratch>`): after `sp_migrate_run`, every file equals the pre-install copy byte for byte, `w2ef_read` gone, backup renamed. Second case: one patched file changed afterwards → it stays, result "partial", patches off.
- [ ] **Step 2-4:** FAIL → implement → PASS.
- [ ] **Step 5: Owner check** on the real install (owner keeps a manual copy of `CookedPC\base_scripts.dzip` and `CookedPC\globals\gui\ui_trade.swf`/`ui_storage.swf` first): reinstall 0.5's patches with the old `Install.bat`, start with the new DLL → log `migrate: restored 3 files, removed w2ef_read`, game works with the DLL patches.
- [ ] **Step 6: Commit** `0.6.0 work: restore old on-disk script patches once`.

### Task 9: Release 0.6.0 without scripts

**Files:**
- Modify: `tools/package-release.ps1` (drop the `Witcher2EngineFix_Optional/...` entries), `docs/README.txt`, `README.md`, `dist-files/witcher2_enginefix.ini`, `CHANGELOG.md`, `src/version.h` (0.6.0 / 0,6,0,0), `docs/fixes/shop-book-read-marker.md` and `menu-buttons.md` (pointer to the DLL implementation), `docs/plans/progress.json`, `docs/local/nexus.md` (description, files, changelog)
- Delete: `tools/script-patches/ScriptPatches.ps1`, `Install.bat`, `Uninstall.bat` (keep `check_texts.py`; the golden test goes with them, so record in the fix doc that it last passed at commit X)

- [ ] **Step 1:** package; zip lists no `.bat`/`.ps1` (`[IO.Compression.ZipFile]::OpenRead(...).Entries`).
- [ ] **Step 2:** the README/antivirus note now only mentions the DLL; Install/Uninstall text is "copy the files / delete them" again.
- [ ] **Step 3:** owner uploads to Nexus as the new main file; check the scan. Target: at or near the W1 level (≤ 1-2 engines, file not quarantined). Record the result in `docs/local/nexus.md`.
- [ ] **Step 4: Commit** `0.6.0: script patches built into the DLL, no scripts in the release` and a GitHub release per `docs/conventions.md` (owner's go-ahead for the push and release).

## Risks

| Risk | Effect | Handling |
|---|---|---|
| Hook not in place before the first `base_scripts.dzip` open (Q2) | patches silently missing | Task 1 measures it; stop and re-plan if it fails |
| A size query not redirected (Q3) | game reads past the end / rejects the archive | Task 1 lists every API; Task 7 covers all of them |
| Generated icons not found (Q4/Q5) | read books keep the plain icon in loot/preview | Branch A/B decided by data; status texts still work |
| A future mod changes an anchor | that feature skipped | same as today, logged; never half-patched |
| Golden test cannot cover vanilla | vanilla untested offline | earlier vanilla scratch copy if present, else owner note |
| AV now flags the DLL instead | quarantine continues | the DLL only reads game files and writes its cache under `bin`; W1's DLL scans clean with similar imports |
