# Crash: physics ray cast against a freed object (leaving the Flotsam inn)

Status 2026-11-02: cause identified from the minidump, guard `[stability] raycast_guard=1`
implemented; the owner's start test passed and two later runs went in and out of the inn without
a crash, but the guard has not been seen to trigger. Target: Steam Enhanced Edition 3.5.0.26.

## Symptom

During the load-time test (`docs/fixes/load-times.md`, run 2) the game closed while Geralt
walked out of the Flotsam inn through its streaming door, 0.8 s into the door task. The DLL's
crash capture logged it and wrote a minidump
(`<game>\bin\witcher2_enginefix_crash_2026-11-02_18-02-55.dmp`, not committed); log lines in
`docs/validation/load-trace-run2-doors-2026-11-02.log`.

```
crash: unhandled exception 0xC0000005 at 0x0084C7AF (witcher2.EXE+0x44C7AF), thread 20280
crash: access violation reading 0xFFFFFFFF
crash: eax=17CDFDE0 ebx=5947BE03 ecx=00000000 edx=3E196900 esi=67091E3C edi=17CDFD90
```

## Analysis (offline, `cdb -z` on the dump; no live debugging)

* `0084C7AF` is `movaps xmm1, [esi+30h]` in `FUN_0084c750`. An unaligned `movaps` raises a
  general-protection fault that Windows reports as an access violation at `0xFFFFFFFF`; `esi+30h`
  = `67091E6C` is not 16-byte aligned.
* `FUN_0084c750` is slot 1 of a ray-cast collector (vtable `00D41A34`, `[edi]` at the crash is
  `00D41A34`), thiscall `(handle, index)`, `RET 8`, returns the hit fraction in ST0. It is called
  per broadphase object from the world ray cast `FUN_00625400` (`mov edx,[edx+4]; ... call edx` at `006258DB`).
* It computes the object as `handle + (signed char)handle[5]` (owner offset), reads the shape at
  `owner+0` and the transform pointer at `owner+8`. At the crash `ebx = owner = 5947BE03`: an **odd**
  address, inside a block of floats (`00 00 80 3F` = 1.0 seen three bytes off). `esi = [owner+8] =
  67091E3C` lies **inside `nvgpucomp32.dll`** (the NVIDIA driver image, `!address`). Neither can be
  a live physics object: the handle was freed and its memory reused.
* Thread 20280 is a game worker thread (start `0066F607`) that also streams data; the main thread
  was switching the inn's layer out (door task type 1, see load-times). A ray cast on the worker
  met an object the layer unload had just freed - a race in the game.

The trace's hooks (ReadFile/Sleep/WaitForSingleObject/unpack/Present/door timer) only count and
pass through, and none of them touches physics memory, so the logging is not the cause.

## Fix (`[stability] raycast_guard=1`, default on)

`src/enginefix.c`, section "ray cast guard". The worker tick replaces `.rdata` slot `00D41A38`
(holding `0084C750`, verified before the swap) with `hook_RayHit`, which calls the original inside
`__try`. On `EXCEPTION_ACCESS_VIOLATION` it returns `*(float*)(*(this+0xC)+0x10)` - exactly what
the callback's own "no hit" exits return (`0084C8CA`/`0084C8D9`/`0084C8E8`) - so the ray simply
misses the object that is gone, and logs
`stability: ray cast hit a freed physics object (handle ..., thread ...) - skipped, N so far`
(first five, then at powers of two). Valid calls take the same path as before; other exceptions are
not caught.

Limits: it removes this crash, not the race. Other code that reads the same freed object could
still fail elsewhere; a later crash at a different address would be a separate entry.

Turn off: `raycast_guard=0`.

### GOG 3.5.0.26g

The collector class is `hkpSimpleWorldRayCaster` by RTTI on both builds (vtable Steam `00D41A34`,
GOG `00E0873C`). GOG's slot 1 (`00E08740`) holds `0091F110`, whose 0x1A5-byte body is identical to
Steam's `0084C750`: thiscall `(handle, index)`, `RET 8`, the hit fraction in ST0, the "no hit"
exits returning `*(float*)(*(this+0xC)+0x10)`, and the same faulting `movaps xmm1,[esi+30h]` at
`0091F16F`. The caller is `hkp3AxisSweep` slot 22, `FUN_006f6290`, call at `006F676B`. The guard
only compares the slot with `ga(VA_RAY_HIT)`; the loader relocates both alike.

## Verification

* Build clean (`/W4`). Start test passed (owner, 2026-11-02, runs 3 and 4): the log shows
  `stability: ray cast guard installed (collector slot 00D41A38)`
  (`docs/validation/load-trace-run3-fast-doors-2026-11-02.log`, `...run4-first-door...`).
* In game: walk in and out of the Flotsam inn a few times. The crash was seen once; it is a race,
  so "no crash" is evidence, not proof. A `stability: ray cast hit a freed physics object` line
  would show the guard caught one. Runs 3 and 4 went through the inn door without a crash and
  without that line.
