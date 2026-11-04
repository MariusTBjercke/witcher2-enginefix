# Crash capture

`[diagnostics] crash_dump`, default 1. Diagnostics only: nothing about the game's
behaviour changes, before or after a crash.

## Symptom

2026-11-01: the game closed by itself mid-play. Nothing anywhere: no
`Application Error` / WER entry in the Application event log, no display-driver
reset in the System log, no dump in `%LOCALAPPDATA%\CrashDumps`, and
`witcher2_enginefix.log` (opened with `"w"`) had already been overwritten by the
restart.

## Why nothing was recorded

`witcher2.exe` calls `SetUnhandledExceptionFilter` from a static initializer,
`FUN_00930D99` (called from `00BC7278`, i.e. during CRT init, after every imported
DLL's `DllMain`):

```
_DAT_01626688 = SetUnhandledExceptionFilter(&LAB_00930d4a);   // previous filter kept
```

The handler at `00930D4A` is `33 C0 40 C2 04 00` = `xor eax,eax; inc eax; ret 4`:
it returns `EXCEPTION_EXECUTE_HANDLER` without looking at the exception and never
chains to the saved filter. The kernel then terminates the process with the
exception code. WER is never invoked, so there is no report, event or LocalDumps
dump (a WER `LocalDumps\witcher2.exe` key would not help either). The destructor
`FUN_00930D79` restores the saved filter at shutdown.

The dbghelp imports (`StackWalk64`, `SymFromAddr`, ...) belong to `FUN_00931140`,
which is not on this path.

CRT failures (`__report_gsfailure`, `__call_reportfault` for `abort`/invalid
parameter) clear the filter themselves and call `UnhandledExceptionFilter`
directly; those do reach WER and would show up in the event log.

## What the DLL does

- `DllMain`: renames the last log to `witcher2_enginefix.prev.log`, then installs
  `crash_filter`. The game's initializer replaces it moments later. Only in `witcher2.exe` (and
  the input probe): the launcher's `bin\Configurator.exe` loads the DLL too, and until 0.9.0 it
  rotated and rewrote both logs, so opening the settings after a crash lost the crash's log
  (seen 2026-11-04). In any other program the DLL now only forwards to the system DLL.
- Worker (`crash_init`, then `crash_tick` every 120 ms): loads
  `MiniDumpWriteDump` from the already loaded `dbghelp.dll`, prunes old dumps to the
  newest 5, and calls `SetUnhandledExceptionFilter(crash_filter)`. Whatever it
  displaces (in practice Steam's `CSERHelper.dll`, which sits on top of the game's
  `00930D4A`) becomes the filter it chains to. The first 5 changes are logged as
  `crash: filter armed, chaining to <addr> (<module>+0x<off>)`.
  The first ~1.5 s after start are not covered (the worker's initial sleep).
- `crash_filter`, on an unhandled exception:
  - `crash: unhandled exception <code> at <addr> (<module>+0x<off>), thread <id>`
  - for an access violation: reading/writing/executing and the target address
  - registers, then up to 24 code addresses found on the stack (raw scan, newest
    first: mostly return addresses, some stale; the dump has the real unwind)
  - private bytes and working set (the game is 32-bit: an out-of-memory crash
    shows up here as ~3.5-4 GB private)
  - the dump, `bin\witcher2_enginefix_crash_YYYY-MM-DD_HH-MM-SS.dmp`:
    `crash_dump=1` indirectly referenced memory + thread/process data (a few MB),
    `crash_dump=2` full memory (1-3 GB)
  - then returns whatever the previous filter returns, so the game ends exactly
    as before.
- Log lines in the filter use `TryEnterCriticalSection`: if the crashing thread
  holds the log lock, they are skipped instead of deadlocking.
- Re-entry (a filter chaining back to us, or a second thread crashing) returns
  `EXCEPTION_CONTINUE_SEARCH`.

How a session ended shows in its last lines (`[diagnostics] exit_trace`, below):

- `exit: ExitProcess(0) ...` and then `unloaded`: a normal quit (Steam).
- `exit: CorExitProcess(0) ...` as the last lines: also a normal quit, with the .NET
  runtime in the process (GOG, most likely brought by Galaxy's overlay). The runtime
  ends the process without the DLL detach, so `unloaded` never comes.
- `crash:` lines: a crash, as above.
- None of these: the process was ended some other way (`TerminateProcess` from
  another module, a CRT abort, a driver/TDR, a hang closed from Task Manager, or
  power loss). Check the event logs for those.

## Exit trace

`[diagnostics] exit_trace`, default 1. Logging only; 0 turns it off without touching
the crash capture.

- In DllMain, these import slots get hooks that log
  `exit: <function>(<code>) from <module+offset> on thread N` and then call the real
  function:
  - the exe's `ExitProcess` and `TerminateProcess`;
  - the same two in the platform DLL (`Galaxy.dll` on GOG, `steam_api.dll` on Steam).

  `TerminateProcess` is logged only when the target is the game itself. A slot that does
  not hold the real export (a compatibility shim, another overlay) is left alone.
- The game's CRT ends in `__crtExitProcess` (Steam `00672371`, GOG `00743F4A`). When
  `mscoree.dll` is loaded, it first calls the .NET runtime's `CorExitProcess`, found
  through `GetProcAddress`, and calls `ExitProcess` only if that returns. So the exe's
  `GetProcAddress` slot is hooked as well. That hook hands out a logging wrapper for this
  one name, and every other lookup jumps on with the game's own return address. If
  `CorExitProcess` returns, the log says so.
- The exit lines wait for the log lock for 250 ms at most, then write straight to the
  file handle. The DLL detach does the same when the lock's owner was already ended.

See `docs/validation/gog-shutdown-2026-11-04.md`.

## Reading a dump

Offline only (no live debugger in the game process, see `docs/research.md` §6):

```
cdb -z witcher2_enginefix_crash_<...>.dmp -y "srv*C:\symbols*https://msdl.microsoft.com/download/symbols" -c ".ecxr; kb 40; lm; q"
```

`witcher2.exe` addresses map 1:1 onto the unpacked Ghidra program (image base
`0x00400000`, no ASLR).

## Verified

- Offline, 2026-11-01: a test process named `witcher2.exe` loads
  `build\dinput8.dll`, installs a filter that returns `EXCEPTION_EXECUTE_HANDLER`
  (like `00930D4A`), waits 3 s and writes to `0x10`. Log: filter armed and chaining
  to the test filter, `unhandled exception 0xC0000005 ... access violation writing
  0x00000010`, registers, stack, memory, a 0.2 MB `MDMP` dump, `passing on to the
  previous filter`; exit code `0xC0000005` as without the DLL. A clean run before it
  ended in `unloaded` and became `witcher2_enginefix.prev.log`.
- In game, 2026-11-01 03:52 (start test, normal play, clean quit): one
  `crash: filter armed, chaining to 0x6F822571 (CSERHelper.dll+0x12571)` at +1.5 s
  and no further re-arm lines, then `unloaded`. The top filter at that point is not
  the game's but Steam's `CSERHelper.dll` (from the Steam folder, loaded through
  `steam_api`/the overlay), installed after `00930D4A`; ours now runs first and then
  hands over to Steam's. Steam left no dump of the earlier crash (`Steam\dumps` does
  not exist), so the "nothing on disk" finding stands.

## Turning it off

`[diagnostics] crash_dump=0`. The `.prev.log` rotation always happens while
`[general] log=1`.
