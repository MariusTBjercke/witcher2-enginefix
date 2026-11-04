# Why the GOG log ends without `unloaded`, 2026-11-04

GOG 3.5.0.26g, vanilla scripts plus A Better UI, the shipped ini, the 0.10.0 working build with the
shutdown diagnostics (`docs/fixes/gog-build.md`, "Open"). The owner started the game from GOG
Galaxy, loaded a save, played briefly and quit through the pause menu's Quit. No dump.

## What the build logs

* `exit:` with the caller, when the exe or the platform DLL (`Galaxy.dll` / `steam_api.dll`)
  calls `ExitProcess` or `TerminateProcess` on itself (their import slots, found by name).
* At the DLL detach, when the log lock is held by a thread Windows has already ended, a last
  `unloaded (...)` line written straight to the file handle.
* The quit guard's first catch from the next worker tick, not only in the total at the detach.

## The run (13:41-13:43)

```text
build: GOG 3.5.0.26g (TimeDateStamp 644689F9, .text 008FC18D), delta 0x00930000 from 0x00400000; import slots 25/26 found by name, not imported: SteamRemoteStorage
exit: hooked kernel32.dll!ExitProcess
exit: hooked kernel32.dll!TerminateProcess
exit (Galaxy.dll): hooked kernel32.dll!ExitProcess
exit (Galaxy.dll): hooked kernel32.dll!TerminateProcess
crash: filter armed, chaining to 0x01336244 (witcher2.EXE+0x606244)
stability: quit guard installed (FactsAdd time default at 00E3B0A9)
crash: filter armed, chaining to 0x7512D7C0 (mscoreei.dll+0x4D7C0)
input: DirectInput8Create (A) from proxydinput_Win32_Release.dll+0x2D6E: ...
...
distance stats: ...
crash: filter armed, chaining to 0x01077DE4 (witcher2.EXE+0x347DE4)
```

The log ends there: no `exit:` line, no `unloaded` and no raw marker. So neither the exe nor
`Galaxy.dll` called `ExitProcess` or `TerminateProcess` through its import table, and the DLL's
detach never ran. (No quit guard catch either; it depends on where the save was.)

## Why

The game's static CRT ends the process in `__crtExitProcess` (GOG `00743F4A`, Steam `00672371`),
the same code on both builds:

```text
__crtCorExitProcess (GOG 00743F1F, Steam 00672346):
  push L"mscoree.dll"; call [GetModuleHandleW]; test eax,eax; jz out
  push "CorExitProcess"; push eax; call [GetProcAddress]; test eax,eax; jz out
  push [ebp+8]; call eax
__crtExitProcess:
  push [ebp+8]; call __crtCorExitProcess; pop ecx
  push [ebp+8]; call [ExitProcess]
```

When the .NET runtime is loaded in the process, the CRT hands the exit to its `CorExitProcess`.
Only if that returns does it call `ExitProcess`. In the GOG process the runtime is there: the
top-level filter chain shows `mscoreei.dll`, about when Galaxy's overlay input proxy
(`proxydinput_Win32_Release.dll`) starts. That makes the overlay the likely source, but it is
not proven. `CorExitProcess` did not return, since no `exit: ExitProcess` line followed, and the
process ended without the DLL detach. The Steam process has no .NET runtime (no `mscoree` in its
logs). There the same CRT calls `ExitProcess`
directly, and `unloaded` follows the CRT's filter change by about 100 ms:

```text
13:14:08.900 crash: filter armed, chaining to 0x00676B04 (witcher2.EXE+0x276B04)
13:14:08.999 unloaded
```

The orphaned log lock that 0.10.0 first suspected is not the cause: the detach did not run at all.
The first GOG run of the day (`gog-phase0-2026-11-04.md`, run 1, main menu only) did end with
`unloaded`. In that run either .NET was not loaded yet, or `CorExitProcess` returned.

## Change

The exe's `GetProcAddress` slot is hooked too. A lookup of `CorExitProcess` gets a wrapper that
logs `exit: CorExitProcess(...)` with its caller and the quit guard total, then calls the
runtime's function. That puts the end of the log in place before .NET ends the process.
Confirmed in the next GOG run (`gog-doors-2026-11-04.md`, "The end of the log"). Since then the
wrapper also logs when `CorExitProcess` returns, and the exit lines wait for the log lock for
250 ms at most.
