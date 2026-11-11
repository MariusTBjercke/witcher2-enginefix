// The plugin interface of the script bindings ([diagnostics] script_bindings=1).
//
// A plugin is a 32-bit DLL that the host loads before the game compiles base_scripts.dzip, from
// <the folder of dinput8.dll>\witcher2_enginefix_plugins\. The host calls the one export:
//
//     int __cdecl W2EF_PluginInit(const w2ef_plugin_api* api);   // nonzero = loaded
//
// Through the API a plugin registers its own global script functions (the native's contract is in
// docs/engine-script-bindings.md) and logs to witcher2_enginefix.log. A .ws that calls one of them
// still needs its own `import function` declaration in the compiled sources - the loader only
// registers the name.
//
// Timing: the load happens inside the engine init, after SteamStub has decrypted .text and before
// CompileScriptsFromSources and the creation of CGame. Code that has not run yet can be patched;
// CRT init, SteamStub and the earlier engine init cannot. CGame does not exist at load time, and
// neither does the settings object - read game state from a native's body, at call time.
//
// A plugin is native code inside the game process and can do anything a DLL can. Only load
// plugins you trust; a crashing plugin takes the game down before the menu.
#ifndef W2EF_PLUGIN_H
#define W2EF_PLUGIN_H

#define W2EF_PLUGIN_API_VERSION 1u
#define W2EF_PLUGIN_INIT        "W2EF_PluginInit"

// The VM's value slot: 16 bytes for every type (docs/engine-script-bindings.md, "The value slot").
typedef struct { unsigned char b[16]; } w2ef_value;

// Argument kinds: how the host builds an argument before the VM evaluates it, and how it releases
// the slot afterwards.
enum {
    W2EF_ARG_PLAIN  = 0,   // int, float, bool: the value at +0, nothing to release
    W2EF_ARG_STRING = 1,   // 16-byte wide-string container: deep copy in, release out
    W2EF_ARG_NAME   = 2,   // a CName dword at +0, nothing to release
    W2EF_ARG_OBJECT = 3    // a reference pointer at +0; the slot owns one reference
};

typedef struct w2ef_plugin_api w2ef_plugin_api;

// The script call itself: native(ctx, frame, result), __cdecl. A `result` is null when the script
// uses the call as a statement, and points at a value slot otherwise.
typedef void (__cdecl *w2ef_native_fn)(void* ctx, char* frame, void* result);
typedef void (__cdecl *w2ef_body_fn)(void* ctx, char* frame, w2ef_value* args, void* result);

struct w2ef_plugin_api {
    unsigned size;      // sizeof(w2ef_plugin_api), for extensions
    unsigned version;   // W2EF_PLUGIN_API_VERSION

    // Register `name` as a global script function. Returns 1 on success.
    int (__cdecl *register_native)(const wchar_t* name, w2ef_native_fn fn);

    // The native's contract in one call: evaluate `nargs` arguments by `kinds`, consume the call's
    // terminator, run `body`, release the arguments. Use this instead of FUN_00abc328 by hand.
    void (__cdecl *call)(void* ctx, char* frame, void* result, const unsigned char* kinds,
                         unsigned nargs, w2ef_body_fn body);

    void (__cdecl *free_string)(w2ef_value* slot);                        // release a string slot
    void (__cdecl *copy_string)(w2ef_value* dst, const w2ef_value* src);  // deep copy a string
    void (__cdecl *free_object)(w2ef_value* slot);                        // release one reference

    // One line in witcher2_enginefix.log, prefixed with "plugin: ".
    void (__cdecl *log)(const char* fmt, ...);
};

typedef int (__cdecl *w2ef_plugin_init_fn)(const w2ef_plugin_api* api);

#endif
