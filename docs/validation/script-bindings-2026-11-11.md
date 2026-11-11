# Script bindings: the in-game runs (2026-11-11)

Steam 3.5.0.26. Evidence for `docs/engine-script-bindings.md`. Fifteen starts in all;
every one used `[diagnostics] script_bindings=1` except where noted, and the probe was
called from the main menu build (`continue_button.ws` inserts the call into
`InsertMenuButton`'s position in `guimainmenu.ws`). One start (18:51) hung before the menu
when two patch barriers ran at once; it is the second-to-last section below.

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

## Run 10 - a native reads engine state: menu

`W2EF_MeshDistance` (`src/enginefix.c`) returns `MeshDistanceScale` from the rendering
settings object. The probe called it from `FillMainMenu` and fed the result into
`W2EF_Num`:

```
18:15:44.267  hooked CompileScriptsFromSources at 00AA69A0
18:15:45.203  W2EF_MeshDistance registered at slot 1694
18:15:45.674  settings object 0x02580E78 found: mesh=1.200 foliage=2.000
18:15:45.674  lod enforced: mesh=2.000 foliage=2.000
18:16:00.639  enter W2EF_Num
18:16:00.639  enter W2EF_MeshDistance
18:16:00.639  MeshDistanceScale 2.000 (foliage 2.000)
18:16:00.639  float 2
```

The outer `W2EF_Num` enters first and evaluates its argument, which is the inner
`W2EF_MeshDistance` call; `float 2` is that argument arriving in the outer body. The read
value is the one the LOD code had just enforced (2.000), not the 1.200 the object carried
before, so the native reached the same object the DLL writes. Main menu reached, exit at
18:16:03 clean, no crash dump.

## Run 11 - a name is one dword; `thePlayer` is null at the menu

Probes for the two unmeasured types. The name went out and back through `NameToString`:

```
18:55:43.040  W2EF_ProbeName registered at slot 1695
18:55:43.040  W2EF_ProbeObject registered at slot 1696
18:55:56.605  enter W2EF_Echo
18:55:56.605  name slot 00004DD8 00000000 00000000 00000000
18:55:56.605  string "SortTypeBook"
18:55:56.605  object slot 1 00000000 00000000 00000000 00000000 ptr 00000000 refs 0
18:55:56.605  object slot 2 00000000 00000000 00000000 00000000 ptr 00000000 refs 0
```

The name slot is a single CName dword with the other 12 bytes zero, and the round trip
produced the original text. The object probe got null twice because `thePlayer` does not
exist at the main menu; a null reference is a valid value (zero pointer) but says nothing
about a live object.

## Run 12 - an object reference holds one reference per argument

`theGame` (a `CGame`, alive from engine init) instead of `thePlayer`:

```
18:59:59.860  object slot 1 0B1E2970 00000000 00000000 00000000 ptr 0B1E2970 refs 4
18:59:59.860  object slot 2 0B1E2970 00000000 00000000 00000000 ptr 0B1E2970 refs 5
18:59:59.860  object refs after release 3
```

So the slot is one pointer dword, the object's own first dword is its reference count,
**each evaluation hands out one owned reference** (4 then 5), and the two releases through
`FUN_00ABC99F` took it back to the baseline of 3. That is the contract `ThrowEntity`
(`0x00A72721`, a kind-3 native) implements in the engine.

## Run 13 - the wrapper natives: name and object through `sb_call`

The probes became the permanent `W2EF_Name` / `W2EF_Object` natives (`SB_ARG_NAME`,
`SB_ARG_OBJECT`), with a statement call and a nested one:

```
19:01:48.651  W2EF_Name registered at slot 1695
19:01:48.651  W2EF_Object registered at slot 1696
19:01:59.378  enter W2EF_Echo
19:01:59.378  enter W2EF_Name
19:01:59.378  name 00004DD9 (slot 00004DD9 00000000 00000000 00000000)
19:01:59.378  string "SortTypeBook"
19:01:59.378  enter W2EF_Object
19:01:59.378  object 0B012970 refs 4
19:01:59.378  enter W2EF_Object
19:01:59.378  enter W2EF_Object
19:01:59.378  object 0B012970 refs 4
19:01:59.378  object 0B012970 refs 4
```

Three object calls: the statement call releases its argument, the nested one moves the
inner reference into the result and the outer releases its argument. Every body saw refs 4
- the baseline of 3 plus its own argument. A leaked reference would have read 5 and then
6, and a result that copied instead of moving would have shown an extra reference.

## A start that hung before the menu (18:51)

A start with the same build never reached the menu. The log stopped at
`startup: fast_start: FMOD waits ... after 89593 ms` and had no `script bindings` lines;
all seven game threads sat in `Wait/Suspended` when inspected. Two patch installs had
frozen the process at the same moment and suspended each other, so no hook was installed.
That is the concurrency bug in the shared peer-stop barrier, now fixed with `g_pbLock`
(`docs/research.md`, "Peer-stop barrier at startup"); runs 12 and 13 installed normally.

## Run 14 - a plugin DLL registers its own native: menu

A start with `plugin-hello.dll` in `<game>\bin\witcher2_enginefix_plugins\`;
`tools\plugin-example\plugin.c` registers `W2EF_PluginHello( value : float ) : float`, and
the probe called `W2EF_Num( W2EF_PluginHello( 41.0 ) )`:

```
21:24:26.822  script bindings: W2EF_PluginHello registered at slot 1697
21:24:26.822  plugin: hello: registered W2EF_PluginHello
21:24:26.822  plugins: plugin-hello.dll loaded
21:24:26.822  plugins: 1 of 1 loaded
21:24:42.276  plugin: hello: called with 41
21:24:42.276  script bindings: enter W2EF_Num
21:24:42.276  script bindings: float 42
```

The host loaded the DLL after its own six registrations, the plugin registered through
`api->register_native` (slot 1697), and its body returned 42 into the next native, which
logged it. The plugin was removed again after the test.

## State after the runs

The probe was removed from `book_helpers.ws` and `continue_button.ws` again,
`script_bindings` is back to 0, and the build in `bin` is the normal one. The hook stays
in `src/enginefix.c` behind the flag, Steam only.
