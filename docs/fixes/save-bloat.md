# Many saves: list cost, resident previews, Steam quota (research, 2026-10-30)

Status: trace run 1 (2026-10-30) found a real leak in the save list (section 6);
fix `[saves] fix_list_leak=1` confirmed in game the same day (run 2, end of file). Owner's direction: fixes must not limit how many saves a player keeps.

Trigger: a Steam review says the game "creates new save files" instead of
overwriting, and that the bloat makes it crash regularly from chapter 2 on.
Compare Witcher 1's save-list fix in the sibling repo
(`witcher-enginefix/docs/fixes/save-list.md`).

Owner's state on this date: 60 saves (30 AutoSave, 29 ManualSave, 1 QuickSave)
in `userdata/<steamid>/20920/remote`, 120 files, ~90 MB. `.sav` size grows with
progress (287 KB prologue to ~930 KB late chapter 1/2); every preview is
921 656 bytes (live) or 691 254 bytes (stock).

## 1. Every save is a new file, nothing is ever pruned

`009B79AD` (write save) names the file `AutoSave_%04i` or `ManualSave_%04i`
(QuickSave is the only fixed name). The number comes from `009B6F82`:
enumerate all saves, parse the trailing number, return max + 1. No code in the
save path deletes or overwrites an older AutoSave. Each save is two files
(`.sav` + `_640x360.bmp`). So the review's premise is right: autosaves pile up
until the player deletes them.

## 2. The save list reads every save in full, twice per build

`009A49B9` builds the list. Callers: `009A515C` (startup/main menu: it uses the
newest save to pick the menu background, so the list is built at **every game
start**) and `009A5245` (a native registered in `004B817A`, called from the GUI
when the load/save panel opens).

Per save, with the Steam backend (`00AA73A4` true, which the startup override
makes the normal case, see `save-previews.md`):

- `009B76FD` -> `00A9805B` opens the `.sav` through provider slot +0x18
  (`00931DB7` -> `0052ADFF`, mode 1). That calls Steam `GetFileSize` + `FileRead`
  for the **whole file**, then writes the whole buffer to a local temp file and
  reopens it, only to read `timeInfo`/`saveInfo`/`worldInfo`/`timeManger` from
  the header.
- The preview: provider slot +0x18 (mode 0, whole BMP read into memory) and
  GUI image registry AddImage (`00B28657`).

Late-game saves are ~0.9 MB + 0.9 MB, so N saves cost ~1.8 MB x N of Steam
reads and ~0.9 MB x N of temp-file writes per list build. Cost is linear (no
quadratic GUI step was found here, unlike W1's `CListControl:Refresh`), but it
is paid at startup and on each menu open.

## 3. Previews stay resident for the whole session (likely crash factor)

AddImage `00B28657` decodes the stream (`009DB957`: image object + texture,
registered under a critical section) and keeps it in the registry with a
refcount. Remove (`00B285C6`, vtable slot 3) is reached only through the vtable;
nothing in the save or list path calls it (the existing
`[saves] fix_thumbnails` QuickSave reload is the only caller we know of).
AddImage returns early for a known key, so the cost is one decoded 640x360
image per distinct save name ever listed in the session, including saves
deleted since.

Rough size: 640x360x4 = 0.92 MB per preview as a texture, plus whatever the
decoder and the D3D9 managed copy keep. 300 saves -> ~0.3-0.6 GB pinned in a
32-bit process that is already known to run near its limit. witcher2.exe is
`LARGE_ADDRESS_AWARE` (characteristics 0x122), so 4 GB on 64-bit Windows, but
the game's own streaming has to fit in what is left. This matches "crashes
more and more as saves accumulate". **Not measured**: the real per-image cost
and whether the registry is flushed on world load.

## 4. Steam Cloud quota: silent save loss at ~500 saves (unverified)

The app's cloud quota (local `appinfo.vdf`) is 1 000 files / 1 GB. At two files
per save that is 500 saves, or ~550 at late-game sizes by bytes. The game's
writer (`0052AC6C`) **ignores the return value** of `FileWrite`, so a write over
quota would leave no `.sav` and no error. Whether Steam enforces the quota when
cloud sync is disabled for the app has not been checked.

## 5. W1 import scan (minor)

The same builder also lists `*.TheWitcherSave` in the W2 save folder and one
other config path and, when a game flag is set, opens each one with
`W1CanImport` (`00AEA809`). Only matters if Witcher 1 saves are copied there.

## Fix candidates (not implemented)

| # | Idea | Effect | Risk |
|---|---|---|---|
| A | (Owner, 2026-10-30: do not reduce how many saves a player can keep; A is shelved.) Autosave rotation: after an AutoSave `.sav` write (FileWrite hook already exists), delete AutoSaves beyond the newest N (`[saves] keep_autosaves`), `.sav` + `.bmp`, through the game's own `ISteamRemoteStorage::FileDelete`. Off by default or conservative N. | Caps all of 2-4 at the source | Deletes player data; must be opt-in and never touch Manual/Quick |
| B | (Superseded for repeat opens by `fix_list_leak`, section 6.) Evict list previews after the panel closes (Remove for every name AddImage saw from `009A49B9`) | Fixes 3 without deleting anything | Need to find the panel-close point and confirm nothing still holds the image |
| C | Header-only read for the list: serve `009B76FD`'s open with a partial read | Faster startup / menu with many saves | Needs the save container layout; larger hook |
| D | Log `FileWrite` failures and warn on quota | Makes 4 visible | Small |

Measure before choosing: log process private bytes / VA before and after
opening the load menu with the current 60 saves (in-process logging, no
debugger), then with a copied set of ~300 saves in a test backup.

## Save list trace (`[diagnostics] savelist_trace=1`), 2026-10-30

`src/enginefix.c`, section "save list trace". Passive: the list is built exactly
as before.

- The two `call 009A49B9` sites (`009A5175` startup, `009A524F` GUI request)
  are redirected to a stdcall wrapper after checking the builder prologue
  and each original rel32 (locked exchange; each rel32 is within one cache line).
- `ISteamRemoteStorage` slot 1 `FileRead` (thiscall, 3 args, shape from
  `0052AEB6..AECA`) counts reads and bytes while a build runs.
- The existing AddImage hook counts previews per build and how many distinct
  names the registry has received this session (FNV hash, case-folded).
- Memory before/after each build and every 60 s: private bytes and a
  `VirtualQuery` walk (committed, reserved, free, largest free block).

Log lines:

```
savelist: build N (startup|menu) T ms; Steam reads S .sav + O other, M MB in R ms; previews P (new X, H held this session)
savelist:   private a -> b MB, commit a -> b MB, largest free a -> b MB
savelist: periodic private=... commit=... reserve=... free=... largest_free=... MB, previews held=H
```

Validation: `/W4` clean. `tools/savelisttest` maps a fake builder and both call
sites at their real addresses, redirects them and runs them 1001 times (stdcall
stack balance), checks the refusal of a foreign call target, the preview
counting and the memory walk: 7/7. The `FileRead` hook has not run in the game
yet (same shape as the confirmed `FileWrite` hook). Installed 2026-10-30 12:32
with the game closed; previous DLL/ini/log in
`build/savelist-trace-install-backup-20261030-123209/`. Rollback: copy that DLL
back, or set `savelist_trace=0`.

What to read from the first run: does the startup build show up (and how long
it takes with 60 saves), does `commit` grow by roughly 1 MB per new preview on
the first menu open and stay flat on later opens, and does `largest free` shrink
over a long session.

## 6. Trace run 1: the list leaks every preview it already shows (2026-10-30)

Owner: game start, Load menu twice from the main menu, load a save, Load menu
twice from the pause menu, quit. 60 saves. Log:
`docs/validation/savelist-trace-run1-2026-10-30.log`.

| build | when | ms | previews (new) | private MB | commit MB |
|---|---|---|---|---|---|
| 1 | startup | 4547 | 60 (60) | 879 -> 979 | 1232 -> 1332 |
| 2 | main menu | 1187 | 60 (0) | 1021 -> 1071 | 1386 -> 1436 |
| 3 | main menu | 1125 | 60 (0) | 1055 -> 1105 | 1426 -> 1476 |
| 4 | pause menu | 1156 | 60 (0) | 1560 -> 1610 | 1915 -> 1965 |
| 5 | pause menu | 1156 | 60 (0) | 1611 -> 1661 | 1981 -> 2031 |
| 6 | back to main menu | 1172 | 60 (0) | 1426 -> 1476 | 1778 -> 1828 |

- Every build reads all 60 `.sav` and 60 BMPs in full from Steam: 89 MB, but
  only ~180 ms of the ~1.15 s. The rest is header parsing, the temp-file copy
  and the GUI (not split further yet).
- **Every build adds ~50 MB that is not given back**, also when all 60
  previews are already held (0 new). 50 MB / 60 = ~0.85 MB, the size of one
  921 656-byte BMP. Between builds 2 and 3 only ~16 MB came back; 4 -> 5 none.
- The registry is not flushed by loading a level (0 new previews in the pause
  menu, `held` stays 60).
- Largest free block 2044 -> 1771 MB over the run: fine at 60 saves.

Cause (static, unpacked 3.5.0.26): `009A49B9` opens the preview with provider
slot +0x18 (`0052ADFF`: whole BMP read into a buffer, stream vtable `00F02C2C`),
keeps it in EDI, calls AddImage (`009A4B5A..4B65`) and then reuses EDI
(`009A4BD8`) without freeing it. AddImage `00B28657` computes the key, calls
`Find` (`004A4A03`, EAX = map at registry +4, `(key*, out*)`, ret 8) and on a hit
returns at `00B2867F` without touching the stream. Only a new name hands the
stream to the loader (`009DB957`). So with N saves every menu open leaks
~0.9 MB x N: about 270 MB per open with 300 saves. That is the most likely
reason the game crashes more often as saves pile up; the save menu probably
builds the same list (not traced separately).

## Fix: `[saves] fix_list_leak=1` (default on)

The call-site redirect from the trace is now also installed for the fix (the
logging stays behind `savelist_trace`). While `009A49B9` runs, the AddImage hook
does the game's own lookup first (ImageKey + `Find` on registry +4, through a
small inline-asm call because `Find` takes the map in EAX). If the name is
held, the stream is the Steam reader (vtable `00F02C2C`) and it is not a name we
just evicted for the QuickSave refresh, it deletes the stream with its own
scalar deleting destructor (`0052AFAA`, thiscall, flag 1: inner memory stream,
temp file, buffer, object) and returns, which is what the original does minus
the leak. Anything else goes to the original unchanged. Fingerprints: builder
prologue, both call sites' rel32, `Find` and destructor prologues, vtable slot 0.

Validation: `/W4` clean. `tools/savelisttest` 14/14 (fake `Find`, ImageKey,
destructor and stream vtable at their real addresses inside a test image linked
at 0x400000): held name in the list frees the stream once and skips the
original; outside the list, new names, unknown stream types and a NULL stream
all reach the original. `tools/thumbtest` still all pass. Installed
2026-10-30 12:43 with `savelist_trace=1`; run 1 log kept as above.

Expected in the next run: builds 2+ show `leaked streams freed 60` and commit
growth near 0 MB per open instead of ~50 MB. Build 1 (60 new previews, +100 MB)
should be unchanged: whether the loader keeps or frees a stream it took is not
known yet.

## Confirmed in game, run 2 (2026-10-30 12:47)

Same steps as run 1, fix on. Log: `docs/validation/savelist-trace-run2-fix-2026-10-30.log`.

| build | when | ms | new | freed | private MB | commit MB |
|---|---|---|---|---|---|---|
| 1 | startup | 1125 | 60 | 0 | 879 -> 980 | 1233 -> 1334 |
| 2 | main menu | 1125 | 0 | 60 | 1021 -> 1021 | 1387 -> 1387 |
| 3 | main menu | 1110 | 0 | 60 | 1021 -> 1022 | 1396 -> 1396 |
| 4 | pause menu | 1156 | 0 | 60 | 1435 -> 1436 | 1809 -> 1810 |
| 5 | pause menu | 1157 | 0 | 60 | 1436 -> 1436 | 1810 -> 1810 |
| 6 | back to main menu | 1141 | 0 | 60 | 1223 -> 1223 | 1575 -> 1576 |

Repeat opens now cost 0-1 MB instead of ~50 MB; no crash, timing unchanged.
Build 1 still adds ~100 MB for 60 new previews (~1.7 MB each: decoded image plus
whatever the loader keeps). That is paid once per distinct save name per session
and is open question 3 (resident previews). Run 1's 4.5 s first build was a
cold Steam/disk cache; run 2's was 1.1 s.

`savelist_trace` set back to 0 in the installed ini; the shipped ini has it at 0.

Still open: what the remaining ~1 s per build is spent on (not Steam reads),
whether a new preview's ~1.7 MB can be released when the menu closes, and
whether the save menu builds the same list.
