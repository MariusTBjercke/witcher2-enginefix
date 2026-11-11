# WitcherScript bindings: how native functions are registered and called

Status: research plus in-game tests, Steam 3.5.0.26. Proved end to end in the running
game: the DLL registers a function before the compile, the `.ws` declares it with
`import function`, the compiler resolves the name, the game calls it with an `int`
argument, the native evaluates that argument out of the interpreter frame, answers with
an `int`, and the script uses the answer - a second call receives `1234` back. Three
attempts crashed; every cause is in this file.

This answers one question: **can the engine be given new script functions from
outside, without touching `witcher2.exe` on disk?** The answer is yes, with the
caveats in "What a proxy can do".

## Why this is possible at all

The game compiles `CookedPC\base_scripts.dzip`'s `.ws` text at startup
(`CompileScriptsFromSources`, `00AA69A0`), so the WitcherScript compiler ships inside
the exe. Every name a script calls is resolved against tables the engine fills before
that compile. Those tables are static `.data` arrays plus one lazily built registry
object, and there is no "finalised" flag anywhere in the registration path: a proxy
DLL can append to them before the compile call.

## Startup order

`FUN_00aa6e94` is the engine init ("Initializing RTTI...", "Initializing Havok...",
"Initializing Game..."). The relevant part, read straight from the disassembly:

| Address | Call | What |
|---|---|---|
| `00AA6F91` | `RegisterEnvironmentScriptNatives` (`00A76637`) | one native group's registration |
| `00AA6F96..00AA6FB4` | seven more `FUN_...` | other native groups |
| `00AA6FC5` | `CompileScriptsFromSources` (`00AA69A0`) | the only call site in the exe |
| `00AA7075` | `FUN_00b12ada` -> `[0x01404950]` | `CGame` is created *after* compiling |

Two consequences: a hook on `00AA69A0` runs before the compiler has read anything, and
at that moment `CGame` (`VA_GAME`) does not exist yet, so the *registration* cannot read
game state - the function itself can, once it is called later.

## The registry object

`[0x02589018]` is a lazily built heap object (`FUN_0040387c` -> `FUN_00403828` ->
`FUN_00b0fbb6`, behind a critical section and an `_atexit`). It has no vtable. It holds
four sorted growable arrays of `{CName key, value}`:

| Offset | Holds | Inserted by |
|---|---|---|
| `+0x10` | types and classes | `FUN_00b104d8` |
| `+0x24` | unknown | - |
| `+0x38` | global script functions | `FUN_00b102a4` |
| `+0x4C` | unknown | - |

Insertion is `FUN_00407d41` (binary search, then `FUN_00401f05(6, 8)` to grow), so the
container reallocs and has no capacity cap. Name lookup is `FUN_00b103e0` for types and
`FUN_00b0fa66` for global functions - the latter is `LEA EDI,[ECX+0x38]` plus a binary
search in `FUN_00408f55`, returning the value or null.

The expression `FUN_0040387c()` is how every subsystem reaches the registry; there are
no other references to `0x02589018`.

## Registering a native function (the engine's own path)

`FUN_00b0f409` builds a descriptor in caller-allocated memory. `this` is in **EDX**,
arguments are two dwords on the stack, `RET 0x8`: `(CName*, void* fn)`.

```
00b0f40f  MOV [EDX],0xE78174          ; vtable: CFunction
00b0f41b  MOV [EDX+0x8],EAX           ; CName (arg 1)
00b0f41e  MOV [EDX+0xC],3             ; kind 3 = global script function
00b0f46e  MOV ECX,[EAX+0x1C]          ; EAX = [0x0160C6B0]: next free slot
00b0f474  MOV [ECX*4+0x13F0938],ESI   ; fn pointer into the table
00b0f47b  INC [EAX+0x1C]              ; bump the counter
00b0f47e  MOV [EDX+0xA0],ECX          ; cache the index in the descriptor
```

`FUN_00abe94a(size)` is the zeroing pool allocator the caller uses for the `0xAC`
bytes, not a feature switch - the `if (iVar1 != 0)` in the decompiler is a null check.
After the descriptor, `FUN_00b102a4(reg, desc)` inserts `{name, desc}` into `+0x38`;
a duplicate name is only a warning (`"Global function '%s' already defined"`).

The tables, both static `.data`, are zeroed once by `FUN_00aba03e`:

| Symbol | Size | Entries | Meaning |
|---|---|---|---|
| `DAT_013F0938` | `0x4000` | 4096 x 4 B | kind-3 function pointers |
| `DAT_013F4938` | `0x10000` | 4096 x 16 B | kind-1 member/attribute records |

The counter at `[0x0160C6B0]+0x1C` is seeded to `0x100`, and `FUN_00ab652c` fills slots
`0..255` with the engine's own natives during the same constructor. Slots `256..4095`
are free. Nothing checks the bound, and nothing marks the tables closed.

## Calling a native at runtime

`FUN_00b0f517` is the invoker (`this` in EAX, four stack arguments, `RET 0x10`). The
kind bits at `+0xC` decide the path; for a global function:

```
00b0f558  MOV EAX,[EBX+0xA0]          ; the index cached at registration
00b0f55E  MOV EAX,[EAX*4+0x13F0938]   ; -> the function pointer
00b0f567  JZ  ...                     ; null entry = no-op, not a crash
00b0f569  PUSH [EBP+0x14]             ; arg 3
00b0f56C  PUSH ESI                    ; arg 2
00b0f56D  PUSH [EBP+0xC]              ; arg 1
00b0f570  CALL EAX                    ; cdecl
```

So a registered native is called with three stack arguments. Compiled code refers to
the descriptor object, not to a name; the name is only needed while compiling.

What the three arguments are, from the invoker's own code (`FUN_00b0f517`, kind-3
branch) and from the call we measured:

* `arg 1` is a value slot (a 16-byte struct; the engine initialises and destroys these
  with `FUN_00401faf` / `FUN_004021ee`), not a C integer.
* `arg 2` is the interpreter frame - a stack object; our probe was handed
  `0x001AC3D8` while its own stack sat at `0x001AC2C0`.
* `arg 3` was `0` in that call.
* The argument expressions are evaluated by the invoker before the call, one bytecode
  instruction at a time (`FUN_00abc328`), and the invoker ignores the function's EAX
  afterwards: the result has to go through the value slot, not through the return
  register.

That is why a stub that only logged its arguments desynchronised the interpreter.

## The native's contract

A native is called `native( ctx, frame, result )` - three stack arguments, cdecl. The
three string natives below use `ctx` not at all, take `frame` (the interpreter frame) as
the second, and a destination pointer as the third. `result` is null when the script
uses the call as a statement, and points at a 16-byte value slot otherwise.

The body has a fixed shape:

1. **One evaluation per declared parameter.** Initialise a 16-byte value slot with the
   empty-script default (`FUN_00401faf` with `0x0160FBA0`), then call `FUN_00abc328` with
   `EAX = frame` and the slot as destination: it executes exactly one bytecode
   instruction, which produces that argument.
2. **Consume one byte**: `[frame+0x18]++`. This is the call's terminator in the byte
   stream.
3. Compute.
4. If `result` is not null, store the value there: an `int` is the number at `+0` and the
   type id `2` at `+12`. Proved round trip: the native answered `1234` and the next call
   in the same script received it as its argument (`D2 04` in the log).
5. Destroy a slot **only when its type owns memory**. `FUN_004021ee` releases the
   value's payload as a heap block, so a slot holding an `int` must be left alone: the
   number sits at `+0`, and destroying it calls the allocator with 42 - which is exactly
   how the first parameter test died (`free(42)` read `[0x24]`).

Measured layout of an `int` argument (the value `42`, from a log line in game):

```
+0        2A 00 00 00   the number
+4..+11   00 ...        unused
+12       02 00 00 00   the type id (2 = int)
```

That is what a native reads and what a result has to look like.

The count of `FUN_00abc328` calls is the parameter count, measured on three natives:

| Native | Parameters | `FUN_00abc328` calls |
|---|---|---|
| `StrLen` (`0x008E514C`) | 1 | 1 |
| `StrFindFirst` (`0x008E4F15`) | 2 | 2 |
| `StrReplace` (`0x008E5E2D`) | 3 | 3 |

A native that does none of this desynchronises the interpreter: the code pointer stays
in front of the argument expressions, so the VM executes an argument byte as an opcode.
That is exactly what our stub did, and the game died at `0x00ABC351` calling through a
null table slot, one millisecond after the stub logged its arguments.

## Resolving a name at compile time

Static trace of the front end: `CompileScriptsFromSources` -> `FUN_00abc273` ->
`FUN_00ab9450` (driver) -> per source `FUN_00ab436e` -> scanner `FUN_00ab19b3` and a
LALR parser (`FUN_00ab30f3`, tables at `00E553B0`/`00E55820`).

Identifier resolution is `FUN_00b0a0de` -> scope chain `FUN_00ac2b0a` ->
`FUN_00b09d03` -> `FUN_0040387c()` + `FUN_00b0fa66`, which searches the `+0x38` array
proved above. The resolved `CScriptFunction*` is stored in the syntax node at `+0x40`.

The failure strings name the three cases: `"'%s' is not a member of '%s'"`
(`00E4BF74`), `"I dont know any '%s'"` (`00E4BEFC`), and the parser's
`"parse error, near '%s'"` (`00E4EA58`) - which is the literal source of the
`parse error, near 'DDS'` message in the script-overlay notes.

## Signatures come from the scripts, not from the descriptor

A registered native carries no signature: both builders zero `+0x10..+0x54`. The arity is
supplied by the `.ws` sources instead, with a declaration:

```
import function StrReplace( str, match, with : string ) : string;
```

`core\string.ws` holds these for the string library, `engine\*.ws` for the rest, and the
checker enforces the count they declare. That was measured the hard way: a
`W2EF_TestParam( 1, 2 )` call to a freshly registered native with no declaration stopped
the game at

```
game\gui\w2ef_books.ws[858]: Function 'W2EF_TestParam' does not take 2 param(s)
```

Two things follow. The name **did** resolve - the compiler found the entry the DLL added
to the global function table and the registry, which is the part that was unproven. And
the argument list is not free: a new function needs an `import function` line as well as
the registration, which is also how every native in the engine is declared.

## What a proxy can do

* **Add a global script function.** Allocate `0xAC`, fill it as `FUN_00b0f409` does,
  name it with `CName_FromString` (`00B28D76`), store the pointer at
  `0x13F0938 + index*4`, bump `[0x0160C6B0]+0x1C`, and insert the name with
  `FUN_00b102a4`. 3840 slots remain. The signature comes from an `import function`
  declaration in the mod's own `.ws`, not from the descriptor.
* **Timing.** Do it before `00AA6FC5`; a hook on `CompileScriptsFromSources` entry is
  enough, and the compiler then accepts calls to the name.
* **Add a class.** The type registry at `+0x10` is growable and is inserted into by the
  same `FUN_00407d41` (`FUN_00b104d8`); the generated `*ClassBuilder` static
  initialisers do exactly this before `main`, and script declarations use the same
  registry from `FUN_00ab8135`.

## The remaining limit

* **Game state at registration time.** `CGame` is created at `00AA7075`, after the
  compile, so registration cannot *decide* what to register from game data. The
  function itself runs later and can read `[0x01404950]` (`VA_GAME`) at call time, so
  the body does have access to the game once it is called.

## Unproven

* The other value types: `float`, `bool`, `string`, `name` and object references. A
  string is not a plain pointer (a 16-byte `String` is copied, and it owns memory), and
  each type's id is still to be measured - the same one-start-test method that measured
  `int` works for them.
* Thread safety: only the registry's creation takes a critical section; insert and
  lookup are unsynchronised. Boot-time single-threaded execution is an assumption.
* The GOG build: every address here is Steam's, and none of the `ga()` rows exist yet.
* That no later compile phase rejects an added name for another reason.

## What an SDK on top of this still needs

1. **A wrapper** that takes a plain C function and does the contract: evaluate N slots,
   consume the terminator, write the result, destroy only owned values. Our test native
   is that logic written out longhand for one `int`.
2. **The value type table**: the id for each type and how to read and write it.
3. **A plugin loader**, so a mod ships as a DLL that registers its natives instead of
   patching this one.
4. **GOG rows** for the six addresses, the `ga()` discipline the rest of the project
   already follows.
5. **A build story**: registration must happen before `CompileScriptsFromSources`, which
   is one known call site - the hook this file describes is that mechanism.

The cost is now visible and it is engineering, not research: the mechanism is proved,
the two failure modes are known, and the value layout is measurable the same way the
`int` one was.

## One trap worth remembering

An `import function` declaration with no matching registration **compiles** and then
crashes the game on the first call (`access violation executing 0x00000000` at
`0x00ABC351`, `ecx=0x1E`), because the compiler makes a function out of the declaration
and its implementation slot stays empty. Any tool built on this must therefore register
before the compile, and a missing registration shows up as a crash at menu time, not as
a compile error.

The second trap is the value slot. Destroying a slot whose type does not own memory
calls the allocator with the number itself: `free(42)` reads `[0x24]` and dies.
