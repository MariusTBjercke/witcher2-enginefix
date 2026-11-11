// The example plugin for the Witcher 2 Engine Fix script-binding loader.
//
// Registers W2EF_PluginHello( value : float ) : float - it logs the argument through the host and
// returns value + 1. Build with tools\plugin-example\build.bat, copy build\plugin-hello.dll into
// <game>\bin\witcher2_enginefix_plugins\, and set [diagnostics] script_bindings=1. A script needs
// its own `import function W2EF_PluginHello( value : float ) : float;` declaration.
#include <windows.h>
#include "w2ef_plugin.h"

static const w2ef_plugin_api* g_api;
static int g_calls;

static void __cdecl hello_body(void* ctx, char* frame, w2ef_value* args, void* result)
{
    float value = *(const float*)args[0].b;

    (void)ctx;
    (void)frame;
    if (++g_calls <= 6) g_api->log("hello: called with %g", (double)value);
    if (result) *(float*)result = value + 1.0f;
}

static void __cdecl hello(void* ctx, char* frame, void* result)
{
    static const unsigned char kinds[1] = { W2EF_ARG_PLAIN };

    g_api->call(ctx, frame, result, kinds, 1, hello_body);
}

__declspec(dllexport) int __cdecl W2EF_PluginInit(const w2ef_plugin_api* api)
{
    if (!api || api->version != W2EF_PLUGIN_API_VERSION || api->size < sizeof(w2ef_plugin_api))
        return 0;
    g_api = api;
    if (!api->register_native(L"W2EF_PluginHello", hello)) return 0;
    api->log("hello: registered W2EF_PluginHello");
    return 1;
}
