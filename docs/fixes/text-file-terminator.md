# Crash reading a UTF-16 text file: one terminator byte short

`[stability] text_guard=1` (default). Built 2026-11-07; not yet seen in game.

## Symptom

Steam, 2026-11-07, 14:57:23: the game closed right after loading a save in Flotsam
(`partition: switch returned 1 after 3976 ms`, then 0.2 s later). `crash_dump` logged:

```
crash: unhandled exception 0xC0000005 at 0x004020F9 (witcher2.EXE+0x20F9), thread 32324
crash: access violation reading 0x6AB00000
crash: eax=6AB00000 ebx=6AAF1C02 ... esi=6AAF1C02
crash:   [esp+0008] 00AC6AFB witcher2.EXE+0x6C6AFB
```

The text at `ESI` was `Index;Name.1;Wiedza_Trupojady.2;...`: `globals\knowledge_list.csv`, a
UTF-16 file. Its name was on the stack as well. `004020F9` is the `wcslen` loop of the String
assignment `FUN_004020e3`; it ran from `6AAF1C02` to the end of the page at `6AB00000`.

## Cause

The whole-file text reader `FUN_00ac6aae` (Steam; GOG `00B1B4AB`) has three callers
(`00ac6cbc`, `00414bb8`, `00b263b6`):

```
00ac6ac1  ff 50 0c        call [eax+0x0C]      ; file size -> EDI
00ac6ac6  8d 47 01        lea  eax,[edi+1]
00ac6ac9  50              push eax
00ac6aca  e8 ...          call 0040315F        ; pool allocator, cdecl (size)
00ac6acf  59              pop  ecx
00ac6ad0  8b f0           mov  esi,eax
          ...             call [file+4] (esi, edi)   ; read size bytes
00ac6adb  c6 04 3e 00     mov  byte [esi+edi],0
          ...             FF FE? -> FUN_004020e3(esi + 2) as a wide string
                          else   -> locale conversion of the 8-bit text
```

The allocator `FUN_0040315f` zeroes exactly the size it is asked for. So a UTF-16 file gets
one 0 byte after its data, and a wide string needs a 0x0000 on an even offset. Whether the copy
stops depends on the byte after the block, which still holds whatever the pool held before.
Usually it is 0, which is why the game mostly works. This time it was not, and the copy ran
58 KB on to the end of the page.

This is the game's code and happens without Engine Fix. Engine Fix changes how memory is laid
out (loaded interiors, other allocations), so it can change how often the byte after the block
is not 0.

## Fix

The allocator call (`00AC6ACA`; GOG `00B1B4C2`) goes to `text_alloc_hook`. The hook asks for
2 bytes more and zeroes them. With the game's own 0, a text then ends in three 0 bytes, which
ends a wide string at an even offset for both even and odd file sizes. The read still writes
`size` bytes. As before, the reader never frees the buffer. A size of 0 and sizes near 4 GB pass
through unchanged.

Checks before patching, per build:

| | Steam | GOG 3.5.0.26g |
|---|---|---|
| Reader | `00AC6AAE` | `00B1B4AB` |
| Before the call | `8d 47 01 50 e8` (size EDI) | `8d 46 01 50 e8` (size ESI) |
| After the call | `59 8b f0 8b 03 57 56 8b cb ff 50 04 c6 04 3e 00` | `59 8b f8 8b 03 56 57 8b cb ff 50 04 c6 04 37 00` |
| Allocator | `0040315F` | `00403268` (cdecl, one argument, `pop ecx` after) |

Both builds have exactly one reader that checks for the `FF FE` byte order mark this way
(a byte search of each exe for `cmp byte [reg],FF / jnz / cmp byte [reg+1],FE`, followed by
the UTF-8 `EF BB BF` check). On GOG the allocator's body was not read: the call site, the
argument and the caller's cleanup match Steam.

Log: `stability: text guard installed (text files end in three 0 bytes, allocation at 00AC6ACA)`.
If the bytes differ: `stability: text file reader not recognised at ... - text_guard off`.

## Not proven

- The fix has not run in game yet. Needed: the install line in the log, and loads and area
  changes without problems (every CSV read goes through it).
- The crash is not reproducible on demand, so the evidence will be no more crashes at
  `004020F9` from `00AC6AFB`.
