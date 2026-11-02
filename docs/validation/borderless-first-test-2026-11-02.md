# Borderless window: first in-game test crashed (2026-11-02)

Status: root cause found and fixed; the re-test passed (second build, 2026-11-02). Evidence: the game's log and crash dump,
both still in `<game>\bin\` (`witcher2_enginefix_crash_2026-11-02_14-27-00.dmp`), plus the
analysis below. Context: [borderless-window.md](../fixes/borderless-window.md).

## What happened

The owner started the game (DXVK, 4K, `Fullscreen=1` in User.ini), reached the main menu, opened
Options > Graphics, saw the new **Full screen** / **Borderless window** rows and selected
**Borderless window** — the process died silently (the game's own handler exits without a
report; the DLL's `[diagnostics] crash_dump=1` wrote a minidump and the log lines below).

* DLL installed for the test: 0.7.0, SHA-256 `AF64A45E48E55A9ACE74C92C3CAE84FEEFE9CEDC9845CE7840885A7D2968CB3A`
* `witcher2_enginefix.log` SHA-256 `7EBB387923B498026A4E877D08A188D1545D15C03B2F970ABEDD48BCF675F45D`
* dump SHA-256 `834B4A33FEE92B91A679AA690E86A8C07382F88AABB53503F57ABDFEF8D16A8C`
* at crash time the game was still in full screen: `User.ini` had no `EngineFixBorderless` and
  `Fullscreen=1` (the writer's save happens at exit, so nothing persisted)

Relevant log lines:

```
14:26:17.703 window: startup style not patched (bytes differ or out of range) - borderless would appear after creation
14:26:17.706 window: menu channel hooked (Options > Graphics applies borderless at once)
...
14:27:00.748 crash: unhandled exception 0xC0000005 at 0x001ABBD4 (?), thread 356
14:27:00.748 crash: access violation executing 0x001ABBD4
14:27:00.748 crash: eax=00000001 ebx=0B6637C8 ecx=6FA4C3A8 edx=80000000 esi=001ABDA8 edi=3D93A720
14:27:00.748 crash: eip=001ABBD4 esp=001ABBC8 ebp=009B50EF
14:27:00.749 crash:   [esp+0008] 00AB5351 witcher2.EXE+0x6B5351
14:27:00.749 crash:   [esp+0120] 009B503A witcher2.EXE+0x5B503A
...
14:27:01.315 osd: Borderless window: turn Full screen off, then restart
```

The last OSD line is the worker thread half a second later: it had already seen
`wf_borderless = 1`, i.e. the hook ran and reported the write before the process died. So the
crash was on the way back out of the config write, not in the option handling itself.

## Root cause: the config writer is `__stdcall`, the hook was `__cdecl`

`FUN_0041F4B7` is the engine function behind the script native
`theGame.WriteConfigParamFloat`. Its epilogue is:

```
0041f579: POP EDI
0041f57a: POP ESI
0041f57b: POP EBX
0041f57c: MOV ESP,EBP
0041f57e: POP EBP
0041f57f: RET 0x10          <-- callee removes the four arguments
```

and its only known caller, the script native wrapper `FUN_009B503A`, calls it and continues
without touching ESP:

```
009b50e6: PUSH ESI
009b50e7: PUSH EDX
009b50e8: PUSH ECX
009b50e9: PUSH EAX
009b50ea: CALL 0x0041f4b7
009b50ef: MOV ECX,dword ptr [EBP + 0xc]   <-- return address, no ADD ESP afterwards
```

The 0.7.0 test build declared the hook and the trampoline pointer as `__cdecl`. The trampoline
reaches the original, whose `RET 0x10` already removed the hook's four argument bytes; the
`__cdecl` hook then removed them again on return, so ESP ended 16 bytes high. The hook's epilogue
read EBP from the wrong stack slot (the dword that happened to be there is
`0x009B50EF` — the wrapper's return address, exactly the `ebp` in the crash context) and returned
into the stack. DEP raised the access violation executing `0x001ABBD4`.

The dump's stack around `esp` shows the leftover argument block, including the float we wrote:

```
001abbc0  3f800000 001abbd4 001abcf8 013f72a8
001abbd0  00ab5351 3f800000 0b6637c8 00000005
```

`3F800000` is `1.0f`, the value of `EngineFixBorderless`.

## Second finding: the startup style patch was refused

The log's first window line says the style patch did not install. The site fingerprint had
matched, so the failure was `pb_stop_peers` refusing: while the process is still creating threads,
the freeze-to-a-fixed-point pass keeps finding new ones (or hits a thread that exits between
enumeration and `SuspendThread`). The same barrier succeeded 3 ms later for the writer site, so
it was transient. Borderless-at-start was therefore inactive in this session; only the restyle
path could have applied it.

## Fix

* `src/window_patch.h`: `wf_write_hook` and `wf_write_fn` are `__stdcall`, matching `RET 0x10`.
  (The original's return value in AL is unchanged.)
* `src/enginefix.c`: the startup style patch is retried every millisecond for up to 3 s; it stops
  as soon as the game window exists (after that the birth style no longer matters and the worker's
  restyle applies it), and the log says which case happened. The writer install is retried the
  same way.
* `tools/windowprobe/`: new native harness. It builds a synthetic `__stdcall` target with the real
  6-byte prologue and `RET 0x10`, installs the same detour through `wf_make_trampoline` (now
  shared) and checks the arguments, the return value, the notification, the stack balance and the
  style stub's registers. 15 checks.

## Verification

* `build.bat`, `sptest test --game` (23 tests), `distanceprobe` (28+9+21 checks), `check_texts`
  (12 languages): all green after the fix.
* `tools\windowprobe\build-and-run.bat`: 15 checks, 0 failures.
* The probe was also built with the hook temporarily changed back to `__cdecl`: it dies with
  `0xC0000005` (`-1073741819`), i.e. it reproduces this incident. The fix makes it pass.

## Still open

* Re-test in game with the `__stdcall` build: the same Options > Graphics toggle must now write
  the keys, persist them at exit and, after turning Full screen off, start borderless.
* Whether the engine persists the new `[Rendering] EngineFixBorderless` key in User.ini (it is
  written into the in-memory config; the file save is the engine's).
