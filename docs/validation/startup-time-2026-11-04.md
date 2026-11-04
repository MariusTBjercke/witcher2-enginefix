# Startup time, 2026-11-04

Setup: Steam 3.5.0.26, the owner's modded install (23 archives in `CookedPC`), DXVK 3.1.1, 3840x2160,
Intel Core Ultra 7 265KF, RTX 5070 Ti, warm file cache, `skip_intro=1`, many saves (165 `.sav` files in
Steam storage). `witcher2_enginefix` 0.10.0 plus the working tree of `[startup] fast_start`. See
`docs/fixes/startup-time.md`.

Each run was started by the owner through the launcher, with `tools/bootprofile/bootprofile.exe`
sampling for 40 s. The captures (2-5 MB each) are not kept. Times count from the process being found,
within ~70 ms of the DLL's first log line.

| Run | Build | Window | Menu reached | Sound step (`00AA7179`) | Scripts (`00AA6FCA`) |
|---|---|---|---|---|---|
| 1 | Original (0.10.0) | +15 046 ms | 22:45:17.501 -> 22:45:33.615 = 16.1 s | 10 932 ms | 2 051 ms |
| 2 | `timeBeginPeriod(1)` | +8 754 ms | 22:51:48.671 -> 22:51:58.388 = 9.7 s | 4 956 ms | 2 093 ms |
| 3 | + `fmod_event.dll` Sleep | +8 292 ms | 22:56:39.218 -> 22:56:48.455 = 9.2 s | 4 442 ms | 2 053 ms |
| 4 | + `fmodex.dll` Sleep | +5 484 ms | 22:59:40.873 -> 22:59:47.333 = 6.5 s | 1 747 ms | 2 044 ms |

Run 0, the same original build without the sampler, from the log: 22:29:59.407 -> 22:30:17.289 = 17.9 s.
The nine runs from 2026-11-02 in this folder that log both lines take 16.6-17.9 s. So run 1 is at the
fast end of the original's range, and the comparison is, if anything, conservative.

## Process CPU

In run 1 the capture's per-second `C` lines show main thread and process at 0 % of one core from +5 s
to +14 s, after 85-96 % in the first four seconds. In run 4 the same idle stretch is about one second
(+4 s to +5 s).

## Log lines

Run 3:

```
startup: fast_start: timeBeginPeriod(1) = 0, Sleep(1) took 15.8 ms before, 1.9 ms after
startup: fast_start: 2234 FMOD waits of 10 ms cut to 1 ms in 8218 ms, until the game window
```

Run 4:

```
startup: fast_start: timeBeginPeriod(1) = 0, Sleep(1) took 11.2 ms before, 1.9 ms after
startup: hooked kernel32.dll!Sleep
startup: hooked kernel32.dll!Sleep
startup: fast_start: FMOD waits of 10 ms cut to 1 ms until the game window, after 5437 ms: fmodex 1673, fmod_event 835
exit: ExitProcess(0) from witcher2.EXE+0x272388 on thread 41728 (the main thread)
```

## Seen by the owner

Run 3: "Ja, det virket normalt" (start and sound). Run 4 reached the menu and exited with
`ExitProcess(0)` and no new dump.

Run 5, the same DLL as run 4, without the sampler: music and sound normal in the main menu and after
loading a save ("lyden var normal i hovedmeny og inne i spillet"). From the log:

```
23:06:53.467 witcher2_enginefix 0.10.0 loaded (image base 0x00400000)
23:06:53.476 startup: fast_start: timeBeginPeriod(1) = 0, Sleep(1) took 3.4 ms before, 1.9 ms after
23:06:58.855 startup: fast_start: FMOD waits of 10 ms cut to 1 ms until the game window, after 5375 ms: fmodex 1572, fmod_event 791
23:06:59.808 startup: 1 startup movie(s) stopped, main menu reached
23:07:22.450 exit: ExitProcess(0) from witcher2.EXE+0x272388 on thread 44392 (the main thread)
```

That is 6.3 s to the menu, no new dump.

## Main thread stack in the idle time (run 1)

```
ntdll (NtDelayExecution) < KERNELBASE!Sleep < fmod_event.dll+113B < +3626D < +36F77 < +10158 < +11604
  < +10C85 < 00A9DCD7 < fmod_event.dll+2E7BC < 00A9DF61 < ... < 00A5A1D1 < 00A5A23B < 00A5A26A < 00A5A291
  < ... < 00A5A2EE < 00A5A3A8
```

## FMOD files

`fmodex.dll` and `fmod_event.dll` have the same SHA-256 on the Steam and GOG installs (start of the
hash: `7a223a66a42a9099`, `b6dc07d8c5d307d1`).
