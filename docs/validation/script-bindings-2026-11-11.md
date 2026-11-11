# Script bindings: the in-game runs (2026-11-11)

Steam 3.5.0.26. Evidence for `docs/engine-script-bindings.md`. Five starts, all with
`[diagnostics] script_bindings=1` except where noted, and the probe called from the main
menu build (`continue_button.ws` inserts the call into `InsertMenuButton`'s position in
`guimainmenu.ws`).

## Run 1 - two parameters, no operand consumption: crash

The native was a plain `int f(int,int,int)` that only logged. It was entered, the
compiler had accepted the name and the `import function` declaration, then the
interpreter died:

```
16:44:06  hooked CompileScriptsFromSources at 00AA69A0
16:44:06  W2EF_TestParam registered at slot 1691
16:44:18  W2EF_TestParam(1060415040, 1753848, 0) called
16:44:18  access violation executing 0x00000000, eip=00000000, ecx=0000001E
```

The second argument, `1753848` = `0x001AC3D8`, sits on the stack next to
`esp=0x001AC2C0`: it is the interpreter frame. The fault is the dispatch at `0x00ABC351`
(`CALL [ECX*4+0x13F0938]`) reading a data byte as an opcode, because the native never
advanced `[frame+0x18]`.

## Run 2 - declared, not registered: the same crash

The ini still said `script_bindings=0` (an install mistake), so nothing was registered.
The run produced **no** `script bindings` line and the same crash at the same registers
(`ecx=0x1E`, `esp=0x001AC2C0`).

Consequence: an `import function` declaration with no registration compiles and crashes
on the first call. A missing registration is a crash at menu time, not a compile error.

## Run 3 - no parameters, terminator consumed: menu

The native only did `[frame+0x18]++`:

```
16:57:22  hooked CompileScriptsFromSources at 00AA69A0
16:57:23  W2EF_TestParam registered at slot 1691
16:57:36  W2EF_TestParam called
```

Main menu reached, no crash. The terminator rule is right.

## Run 4 - one int parameter, with destroy: crash

Following `StrLen` (`0x008E514C`): initialise the slot, evaluate one argument, consume
the terminator, log, destroy. The argument arrived correctly and the destroy killed it:

```
17:37:42  argument slot 2A 00 00 00 00 00 00 00 00 00 00 00 02 00 00 00
17:37:42  access violation reading 0x00000024 at 0x0066F7B3, eax=00000028
```

`0x0066F7B3` is `push [eax-4]` in the block free path: the destructor read `42` as a
pointer. **An int value is the number at `+0` and the type id `2` at `+12`; it owns no
memory and must not be destroyed.**

## Run 5 - the same without the destroy call: menu

```
17:38:57  hooked CompileScriptsFromSources at 00AA69A0
17:38:58  W2EF_TestParam registered at slot 1691
17:39:11  argument slot 2A 00 00 00 00 00 00 00 00 00 00 00 02 00 00 00
```

Main menu reached. Registration, declaration, resolution, call and one `int` argument
are proved in the running game.

## Run 6 - an int result back into the script: menu

The declaration became `import function W2EF_TestParam( value : int ) : int;`, the native
writes `1234` and the type id `2` into argument 3, and the probe feeds the answer into a
second call:

```
n = W2EF_TestParam( 42 );
W2EF_TestParam( n );
```

```
17:44:10  hooked CompileScriptsFromSources at 00AA69A0
17:44:11  W2EF_TestParam registered at slot 1691
17:44:24  argument slot 2A 00 00 00 00 00 00 00 00 00 00 00 02 00 00 00
17:44:24  argument slot D2 04 00 00 00 00 00 00 00 00 00 00 02 00 00 00
```

`2A` = 42 in, `D2 04` = 1234 back as the next call's argument. The result crosses the VM
correctly, so the contract is complete for `int`: arguments in, result out.

## Run 7 - three types, then a heap corruption: a bug in the wrapper

The wrapper was refactored into plain C with naked shims, and three natives were
registered (`W2EF_Echo` string, `W2EF_Num` float, `W2EF_Flag` bool). The game died with
`0xc0000374` (STATUS_HEAP_CORRUPTION) after `enter W2EF_Num` and before the body ran.

Cause: the hand-written version zeroed the 16-byte slot before calling `FUN_00401faf`,
the C version did not. That helper *releases* `[slot+0]` before it writes, so an
uninitialised slot frees whatever the stack held. Fixed with a `memset` before the init.

## Run 8 - all four value types, both directions: menu

```
18:03:52  W2EF_Echo registered at slot 1691
18:03:52  W2EF_Num registered at slot 1692
18:03:52  W2EF_Flag registered at slot 1693
18:04:04  enter W2EF_Num      enter W2EF_Num      float 1.5   float 1.5
18:04:04  enter W2EF_Flag     enter W2EF_Flag     bool 1      bool 1
18:04:04  enter W2EF_Echo     enter W2EF_Echo     string "hei"  string "hei"
```

Each function is called nested (`W2EF_Num( W2EF_Num( 1.5 ) )`), so the inner result is
the outer call's argument: the outer line proves the result crossed back. `string` also
proves the deep copy into the result slot and the release of the argument slot. Main
menu reached, no crash.

## State after the runs

The probe was removed from `book_helpers.ws` and `continue_button.ws` again,
`script_bindings` is back to 0, and the build in `bin` is the normal one. The hook stays
in `src/enginefix.c` behind the flag, Steam only.
