# Plugins: adding script functions from your own DLL

Status: implemented and tested in the running game, 2026-11-11, Steam 3.5.0.26. The loader runs
only with `[diagnostics] script_bindings=1`, which is **off** in the release; nothing in this file
happens on a normal start.

A plugin is a 32-bit DLL that registers global WitcherScript functions from outside
`witcher2.exe`, using the same registration path as the built-in `W2EF_*` natives. The contract a
native implements is in `docs/engine-script-bindings.md`; the ABI is in `src/w2ef_plugin.h`.

## Where and when

* Folder: `<game>\bin\witcher2_enginefix_plugins\` (next to `dinput8.dll`). It is **not** created
  by the DLL; a missing folder simply loads nothing. Every `*.dll` in it is loaded, in file name
  order.
* The load happens at the entry of `CompileScriptsFromSources`, after SteamStub has decrypted
  `.text` and before the game compiles `base_scripts.dzip`. That is early enough for everything
  that starts later - the script compiler, `CGame`, the renderer, streaming, the UI and gameplay -
  and too late for what already ran: CRT init, SteamStub and the earlier engine init.
* A plugin is native code inside the game process, so it can also hook or patch code by itself.
  The loader only standardises the script side.
* `CGame` does not exist yet at load time, and neither does the settings object. Registration
  cannot read game state; a native body can, at call time (see `W2EF_MeshDistance`).

## The interface

`src/w2ef_plugin.h` is the complete ABI. One export:

```c
int __cdecl W2EF_PluginInit(const w2ef_plugin_api* api);   // nonzero = loaded
```

and the API struct:

| Field | What it does |
|---|---|
| `register_native(name, fn)` | registers a global script function; 1 on success. Call it during init, before the compile. |
| `call(ctx, frame, result, kinds, nargs, body)` | the native's contract once: evaluate the arguments by `kinds`, consume the terminator, run `body`, release the arguments. |
| `free_string` / `copy_string` | release a string slot / deep-copy one. |
| `free_object` | release the one reference an object argument holds. |
| `log(fmt, ...)` | one line in `witcher2_enginefix.log`, prefixed `plugin: `. |

`w2ef_value` is the VM's 16-byte value slot. The kinds are `W2EF_ARG_PLAIN` (int, float, bool),
`W2EF_ARG_STRING`, `W2EF_ARG_NAME` and `W2EF_ARG_OBJECT`; see the value table in
`docs/engine-script-bindings.md` for what a slot holds and which kinds are released.

A body has the shape our own natives use:

```c
static void __cdecl hello_body(void* ctx, char* frame, w2ef_value* args, void* result)
{
    float value = *(const float*)args[0].b;     // the evaluated argument

    if (result) *(float*)result = value + 1;   // null when the call is a statement
}

static void __cdecl hello(void* ctx, char* frame, void* result)   // the registered native
{
    static const unsigned char kinds[1] = { W2EF_ARG_PLAIN };
    g_api->call(ctx, frame, result, kinds, 1, hello_body);
}
```

### The `.ws` side is not the plugin's

Registering a name only tells the compiler what the name means. A script can call it only if the
compiled sources declare it:

```ws
import function W2EF_PluginHello( value : float ) : float;
```

The loader cannot add that line, so a plugin must ship its declaration through the script overlay
(`src/scriptpatches`, which replaces entries in `base_scripts.dzip`) or another mounted script
file. Registration without a declaration is invisible to scripts; the reverse - a declaration
without a registration - **compiles and then crashes the menu** on the first call (see the trap in
`docs/engine-script-bindings.md`).

## Example

`tools/plugin-example/plugin.c` is a complete plugin: it registers
`W2EF_PluginHello( value : float ) : float`, logs its argument and returns `value + 1`.

```powershell
cmd /c tools\plugin-example\build.bat        # -> build\plugin-hello.dll
New-Item -ItemType Directory -Force "<game>\bin\witcher2_enginefix_plugins"
Copy-Item build\plugin-hello.dll "<game>\bin\witcher2_enginefix_plugins\"
```

then `[diagnostics] script_bindings=1` in `<game>\bin\witcher2_enginefix.ini`, a `.ws`
declaration as above, and the cache deleted. The log shows the load and the registration:

```
plugins: plugin-hello.dll loaded
plugin: hello: registered W2EF_PluginHello
script bindings: W2EF_PluginHello registered at slot 1697
```

## Reporting and limits

* The log names what happened per file: `LoadedLibrary failed`, `no W2EF_PluginInit export`,
  `W2EF_PluginInit returned 0`, and the last line counts them (`plugins: 2 of 3 loaded`).
* A plugin is trusted native code: one that crashes or hangs takes the game down before the menu.
  There is no sandbox and none is possible in-process.
* The DLL stays loaded for the session (that is what keeps the native's code valid). Copying over
  it while the game runs fails as usual.
* Registering after the compile is too late for the compiler; do it in the init callback.
* Steam 3.5.0.26 only for now: the registration addresses have no GOG rows.

## Tested in game (2026-11-11)

Run 14 in `docs/validation/script-bindings-2026-11-11.md`: the example plugin loaded, its
native was registered at slot 1697, and a script call returned its value into another
native (log: `plugins: plugin-hello.dll loaded`, `plugin: hello: called with 41`,
`script bindings: float 42`).
