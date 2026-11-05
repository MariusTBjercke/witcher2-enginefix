# The game hangs on 32 logical processors

Reported on Nexus (2026-11-04): after *Launch* the splash shows, then nothing. `witcher2.exe`
keeps running in the background without a window or taskbar icon. Taking one core away from
the affinity in Task Manager makes the game start.

## Cause

`FUN_00930a6e` (Steam, called once through `FUN_00932081` from `FUN_00aa6e94`) counts the CPU
topology, and looks like Intel's old sample code. It only runs when the process and system
affinity masks are equal, which is why removing a core helps: the masks differ and the whole
count is skipped.

The loop walks the mask one bit at a time:

```
00930BBD  SHL EDI,CL        ; mask = 1 << i
00930BC2  TEST EDI,EDI
00930BC4  JNZ  00930ACF     ; the top of the loop stops on mask > system mask
```

x86 masks a shift count to 5 bits, so `1 << 32` is 1 and `mask` is never 0. With a system mask
of `0xFFFFFFFF`, `mask > system mask` is never true either. A 32-bit process sees at most 32
logical processors, so any CPU with 32 or more threads gets that mask (for example a 16-core
CPU with SMT). Each round also appends to a text buffer and writes to 256-byte stack arrays.

The GOG build has the same bytes (`d3 e7 89 75 ec 85 ff 0f 85`), so it should hang the same way.

## Fix

`[stability] cpu32_guard=1` (default) hooks `GetProcessAffinityMask` (Steam slot `00CAD20C`; since
0.10.0 the slot is found by name, and on GOG too, where the CPU count code itself has not been
re-checked). When
both masks are `0xFFFFFFFF` it reports the process mask as `0x7FFFFFFF`, so the game takes
the same path as with the Task Manager trick, while its threads may still use every core.
Other CPUs get the real values. The other caller, `FUN_006b9a30`, overwrites the value with
`0xFFFFFFFF` on success.

The hook goes in from `DllMain` (the worker thread starts too late); if the slot is not filled
yet, a small thread retries for up to 3 s. The log says
`cpu32: 32 logical processors seen, the game's CPU count is skipped` when it acts.

## Status

Found by reading the code; the owner's machine has 20 threads. The reporter confirmed it on their 32-thread CPU
(Nexus, 2026-11-04): the game starts, and the log has `cpu32: 32 logical processors seen`.
