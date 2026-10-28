# Missing save previews, 2026-10-28

Status: FIXED by enginefix `[saves] fix_thumbnails=1`, confirmed in game
restoring previews for new saves. Root cause: a capture/save race (see "Trace
result" at the end). Old saves keep their missing previews.

## Handoff summary from the first investigation (historical)

**Superseded by the trace result and fix at the end.** Original DXVK, enginefix
and User.ini are restored; CloudStorage=true, User.ini/Rendering.ini read-only.
No helper/debugger remains active. All 11 existing SAVs and the one stock BMP
matched their pre-crash hashes. Latest save is ManualSave_0009; no new save was
created during the breakpoint incident.

| Attempt | Observed result | What the next agent must retain |
|---|---|---|
| Disable Steam cloud synchronization | Already disabled; thumbnails still missing | Synchronization and the game's storage-backend selection differ. |
| CloudStorage=false in locked User.ini | ManualSave_0007 still written to Steam, no BMP | Inconclusive backend comparison: startup can override the runtime flag. Do not repeat the ini-only test. |
| Lock User.ini read-only | It was already locked during the false-setting test | Disk protection does not prevent an in-memory override. |
| Native D3D9 game test without DXVK | ManualSave_0008 created without BMP | DXVK is not required to reproduce the failure. DXVK was restored afterward. |
| Isolated 4K readback/downsample/BMP probe | Native and installed DXVK both pass, identical BMPs | Does not test the game's scheduling, live targets, fullscreen state or storage callback. |
| Automatic trace readiness via runtime log | fopen_s exclusive lock caused helper failure; ManualSave_0009 untraced | No capture data. Do not use Get-Content on the running enginefix log. |
| Live CDB software-breakpoint trace | Attached/READY, then crashed at first breakpoint009B6BC5, code4000001F | Workflow DISABLED in both helper and command file. No capture HRESULTs. Canary clear/detach tests did not validate the sustained lifecycle. |

**Not performed:** a full Documents configuration reset, a lower-resolution
game comparison, or a controlled enginefix/content-mod A/B. The forum reset
success is another user's report, not a verified local fix. No live capture
HRESULT identified the original failure, and the exact gradient fallback has
not been traced. Do not present the diagnostic crash as its root cause.

Next step: read-only review of the mapped capture/storage path and retained
evidence; design a separately validated diagnostic method before another
human test. The user has not been asked to perform another test after the crash.

## Initial evidence (before the later game tests)

- Latest graphics-preset build is installed and logs startup at `extreme`, then
  successful distance-hook installation. Those changes only group the existing
  distance/LOD controls; no save/capture hooks exist in enginefix.
- The active save directory is
  `C:/Program Files (x86)/Steam/userdata/<steamid>/20920/remote`.
  Eight `.sav` files are present, including `ManualSave_0006.sav` created at
  14:25:53. Only `AutoSave_0000_640x360.bmp` exists. The Documents gamesaves
  directory is empty. The surviving BMP is a stock scene, as established in
  `quality-audit-2026-10-28.md`.
- Steam logs a failed read for `QuickSave_640x360.bmp` at 12:05:39 (before the
  supplied screenshot), and again at 18:57:56 and 19:18:45. The gradient is
  consistent with displaying a missing image; its precise fallback texture has
  not been traced.
- Steam log says `Sync Disabled`, but `[FileSystem] CloudStorage=true` in
  Documents `Config/User.ini`. Disabling Steam synchronization does not change
  the game's choice of storage backend.
- The engine requests 640x360 BMP output irrespective of display resolution:
  constructor `009B70DA`, capture request `00AA1C4B`. Thus 4K does not imply a
  4K-sized file being sent to Steam. Full-resolution intermediate readback can
  still be affected by graphics-driver behavior or memory pressure.
- The user has normal full-control filesystem permissions on Documents'
  gamesaves directory. Defender event-log inspection produced no matching
  records when run outside the sandbox; this is not proof that every security
  product allows writes.

## Forum report supplied by the owner

[Saves screenshots missing (CDPR forum, July 2014)](https://forums.cdprojektred.com/index.php?threads/saves-screenshots-missing.31270/)
was read in the owner's open browser tab. The suggested workaround was to back
up saves and reset the Documents/Witcher 2 directory, then create a new save.
The original poster confirmed it worked. The replies also explain that Steam
saves live in userdata/20920/remote while the Documents gamesaves directory may
be empty.

This establishes an older occurrence and a reported workaround, not the exact
cause. No DXVK attribution or resolution threshold is established in that
thread. A full reset would discard the owner's tuned graphics/input settings,
so first test a single storage setting, retaining the remaining configuration.

## Isolated 4K readback test

`tools/savepreviewprobe/probe.cpp` creates a hidden D3D9 device, clears a
3840x2160 X8R8G8B8 backbuffer to a known color, reads it into a system-memory
surface, locks it with the game's flags, applies the game's block averaging,
and encodes a 640x360 BMP through the same `d3dx9_39.dll` imported by the game.
It checks every output pixel against the known color and tests both memory
and direct-to-file encoding. It does not open the game or access saves.

Fresh x86 build: `tools/savepreviewprobe/build.bat` (MSVC `/O2 /W4 /MT`, no
warnings). Tests with the installed DXVK 3.1.1 `bin/d3d9.dll` and native
`C:/Windows/SysWOW64/d3d9.dll` both exit 0:

- All capture, lock and encode HRESULTs are `00000000`.
- 0 mismatched pixels out of 230400.
- BMP is 921656 bytes; both outputs have SHA256
  `1E67E420FBCD5B8BF68DB84BA384E5F41D5378F4036D756DB7F601E7F1B5AB3F`.
- DXVK was also tested with `DXVK_CONFIG_FILE` pointing at the installed
  `bin/dxvk.conf`. See `docs/validation/save-preview-*-4k-2026-10-28.log`.

This rules out a general inability of this DXVK install to perform these
operations at 4K. It does not reproduce the game's render-target lifecycle,
fullscreen mode, memory load, scheduling, callback, or Steam storage transfer.
The uniform test image checks transport/encoding, not the visual correctness
of the downsampling filter on arbitrary game content.

Relevant Microsoft documentation:
[GetRenderTargetData](https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getrendertargetdata),
[D3DXSaveSurfaceToFileInMemory](https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3dxsavesurfacetofileinmemory).
Context7 resolved Direct3D 9 to
`/websites/learn_microsoft_en-us_windows_win32_direct3d9`; the focused docs
lookup returned related surface-copy material, so exact method contracts were
verified directly against Microsoft's references.

## Engine path and original test plan (historical)

The plan below was carried out. Read the results and incident sections before
acting; it is not a fresh instruction to repeat the original tests.

- `009B747E` schedules live screenshot capture to a temporary local filename
  with callback `009B733A`, or writes a stock level BMP for non-live captures.
- `0053CEF0` performs readback/downsampling and D3DX encoding.
- `00B15317` invokes capture and then the completion callback.
- `009B733A` only transfers the temporary image to the storage backend when
  `00AA73A4` says storage is active; additional behavior depends on storage
  state at engine object +0x114. `00AA73A4` checks +0x234 and +0x230.
- `00A976B2` constructs the local Documents gamesaves path and calls
  CreateDirectoryW. This is also used for temporary capture output before the
  Steam-backend transfer.

First controlled test: back up User.ini and current saves; copy existing saves
to Documents gamesaves without overwriting anything; set only
`[FileSystem] CloudStorage=false`; keep User.ini read-only afterward. Retain
DXVK, 3840x2160, Rendering.ini and all enginefix settings. Human starts through
Steam/Launcher, loads a copied save and makes a NEW manual save. Inspect for the
new `.sav` plus `_640x360.bmp` under Documents gamesaves.

If that works, storage-backend selection/transfer is implicated and this is a
potential workaround; it does not by itself identify the broken instruction.
If it fails, restore the setting and compare native D3D9 at the same resolution,
then investigate the in-process capture HRESULT/callback boundary. A full
configuration reset can be a later controlled experiment with a backup.

Original Steam files must be retained. New saves made during a local-storage
test stay in Documents until explicitly copied back when rolling back. Old
missing camera images cannot be recovered from an absent sidecar.

## Local-storage test prepared with owner approval

Prepared 2026-10-28 at 19:32 Oslo time; game and launcher were closed.
Owner explicitly approved preparation in chat. Human game test was subsequently
performed; see the result below. This configuration has now been rolled back.

- Backup folder: `build/save-preview-local-test-20261028-183244-UTC/`.
  `config/` holds original User.ini and Rendering.ini; `steam-saves/` holds all
  eight SAVs and the existing AutoSave_0000 BMP. `manifest.json` records hashes.
- Copied those nine files into the previously empty
  `%USERPROFILE%/Documents/Witcher 2/gamesaves/`. Hashes match the originals.
  No Steam source files were edited or removed.
- Changed only `[FileSystem] CloudStorage=true` to `CloudStorage=false` in
  Documents `Config/User.ini`; restored its read-only attribute in a finally
  block. Rendering.ini remains read-only and its hash is unchanged.
- User.ini original SHA256:
  `B1C30FAF50C7FC28B9CB7297959CBC770B473A6DD0756BA5A57AEF005C4564C6`.
  Test SHA256:
  `E671B62ACB5CCF0E7E03ED8F1F15AC201827333FEF39D7B7EB087ACE331B9F94`.

Human action: launch via Steam/Launcher, load an existing copied save, create a
new MANUAL slot, then inspect it in the Load menu. Existing missing thumbnails
are not expected to return. Report completion so the new local SAV/BMP pair
can be inspected.

Rollback: close game/launcher, preserve any new or changed local saves first,
then restore the backed-up User.ini (unlock destination first, relock after).
Do not delete Documents gamesaves. Restoring the backend selects the original
Steam directory, which will not automatically contain progress made during
the local-storage test.

## Result: backend override invalidated the local-storage comparison

Owner reported that the new manual save still has no image. Filesystem checks
show `ManualSave_0007.sav` (376031 bytes, 19:38:50) in the Steam remote directory,
with no corresponding BMP. Documents gamesaves still contains only the original
nine copied files. User.ini remained `CloudStorage=false` on disk. Steam logs
show the new SAV was written through its backend.

Therefore this was NOT a verified local-backend test and does not rule out a
Steam-storage transfer problem. Correct the earlier assumption that changing
this ini key alone would select local storage.

New static evidence (unpacked 3.5.0.26):

- `00AA9D32` reads User/FileSystem/CloudStorage into engine object +0x234.
- Boolean reader `00403936` explicitly recognizes both UTF-16 `true` and
  `false` (strings `00E76D34` and `00E76D40`), so false is valid syntax.
- Startup `00AC9152` installs the Steam storage provider at +0x230 and calls
  `00AA73A4` at `00AC920E`. If storage is inactive, it calls provider vtable
  +0x28, and if that returns true, it unconditionally invokes `00AA982C`
  with CL=1 at `00AC9220..22`.
- Provider vtable is `00F02B78`; +0x28 points to `00931492`. Raw code there
  loads provider +0x5C, invokes its vtable +0x2C, then returns whether the
  result is greater than zero. This is consistent with existing Steam files
  causing the startup override observed here.
- `00AA982C` sets engine +0x234 and updates/saves the config through
  `004120B7`/`00AC1C97`. The read-only ini can still say false while the
  in-memory value has been enabled. No runtime flag was directly sampled.

## Native-D3D9 game comparison prepared

Prepared 2026-10-28 at 19:42 Oslo time, game and launcher absent.

- Backed up all 10 current Steam SAV/BMP files (including ManualSave_0007)
  and all nine local copies under
  `build/save-preview-native-test-20261028-184254-UTC/`, with a hash manifest.
- Restored original User.ini byte-for-byte from the first test backup;
  `CloudStorage=true`, original SHA256 `B1C30FAF50C7FC28B9CB7297959CBC770B473A6DD0756BA5A57AEF005C4564C6`,
  read-only restored. Existing saves in both locations retained.
- Backed up installed DXVK d3d9.dll and renamed the installed file to
  `bin/d3d9.dll.savepreview-disabled`. No `bin/d3d9.dll` remains, selecting
  system D3D9 on the next normal launch. DLL SHA256 before/after:
  `265888C31CA78DFFA290C39CB7E50BFB02762590E41927906E46FB32F01497FA`.
- Enginefix DLL/ini, dxvk.conf, graphics configuration and game exe untouched.

Human starts via Steam/Launcher, loads a save, creates a NEW manual slot, and
checks its preview. This tests native D3D9 against the original DXVK game
behavior with the same actual Steam backend and graphics configuration.
Completed, failed to create a preview; see the result below.

Afterward restore DXVK while the game/launcher are closed by renaming exactly
`bin/d3d9.dll.savepreview-disabled` back to `bin/d3d9.dll`, first checking that
the destination does not exist. Do not overwrite another DLL. Retain all new
saves. If native succeeds, the in-game DXVK/capture interaction becomes the
main lead; if native fails too, investigate shared capture/callback behavior.

## Native result and return to original setup

Owner reports no preview again. `ManualSave_0008.sav` (374236 bytes, 19:47:10)
exists in Steam remote with no matching BMP. At inspection `bin/d3d9.dll` was
absent, the disabled DXVK file remained, and the DXVK log timestamp was still
19:39:14 from the preceding run. This is evidence the native comparison was
actually performed. Original DXVK was then restored, with verified hash
`265888C31CA78DFFA290C39CB7E50BFB02762590E41927906E46FB32F01497FA`.
The original CloudStorage=true User.ini and read-only state remain.

Saved result and runtime log in native backup's `after-native/` directory.
ManualSave_0008 SHA256:
`CE45362397F5CD0529643DCE1E5F110BCA3289FD5A5F1495D7DC8EEFEE18B31B`.

The owner asked if making the ini read-only could help. It was already read-only
throughout the false-setting test. That protects the disk file, not the runtime
flag that startup can override. Do not repeat that same test expecting a lock
to change the result. DXVK is not required to reproduce the missing preview.

## Retired capture trace (historical setup, no new mod hooks)

Microsoft's installed x86 CDB is available under Windows Kits 10/Debuggers/x86.
`tools/savepreviewprobe/capture-trace.commands.txt` was intended to log screenshot scheduling,
capture arguments, source dimensions, each D3D HRESULT, D3DX encoding HRESULT,
and the storage callback. Its commands requested automatic continuation, with
no register/data-value edits. The actual game run crashed; both entry points
are now disabled. A capture-prologue signature guard was insufficient to
validate the live debugger lifecycle. The game exe on disk was never modified.

`wait-for-capture-trace.ps1` checks the installed exe hash/path, waits for a
normally launched game process at least 10 seconds old, then attaches CDB with
`-pd` (detach on quit). `-AllowExisting` accepts a session that was already
started when the helper runs. It writes a timestamped log under
docs/validation and exits with the debugger. No game launch is attempted.
Wait time defaults to 10 minutes before attach; no unattended restart/schedule.

The installed debugger's breakpoint/printf/detach mechanism was tested against
the standalone savepreviewprobe, NOT against the game. The canary detached at
the executable entry, and the probe continued to produce the expected BMP.
The helper's PowerShell syntax parsed without errors. These isolated checks
did not establish safe handling of live game breakpoints. The game attempt
later crashed; see the incident section. Do not run the retired workflow.

Read-only reviewer found no concrete blockers in the command/helper files.

## Trace attempt 1 failed before attachment: exclusive runtime log

Exec session `50015` was armed with a 30-minute wait. It exited 1 when its
initial readiness check attempted Get-Content on the still-open enginefix log.
The owner created ManualSave_0009.sav (371843 bytes, 20:02:56), without BMP,
but no CDB capture trace was collected. Do not interpret this as an
instrumented result or ask the owner to repeat that exact blind workflow.

Root of helper failure: enginefix uses `fopen_s(..., "w")` at enginefix.c:796;
the runtime log is held exclusively. A normal shared .NET FileStream does NOT
replicate this, so an initial generic file-sharing test was discarded. The
actual same fopen_s call in `log-lock-canary.c` reproduces the Get-Content
failure while running.

Removed ALL runtime-log reads from the helper. The runtime signature remains
the breakpoint guard; process age alone is not proof of readiness. Revised
workflow at that time: owner leaves game at main menu, then agent invokes the helper with
`-AllowExisting`, confirms actual `W2PREVIEW READY` output, and only then asks
for a new manual save.

Validation: build-canary.bat compiles the x86 fopen_s canary with /W4, no warnings.
An isolated run reproduced the original log lock; installed x86 CDB then
attached and ran attach-canary.commands.txt, printed W2PREVIEW ATTACH CANARY,
and detached with qd while the file was still locked. The target remained
running, subsequently wrote CANARY CONTINUED AFTER DEBUGGER DETACH, and exited
0. The revised helper parses successfully. Read-only review found no blockers
for this manual main-menu workflow.

Owner subsequently confirmed main menu. CDB attached in exec session `81619`,
PID7288. Actual output confirmed `W2PREVIEW ATTACHED engine=0a854000 cloud=1
viewport=3840x2160`, all 11 breakpoints installed, `W2PREVIEW READY`, and `g`
continuing the game. Trace file:
`docs/validation/save-preview-capture-20261028-190954-UTC.log`. The attempt
subsequently crashed at the first breakpoint; this workflow is now DISABLED.

## Incident: live breakpoint escaped handling and crashed the game

Owner reported immediate game exit on saving. WER Application events1000/1001
at20:11:20/23 record PID7288, exception `0x4000001F`, fault RVA`005B6BC5`.
Offline inspection of `witcher2.exe.7288.dmp` confirms:

- ExceptionAddress `009B6BC5`, `STATUS_WX86_BREAKPOINT` (WOW64 breakpoint).
- EIP=`009B6BC5`, instruction byte `CC` (`int 3`), replacing the original A1
  opcode at the FIRST logging breakpoint (buffer-capture schedule).
- No W2PREVIEW breakpoint-handler events or capture HRESULTs were logged.
  CDB output ends with loading/streaming output and NatVis teardown; session
  81619 exited1. Neither CDB nor Witcher2 remains running.

The injected breakpoint caused the new crash. This is a failure of the
diagnostic workflow, not evidence of the original thumbnail failure. Why CDB
ended/failed to handle this breakpoint is unproved. The cross-turn plain-pipe
session's debugger/input lifecycle is a possible cause, not a demonstrated
root cause. Do not attribute it to DXVK or screenshot encoding.

The prior canary only verified explicit `bc *; qd` or direct qd; it did not
validate sustained auto-continue handling across chat turns or an unexpected
debugger exit with live breakpoints. Review of the command file also did not
establish that runtime lifecycle property. No more live software-breakpoint
tests with this workflow.

Retired by unconditional throw before any attachment in
wait-for-capture-trace.ps1, and immediate qd before breakpoint setup in
capture-trace.commands.txt. Historical commands retained below the guard for
incident review. No background helper/debugger remains. On-disk game exe, mod
DLL and DXVK hashes match their verified originals; User.ini is original
CloudStorage=true/read-only, and Rendering.ini remains read-only.

All 11 existing SAV files and the original BMP were checked against their
pre-test hashes and match. No ManualSave_0010 or other new SAV was created.
Incident dump/runtime log copied to
`build/save-preview-breakpoint-incident-20261028/`. Offline debugger output:
`docs/validation/save-preview-breakpoint-crash-2026-10-28.log`.

Original thumbnail issue remains unresolved. Next investigation must use
offline evidence or a separately validated method that does not leave live
software breakpoints; do not ask for another identical test.

## Second review, 2026-10-28 evening (offline only, no game test)

Read-only review of Steam state, Steam logs, the online reports and the
unpacked exe. No files outside this document were changed.

**Steam Cloud state.** `userdata/<steamid>/7/remote/sharedconfig.vdf` has
`"20920" { "cloudenabled" "0" }` (Witcher 1, 20900, is also off). Local
`appcache/appinfo.vdf` gives the app's cloud quota as 1 000 000 000 bytes /
1000 files, so the old 100 MB quota problem reported on the forums is not it.

**Timeline from `Steam/logs/cloud_log*.txt`.** On 2026-10-27 23:26 the session
started with cloud ON (`AC Launch,down`) and the 2024 `QuickSave_640x360.bmp`
(921656 bytes, a real 32-bit live capture, uploaded 2024-05-26) was still
`in sync` locally. By the exit at 2026-10-28 00:21 Steam reported
`Sync Disabled`. ManualSave_0001..0003 (23:53, 23:54, 00:19) were made in that
session and have no BMP. The point at which cloud was switched off within that
session is not logged, so a cloud-off cause is **not** established; the
failure may predate it. The 2024 BMP shows live capture once worked for this
account, on an earlier install/hardware (this install dates from 2026-10-04).

Steam logs no successful `FileWrite`; each successful save is visible only as
one `YldWriteCacheDirectoryToFile`. There is no BMP entry at all in
`remotecache.vdf` for any ManualSave, so the BMP never reached Steam. The
game's repeated `FileRead` of `QuickSave_640x360.bmp` comes from a stale
`persiststate 2` (deleted) cache entry that cannot be synced while cloud is off.

**Online.** Besides the CDPR 2014 thread above, a GameFAQs topic
(<https://gamefaqs.gamespot.com/boards/975399-the-witcher-2-assassins-of-kings/68581959>)
reports the same symptom: Steam version, cloud disabled, every manual save
shows a purple image instead of a screenshot. No answer or fix is posted.

**Static code (unpacked 3.5.0.26).**

- `009B79AD` (save flow) clears `DAT_0160D0ED/EE`, then calls `009B747E`.
- `009B747E` takes the live path only when `00AA7902` and `00AA72DE` both
  return 0; it then schedules `00AA1C4B(name, &state, 009B733A)` and sets both
  flags to 1. Otherwise it copies a stock `globals\gui\savescreens\l0N.bmp`
  (this is the 24-bit 691254-byte AutoSave_0000 image) and writes it at once.
- `009B733A` (capture callback) only acts if the pending flag is still set and
  `00AA73A4` reports storage active (+0x234 flag and +0x230 provider). It opens
  the local temp BMP via the file system (`DAT_013F0924`, mode 0x20), reads its
  size, and writes it through provider vtable +0x1C.
- Provider vtable `00F02B78` +0x1C = `009316CD`, which builds a buffered writer
  (`0052ABFF`, vtable `00F02BF4`). Its close routine `0052AC6C` ends with
  `ISteamRemoteStorage` slot 0 (`FileWrite(name, data, size)`) and **ignores
  the return value**. A zero-size or failed write therefore disappears silently.
- Windows Controlled Folder Access is off (`EnableControlledFolderAccess=0`);
  only Windows Defender is registered as AV.

**Proposed next step: in-process logging, no debugger.** Add an optional
diagnostic section to enginefix that logs, without changing behaviour:
`ISteamRemoteStorage::FileWrite/FileDelete` (name, size, result) by vtable
patch, the exe's `d3dx9_39` `D3DXSaveSurfaceTo*` imports (HRESULT, path), and
`CreateFileW/DeleteFileW/MoveFileExW` calls whose path contains `640x360`.
One new manual save then shows whether the capture runs, whether the temp BMP
is written, and whether Steam rejects it. If the game's capture cannot be
repaired, enginefix can write its own 640x360 32-bit BMP through `FileWrite`
after a `.sav` write (mod-made thumbnail).

## Save trace build (enginefix `[diagnostics] save_trace=1`)

Implemented in `src/enginefix.c` ("save preview trace"), off unless the ini key
is 1. Passive: every hook calls the original first and returns its result and
last-error unchanged.

- IAT slots of the unpacked import table, each patched only if it still holds
  the `GetProcAddress` value: `CreateFileW` 00CAD320, `DeleteFileW` 00CAD314,
  `CopyFileW` 00CAD308 (logged only for `.bmp`/`640x360` paths),
  `D3DXSaveSurfaceToFileW` 00CAD4A8, `D3DXSaveSurfaceToFileInMemory` 00CAD4AC
  (source surface size/format/pool/MSAA, HRESULT, encoded size).
- `ISteamRemoteStorage` slot 0 `FileWrite`, reached through the
  `SteamRemoteStorage` accessor once it returns non-null. The thiscall shape
  (ECX = interface, three pushes, callee cleanup) was confirmed at
  `0052AD91..0052ADA5`. A `.sav` write also logs engine +0x234/+0x230/+0x114
  and the capture flags.
- Capture flags `0160D0ED`/`0160D0EE` are polled every 120 ms and logged on
  change: 1/1 after scheduling, pending back to 0 once the callback ran.

Validation so far: `/W4` build clean. A scratch loader named `witcher2.exe`
loaded the DLL and the trace correctly refused all five IAT slots (absent in
that process) and kept retrying Steam; no crash. The live hooks have NOT yet
run in the game.

Installed 2026-10-28 20:38 with the game closed. Previous DLL/ini/log backed up
to `build/save-trace-install-backup-20261028-203827/`. Rollback: copy that
`dinput8.dll` back, or simply set `save_trace=0`.

Reading the log after one new manual save:

| Log shows | Meaning |
|---|---|
| flags never reach 1/1 | live capture not scheduled (stock path or save flow aborted) |
| 1/1 but pending never returns to 0 | capture never completed; callback not run |
| D3DX call with failing HRESULT or 0 bytes | readback/encode failure in the game's capture |
| BMP created locally, no Steam `FileWrite` for it | callback skipped transfer (storage check) |
| Steam `FileWrite ..._640x360.bmp` -> 0 | Steam rejected the write |

Documents reset (forum suggestion): superseded by the trace result below.

## Trace result: the capture finishes after the save already gave up

First traced run, 2026-10-28 20:40, one new manual save (ManualSave_0010).
Log: `docs/validation/save-trace-run1-2026-10-28.log`. All IAT and Steam hooks
installed; the game ran and saved normally.

```
20:40:37.845 Steam FileWrite "ManualSave_0010.sav" 376908 bytes -> 1
20:40:37.845 engine cloud=1 provider=0EE8E704 state=1 pending=0 queued=0
20:40:37.870 D3DXSaveSurfaceToFileInMemory source 640x360 format=22 pool=3 msaa=0
20:40:37.872 D3DXSaveSurfaceToFileInMemory fmt=0 -> 00000000, 921656 bytes
```

No BMP file call and no Steam write for the BMP followed. The capture flags
`0160D0ED/EE` never changed: the file-callback path `009B747E` is not used here.

What the code does (unpacked 3.5.0.26):

- `009B63DE` is the per-frame save tick. When a save is requested (save
  manager +0x44 != 0) it calls `009B6BC5`, which sets +0x40 = 0, +0x48 = 1 and
  queues an **in-memory** 640x360 capture into buffer +0x3C with its size
  pointer +0x40 (`00AA1C4B(buffer, &size, no callback)`).
- In the same tick, after `00AAAAA5`, it calls `009B79AD`, which writes the
  save. With +0x48 = 1 it uses `009B7149`: write buffer +0x3C only **if
  +0x40 != 0**, otherwise nothing.
- The capture runs through `00B15317` -> `00B12D8F` -> `0053CEF0`, whose
  memory branch calls `D3DXSaveSurfaceToFileInMemory`, copies the result into
  the buffer and stores the size. That happened 25 ms after the `.sav` write.

So the screenshot is fine (S_OK, a correct 32-bit 640x360 BMP); it is simply
not ready when `009B7149` checks it, and the game never looks again. That fits
every earlier observation: DXVK and native D3D9 both fail, the 4K probe passes,
and stock-image AutoSaves are unaffected. Why it once worked in 2024 is not
proven; a capture that completes within the save tick (other hardware/frame
timing) would satisfy the check.

The Documents reset from the forum is therefore unlikely to help, and is not
planned.

## Fix: late thumbnail write (`[saves] fix_thumbnails=1`, default on)

`src/enginefix.c`, section "save thumbnails". Needs only the
`D3DXSaveSurfaceToFileInMemory` IAT hook and the `ISteamRemoteStorage::FileWrite`
vtable hook from the trace; the other trace hooks stay behind `save_trace`.

1. The D3DX hook keeps a copy of every successful 640x360 BMP encode (max
   2 MB, the game's buffer size).
2. The FileWrite hook notes each `.sav` the game writes. If the game wrote
   `<name>_640x360.bmp` itself just before, nothing more happens.
3. The worker (120 ms tick) pairs a waiting `.sav` with a capture that finished
   no more than 2 s before it and within 10 s after it, then calls the original
   `FileWrite("<name>_640x360.bmp", data, size)` on the game's own interface.
   Older leftover captures are discarded; a save with no capture in 10 s is
   logged as `thumbnail: no capture arrived`.

Only the Steam storage backend is handled (the startup override keeps this
install on it). Existing saves cannot get a picture back.

Validation: `/W4` clean. A test exe that compiles `enginefix.c` directly
passed 9/9 pairing cases (late capture, game wrote BMP itself, QuickSave name,
stale capture rejected, timeout, oversized buffer, another save's BMP). Scratch
loader smoke test: hooks refuse cleanly outside the game. Installed 20:46 with
the game closed; `save_trace=1` still on for the confirming run. The first-run
log is kept in `build/save-trace-run1/`.

Expected log line after the next save:
`thumbnail: wrote ManualSave_0011_640x360.bmp (921656 bytes) for ManualSave_0011.sav -> 1`.

## Confirmed in game, 2026-10-28 20:56

Owner reports the preview now shows in the Load menu. Log:

```
20:56:55.082 Steam FileWrite "ManualSave_0011.sav" 375002 bytes -> 1
20:56:55.112 D3DXSaveSurfaceToFileInMemory fmt=0 -> 00000000, 921656 bytes
20:56:55.123 thumbnail: wrote ManualSave_0011_640x360.bmp (921656 bytes) for ManualSave_0011.sav -> 1
```

`remote/ManualSave_0011_640x360.bmp` exists, 921656 bytes. The capture again
arrived ~30 ms after the `.sav`. Whether resolution (frame time at 4K) decides
if the capture lands inside the save tick is untested.

`save_trace` set back to 0 in `dist-files/` and the installed ini after the
confirming run; the trace code stays in the DLL for later diagnosis.

## QuickSave preview in the same session

QuickSave reuses one name, `QuickSave_640x360.bmp`. On 2026-10-28 21:13 a second
QuickSave wrote the new image correctly (file time 28 ms after the `.sav`,
content checked), but the Load menu in the same session still showed the
previous QuickSave image; after a restart it showed the new one.

Cause: the save list builder `009A49B9` opens each preview through storage
provider slot +0x18 and hands it to the GUI image registry
(`DAT_0160C6EC` vtable +0x70 -> object at `DAT_0160D098 + 0x1C`, vtable `00EC6194`).
Its AddImage (slot 2, `00B28657`, thiscall `(String* name, stream)`) computes
the key with `00B30E9F(String*)` (stdcall) and returns early when the key is
already present. Remove is slot 3, `00B285C6` (thiscall `(key)`). Nothing in
the save path evicts an entry, so a reused name keeps its first image for the
session. The unmodified game would behave the same whenever its own write works.

Fix: every preview written during the session (our late write and the game's
own BMP writes) is remembered (ring of 32 names). The DLL patches vtable slot
`00EC619C` (fingerprinted: slot value and the first 12 bytes of all three
functions); when AddImage is called for a remembered name, it calls Remove with
the registry's own key first and logs `thumbnail: menu reloads <name>`, then
the original AddImage loads the new file.

Validation: unit test `tools/thumbtest` 13/13 (dedup, game BMP marked, late
QuickSave marked, ring keeps newest). Confirmed in game 2026-10-28 21:28: owner
saw the new QuickSave image in the same session; log shows
`thumbnail: menu reloads QuickSave_640x360.bmp` 7 s after the late write.
