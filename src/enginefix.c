// witcher2_enginefix - runtime fixes for The Witcher 2: Assassins of Kings (Steam 3.5.0.26,
// GOG 3.5.0.26g in progress)
//
// Loads as bin\dinput8.dll (the game imports dinput8.dll for controller input) and
// forwards every export to the real system dinput8.dll. witcher2.exe on disk is never
// touched. Delete bin\dinput8.dll and bin\witcher2_enginefix.ini to uninstall.
//
// The #defines below are Steam addresses at the preferred image base 0x00400000. Every use
// goes through ga()/ga_rva() ("builds and addresses" near the end): the running build is
// read from witcher2.exe's PE header, GOG's value comes from k_gog and the load address is
// added (the GOG exe has ASLR, Steam's does not). An address not known on the running build
// is 0, and build_gate() turns the features that need it off. Import slots are found by name
// in the exe's import directory (k_imports).
//
// LOD uses the settings singleton. Optional experimental distance hooks modify two
// fingerprinted renderer instructions in memory; see distance_patch.h and docs/fixes.
//
// Addresses derived from the unpacked binary; see docs/research.md:
//   DAT_02580e28  pointer to the settings object (0x02580E28 -> RVA 0x02180E28)
//   +0x34         MeshLodGameplayDownscale      (bool)
//   +0x38         CharacterLodGameplayDownscale (bool)
//   +0x68         FoliageDistanceScale          (float)
//   +0x6C         MeshDistanceScale             (float)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define PSAPI_VERSION 2   // GetProcessMemoryInfo from kernel32 (K32...), no psapi.lib
#include <psapi.h>
#include <tlhelp32.h>   // CreateToolhelp32Snapshot: per-thread CPU time for load_trace
#include <dbghelp.h>   // MINIDUMP_* types only; MiniDumpWriteDump is resolved at runtime
#include <intrin.h>    // _ReturnAddress: who calls DirectInput8Create and CreateDevice
#include <objbase.h>   // LPUNKNOWN / REFIID for the DirectInput8Create signature
#include <shlobj.h>    // SHGetFolderPathW: the game's Documents folder (User.ini)
#include <stdio.h>
#include <io.h>        // _get_osfhandle: DllMain's last log line when the log lock is orphaned
#include <share.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "distance_patch.h"
#include "window_patch.h"
#include "sp_hooks.h"
#include "sp_dzip.h"
#include "version.h"
#include "w2ef_plugin.h"

#define IMAGE_BASE          0x00400000u   // preferred image base of both builds
#define RVA_SETTINGS        0x02180E28u   // 0x02580E28 - IMAGE_BASE
#define OFF_MESH_DOWNSCALE  0x34
#define OFF_CHAR_DOWNSCALE  0x38
#define OFF_FOLIAGE         0x68
#define OFF_MESH            0x6C
#define SETTINGS_SPAN       0x70

#define MAX_STEPS 8

#define EF_MOD_CTRL   1
#define EF_MOD_ALT    2
#define EF_MOD_SHIFT  4

// The running witcher2.exe; set once in DllMain before any thread starts (build_detect).
enum { BUILD_UNKNOWN, BUILD_STEAM, BUILD_GOG };
static int       g_build;
static uintptr_t g_base;    // where witcher2.exe is loaded
static uintptr_t g_delta;   // g_base - IMAGE_BASE: 0 on Steam, the ASLR shift on GOG
static uintptr_t ga(uintptr_t steam_va);
static uintptr_t ga_rva(uintptr_t steam_rva);
static const char* build_name(void);
static int patch_call(uintptr_t at, uintptr_t target, void* hook);
static int code_matches(uintptr_t at, const char* hex);
static int wf_user_ini_shadow_quality(void);

static HINSTANCE g_self;
static FILE*     g_log;
static DWORD     g_mainTid;   // the thread DllMain ran on: the game's main thread
static char      g_ini[MAX_PATH];
static char      g_logPath[MAX_PATH];
static char      g_prevLogPath[MAX_PATH];
static char      g_binDir[MAX_PATH];
static HMODULE   g_realDinput;
static volatile LONG g_running;

// on-screen display: topmost, colour-keyed layered window composited by DWM
static HWND  g_osdWnd;
static HFONT g_osdFont;
static DWORD g_osdUntil;
static char  g_osdText[192];

// ---- config -------------------------------------------------------------

static int   cfg_enabled        = 1;
static int   cfg_log            = 1;
static int   cfg_enforce        = 1;
static int   cfg_enforce_down   = 1;
static float cfg_mesh           = 4.0f;
static float cfg_foliage        = 2.0f;
static int   cfg_hotkeys        = 1;
static int   cfg_vk_mesh        = VK_F9;
static int   cfg_mod_mesh       = 1;    // EF_MOD_CTRL
static int   cfg_vk_foliage     = VK_F10;
static int   cfg_mod_foliage    = 1;
static int   cfg_suppress       = 1;    // swallow F9/F10 in the game's keyboard state while a modifier is held
static int   cfg_winkeys        = 1;    // [hotkeys] windows_keys: Windows key combinations and Print Screen in game
static int   cfg_osd            = 1;
static int   cfg_osd_ms         = 2500;
static int   cfg_osd_size       = 34;
static float cfg_mesh_steps[MAX_STEPS];   static int cfg_mesh_step_n;
static float cfg_foliage_steps[MAX_STEPS]; static int cfg_foliage_step_n;
static int   cfg_distance;
static float cfg_visibility = 1.0f, cfg_shadow_range = 1.0f;
static int   cfg_vk_visibility = VK_F9, cfg_vk_shadow_range = VK_F10;
static int   cfg_mod_visibility = EF_MOD_CTRL | EF_MOD_SHIFT;
static int   cfg_mod_shadow_range = EF_MOD_CTRL | EF_MOD_SHIFT;
static int   cfg_cascade = 1;   // [shadows] enabled: the Shadow quality levels
static int   cfg_vk_shadow = VK_F10, cfg_mod_shadow = EF_MOD_CTRL | EF_MOD_SHIFT;

typedef struct {
    const char* name;
    float first, seam, far_end;     // where the three sun shadow zones end
    int atlas;                      // shadow map pixels per zone
    float filter;                   // filter width, 1 = the game's
} shadow_level;
#define SHADOW_LEVELS 3
static const shadow_level k_shadow_levels[SHADOW_LEVELS] = {
    { "vanilla", 7.0f, 24.0f, 64.0f, 1024, 1.0f },  // the game's own values
    { "high",    12.0f, 24.0f, 64.0f, 2048, 0.6f },
    { "ultra",   12.0f, 24.0f, 64.0f, 4096, 0.5f }
};
static shadow_level cfg_shadow[SHADOW_LEVELS];
static int g_shadow;
static volatile LONG g_shadowAtlasWant = 1024;  // the size of the level in use
static volatile LONG g_shadowAtlasNow = 1024;   // the size the renderer has
static volatile LONG g_shadowMenu = -1;         // the options row's choice, for the worker
static volatile LONG g_shadowSync;              // 1: write the level into the game's config
static volatile LONG g_shadowStage;             // the viewport tick is switching the atlas
static volatile LONG g_shadowResult;            // 1 switched, -1 refused
static volatile LONG g_shadowAllocRan;          // this switch's Reset reached the allocator
static int g_shadowLive;                        // the viewport tick and the allocator are hooked

// The switch's Reset is over. A level picked during it leaves Want and Now apart, and the next frame
// starts another switch; only a Reset that never reached the allocator was refused.
static void shadow_switch_end(void)
{
    if (g_shadowAllocRan) {
        InterlockedExchange(&g_shadowResult, 1);
    } else {                        // no asking again every frame
        InterlockedExchange(&g_shadowAtlasWant, g_shadowAtlasNow);
        InterlockedExchange(&g_shadowResult, -1);
    }
    InterlockedExchange(&g_shadowStage, 0);
}

#define VA_BILLBOARD_SHOW 0x009BC8AFu   // movss xmm0,[edi+100h] in 009BC894, which adds a mesh's billboard level

typedef struct {
    const char* name;
    float mesh, foliage, visibility, shadow, billboard;
} quality_preset;
#define PRESET_COUNT 4
static const quality_preset k_presets[PRESET_COUNT] = {
    { "vanilla", 1.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { "high",     2.0f, 2.0f, 1.5f, 1.5f, 1.5f },
    { "ultra",    4.0f, 3.0f, 2.0f, 2.0f, 2.0f },
    { "extreme",  8.0f, 5.0f, 3.0f, 3.0f, 3.0f }
};
static quality_preset cfg_presets[PRESET_COUNT];
static int cfg_preset_mode, g_preset = PRESET_COUNT - 1;
static volatile LONG g_ddSync;  // 1: write the live preset into the game's config (Draw distance row)
static int cfg_vk_preset = VK_F10, cfg_mod_preset = EF_MOD_CTRL;
static int cfg_save_trace;      // [diagnostics] save_trace: log the save-thumbnail path
static int cfg_save_thumbs = 1; // [saves] fix_thumbnails: write the late save preview
static int cfg_savelist_trace;  // [diagnostics] savelist_trace: time/memory of the save list
static int cfg_list_leak = 1;   // [saves] fix_list_leak: free the preview the save list leaks
static int cfg_fast_list = 1;   // [saves] fast_list: unpack only the save header blocks (2 = every other build)
static int cfg_light_previews = 1; // [saves] light_previews: Load menu previews without upscaling and mip chain
static int cfg_skip_intro;      // [startup] skip_intro: no logo/intro videos before the main menu
static int cfg_fast_start = 1;  // [startup] fast_start: no idle waiting for FMOD while the game starts
static int cfg_file_trace;      // [diagnostics] file_trace: how the game opens its data files
static int cfg_load_trace;      // [diagnostics] load_trace: where loading-screen time goes
static int cfg_mem_trace;       // [diagnostics] mem_trace: minutes between memory censuses (0 = off)
static int cfg_script_bindings; // [diagnostics] script_bindings: register W2EF_TestParam (research, see the header)
static int cfg_fast_doors = 1;  // [loading] fast_doors: picture back once a house interior is in
static float cfg_door_fade = 0.05f; // [loading] door_fade: seconds of the door fade-out/in (game: 0.5)
static float cfg_area_fade = 0.25f; // [loading] area_fade: door fade when the door changes area (Flotsam gate)
static int cfg_interiors = 2;   // [loading] interiors: 0 game, 1 keep the outdoors, 2 also preload interiors
static int cfg_keep_outdoors = 1; // [loading] keep_outdoors: house doors leave the outdoor layers loaded
static int cfg_skip_door_gc = 1; // [loading] skip_door_gc: no full object GC on a keep_outdoors house door
static int cfg_release_kept = 30; // [loading] release_kept: metres from a door between two areas after which the area left behind is hidden or unloaded (0 = never)
static int cfg_hide_kept = 1;   // [loading] hide_kept: with release_kept, hide them instead while area_memory_mb is free
static int cfg_preload_areas = 1; // [loading] preload_areas: load the small areas behind an area's doors with it
static int cfg_instant_doors = 1; // [loading] instant_doors: no fade on a house door known to load nothing
static int cfg_indoor_outdoors = 1; // [loading] indoor_outdoors: keep drawing terrain/grass behind kept doors
static int cfg_preload_interiors = 1; // [loading] preload_interiors: load every building interior with the area
static int cfg_keep_interiors = 1; // [loading] keep_interiors: a visited house interior stays loaded, no fake rooms back
static float cfg_door_light_blend = 1.0f; // [loading] door_light_blend: seconds the lighting blends over at an instant door (0 = game: at once)
static int cfg_preload_street = 1; // [loading] preload_street: a save inside a house also loads its street
static int cfg_door_spinner;    // [loading] door_spinner: 1 = the loading spinner also on instant doors (game)
static int cfg_preload_memory_mb = 1280; // [loading] preload_memory_mb: preloaded layers load only while this much address space is free (0 = no limit)
static int cfg_door_memory_mb = 1024;    // [loading] door_memory_mb: less free than this at a door -> the game's own door (0 = never)
static int cfg_area_memory_mb = 1536;    // [loading] area_memory_mb: less free than this at a door between two areas -> the area left behind unloads (0 = never)
static int cfg_book_marker = 1; // [scriptpatches] book_marker: Read/Nothing new/Unread in lists
static int cfg_continue = 1;    // [scriptpatches] continue: Continue button in the main menu
static int cfg_pause_quit = 1;  // [scriptpatches] pause_quit: Quit asks first in the pause menu
static int cfg_window_mode = 1; // [scriptpatches] window_mode: Full screen / Borderless in Options
static int cfg_medallion = 1;   // [scriptpatches] medallion: used on the move, 1 s cooldown
static int cfg_wf_enabled = 1;  // [window] enabled: borderless support (patch + live toggle + menu)
static int cfg_wf_borderless;   // [window] borderless: start borderless until the menu has saved it
static int cfg_wf_dpi = 1;      // [window] dpi_aware: DPI awareness when borderless starts on
static int cfg_crash_dump = 1;  // [diagnostics] crash_dump: 0 off, 1 log + minidump, 2 full dump
static int cfg_exit_trace = 1;  // [diagnostics] exit_trace: log how the game ends (exit: lines)
static int cfg_ray_guard = 1;   // [stability] raycast_guard: a ray cast that hits freed physics misses it
static int cfg_cpu32_guard = 1; // [stability] cpu32_guard: no endless CPU count on 32+ logical processors
static int cfg_quit_guard = 1;  // [stability] quit_guard: FactsAdd without a world (quitting) does not crash
static int cfg_storage_leak = 1; // [stability] layer_storage_leak: free the saved NPC states the game drops
static int cfg_clean_quit = 1;  // [stability] clean_quit: Quit with an area loaded ends the game session first
static int cfg_path_guard = 1;  // [stability] path_guard: a zero-length pathfinding line does not hang a job
static int cfg_text_guard = 1;  // [stability] text_guard: a UTF-16 text file read whole always ends in 0x0000
static int cfg_lscr_cleanup = 1; // [stability] loading_screen_cleanup: free a loading-screen object the game left behind
static int g_proxyOnly;         // loaded by another program in bin (Configurator.exe): forward only
static int g_scriptHooks;       // sp_hooks_start succeeded: script patches are active
static volatile LONG g_scriptRedirects;

// ---- logging ------------------------------------------------------------
// One lock for whole lines: the worker, the file-trace hooks (called on any game
// thread) and the save hooks all append to the same FILE*.

static CRITICAL_SECTION g_logLock;

// The peer-freeze barrier (patch_barrier.h), one caller at a time. Defined here, initialized in
// DllMain before any of the patch threads exists.
CRITICAL_SECTION g_pbLock;

static void logf_(const char* fmt, ...)
{
    if (!g_log || !cfg_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// One line straight to the log file's handle, past the FILE buffer and its lock: for the end
// of the process, when the lock may belong to a thread that will never let go of it.
static void log_raw(const char* text)
{
    char line[600];
    SYSTEMTIME st;
    HANDLE fh;
    DWORD wrote = 0;
    int n;
    if (!g_log || !cfg_log) return;
    fh = (HANDLE)_get_osfhandle(_fileno(g_log));
    GetLocalTime(&st);
    n = _snprintf_s(line, sizeof line, _TRUNCATE, "\r\n%02d:%02d:%02d.%03d %s\r\n", st.wHour, st.wMinute,
                    st.wSecond, st.wMilliseconds, text);
    if (fh != INVALID_HANDLE_VALUE && n > 0) WriteFile(fh, line, (DWORD)n, &wrote, NULL);
}

// logf_ for the exit hooks: waits for the log lock 250 ms at most, then writes raw. Whoever ends
// the process may have suspended the other threads first, one of them inside logf_.
static void exit_logf(const char* fmt, ...)
{
    char text[480];
    DWORD t0 = GetTickCount();
    va_list ap;
    if (!g_log || !cfg_log) return;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof text, _TRUNCATE, fmt, ap);
    va_end(ap);
    while (!TryEnterCriticalSection(&g_logLock)) {
        if (GetTickCount() - t0 >= 250) { log_raw(text); return; }
        Sleep(1);
    }
    logf_("%s", text);
    LeaveCriticalSection(&g_logLock);
}

// ---- ini ----------------------------------------------------------------

static void ini_path(void)
{
    char dll[MAX_PATH];
    GetModuleFileNameA(g_self, dll, MAX_PATH);
    char* slash = strrchr(dll, '\\');
    if (slash) *slash = 0;
    sprintf_s(g_ini, MAX_PATH, "%s\\witcher2_enginefix.ini", dll);
    sprintf_s(g_logPath, MAX_PATH, "%s\\witcher2_enginefix.log", dll);
    sprintf_s(g_prevLogPath, MAX_PATH, "%s\\witcher2_enginefix.prev.log", dll);
    strcpy_s(g_binDir, MAX_PATH, dll);
}

static int ini_int(const char* sec, const char* key, int def)
{
    return (int)GetPrivateProfileIntA(sec, key, def, g_ini);
}

static float ini_float(const char* sec, const char* key, float def)
{
    char buf[64]; char d[64];
    sprintf_s(d, sizeof d, "%.4f", def);
    GetPrivateProfileStringA(sec, key, d, buf, sizeof buf, g_ini);
    return (float)atof(buf);
}

static int parse_steps(const char* key, float* out)
{
    char buf[256];
    GetPrivateProfileStringA("hotkeys", key, "", buf, sizeof buf, g_ini);
    int n = 0;
    char* p = buf;
    while (*p && n < MAX_STEPS) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        out[n++] = (float)atof(p);
        while (*p && *p != ',') p++;
    }
    return n;
}

static int key_from_name(const char* name)
{
    if (!_stricmp(name, "F1"))  return VK_F1;
    if (!_stricmp(name, "F2"))  return VK_F2;
    if (!_stricmp(name, "F3"))  return VK_F3;
    if (!_stricmp(name, "F4"))  return VK_F4;
    if (!_stricmp(name, "F5"))  return VK_F5;
    if (!_stricmp(name, "F6"))  return VK_F6;
    if (!_stricmp(name, "F7"))  return VK_F7;
    if (!_stricmp(name, "F8"))  return VK_F8;
    if (!_stricmp(name, "F9"))  return VK_F9;
    if (!_stricmp(name, "F10")) return VK_F10;
    if (!_stricmp(name, "F11")) return VK_F11;
    if (!_stricmp(name, "F12")) return VK_F12;
    return 0;
}

static int mods_held(void)
{
    int m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= EF_MOD_CTRL;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= EF_MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= EF_MOD_SHIFT;
    return m;
}

// "Ctrl+Alt+F9" -> VK_F9 with EF_MOD_CTRL|EF_MOD_ALT. Falls back to `fallback` on garbage.
static void parse_hotkey(const char* spec, const char* fallback, int* vk, int* mods)
{
    char buf[64];
    lstrcpynA(buf, (spec && *spec) ? spec : fallback, sizeof buf);
    int m = 0;
    char* p = buf;
    for (;;) {
        char* plus = strchr(p, '+');
        if (!plus) break;
        *plus = 0;
        if (!_stricmp(p, "Ctrl") || !_stricmp(p, "Control")) m |= EF_MOD_CTRL;
        else if (!_stricmp(p, "Alt"))  m |= EF_MOD_ALT;
        else if (!_stricmp(p, "Shift")) m |= EF_MOD_SHIFT;
        p = plus + 1;
    }
    *mods = m;
    *vk = key_from_name(p);
}

// Presets own only these four live values. The renderer's hooks remain installed
// once; cycling changes their factors, never installs another code patch.
static void select_preset(int index)
{
    const quality_preset* p = &cfg_presets[index];
    g_preset = index;
    cfg_mesh = p->mesh; cfg_foliage = p->foliage;
    cfg_visibility = p->visibility; cfg_shadow_range = p->shadow;
    ef_range_sq = p->visibility * p->visibility;
    ef_shadow_sq = p->shadow * p->shadow;
    ef_billboard_factor = p->billboard;
}

static float preset_float(const char* section, const char* key, float def, int distance)
{
    char buf[64], *end;
    float value;
    GetPrivateProfileStringA(section, key, "", buf, sizeof buf, g_ini);
    value = strtof(buf, &end);
    if (end == buf) return def;
    while (*end == ' ' || *end == '\t') end++;
    if (*end || !(value > 0.0f && value < 100.0f) ||
        (distance && !(value >= 1.0f && value <= 4.0f))) return def;
    return value;
}

static void load_presets(void)
{
    char name[32], section[48], key[64];
    cfg_preset_mode = ini_int("presets", "enabled", 0); // old ini retains old controls
    if (!cfg_preset_mode) return;
    GetPrivateProfileStringA("presets", "default", "extreme", name, sizeof name, g_ini);
    g_preset = PRESET_COUNT - 1;
    for (int i = 0; i < PRESET_COUNT; i++) {
        const quality_preset* def = &k_presets[i];
        quality_preset* p = &cfg_presets[i];
        p->name = def->name;
        sprintf_s(section, sizeof section, "preset.%s", def->name);
        p->mesh = def->mesh;
        p->foliage = def->foliage;
        p->visibility = def->visibility;
        p->shadow = def->shadow;
        p->billboard = ef_billboard_value(preset_float(section, "billboard_scale", def->billboard, 0), def->billboard);
        if (i == 0) {
            // Vanilla was called baseline up to 0.10.0: an older ini still counts.
            p->mesh = preset_float("preset.baseline", "mesh_scale", p->mesh, 0);
            p->foliage = preset_float("preset.baseline", "foliage_scale", p->foliage, 0);
            p->visibility = preset_float("preset.baseline", "visibility_scale", p->visibility, 1);
            p->shadow = preset_float("preset.baseline", "shadow_scale", p->shadow, 1);
            if (!_stricmp(name, "baseline")) g_preset = i;
        }
        p->mesh = preset_float(section, "mesh_scale", p->mesh, 0);
        p->foliage = preset_float(section, "foliage_scale", p->foliage, 0);
        p->visibility = preset_float(section, "visibility_scale", p->visibility, 1);
        p->shadow = preset_float(section, "shadow_scale", p->shadow, 1);
        if (!_stricmp(name, def->name)) g_preset = i;
    }
    GetPrivateProfileStringA("presets", "cycle", "Ctrl+F10", key, sizeof key, g_ini);
    parse_hotkey(key, "Ctrl+F10", &cfg_vk_preset, &cfg_mod_preset);
    // The existing DirectInput filter protects only modified F9/F10, so do not
    // accept a binding that could also invoke the game's own quick load.
    if ((cfg_vk_preset != VK_F9 && cfg_vk_preset != VK_F10) ||
        !(cfg_mod_preset & (EF_MOD_CTRL | EF_MOD_ALT))) {
        cfg_vk_preset = VK_F10; cfg_mod_preset = EF_MOD_CTRL;
    }
    cfg_enforce = 1;
    cfg_distance = 1;
    select_preset(g_preset);
}

static void shadow_select(int level)
{
    const shadow_level* l = &cfg_shadow[level];
    g_shadow = level;
    ef_cascade_set(l->first, l->seam, l->far_end);
    ef_filter_factor = l->filter;
    InterlockedExchange(&g_shadowAtlasWant, l->atlas);
}

static float shadow_field(const char* section, const char* key, float def, float low, float high)
{
    float v = ini_float(section, key, def);
    return v >= low && v <= high ? v : def;
}

// The ini's levels; each field falls back to the level's built-in value. Vanilla is always the game's.
static void load_shadow_config(void)
{
    char name[32], key[64];
    cfg_cascade = ini_int("shadows", "enabled", 1);
    GetPrivateProfileStringA("shadows", "quality", "vanilla", name, sizeof name, g_ini);
    g_shadow = 0;
    for (int i = 0; i < SHADOW_LEVELS; i++) {
        const shadow_level* def = &k_shadow_levels[i];
        shadow_level* l = &cfg_shadow[i];
        *l = *def;
        if (i > 0) {
            char section[48];
            int atlas;
            sprintf_s(section, sizeof section, "shadows.%s", def->name);
            l->first = shadow_field(section, "first_distance", def->first, 7.0f, 128.0f);
            l->seam = shadow_field(section, "seam_distance", def->seam, 24.0f, 256.0f);
            l->far_end = shadow_field(section, "sun_distance", def->far_end, 64.0f, 512.0f);
            l->filter = shadow_field(section, "filter", def->filter, 0.25f, 1.0f);
            atlas = ini_int(section, "atlas", def->atlas);
            l->atlas = ef_cascade_atlas(atlas) == atlas ? atlas : def->atlas;
        }
        if (!_stricmp(name, def->name)) g_shadow = i;
    }
    shadow_select(g_shadow);
    GetPrivateProfileStringA("shadows", "cycle", "Ctrl+Shift+F10", key, sizeof key, g_ini);
    parse_hotkey(key, "Ctrl+Shift+F10", &cfg_vk_shadow, &cfg_mod_shadow);
    // As for the presets: the input filter protects only F9/F10 with Ctrl or Alt.
    if ((cfg_vk_shadow != VK_F9 && cfg_vk_shadow != VK_F10) ||
        !(cfg_mod_shadow & (EF_MOD_CTRL | EF_MOD_ALT))) {
        cfg_vk_shadow = VK_F10; cfg_mod_shadow = EF_MOD_CTRL | EF_MOD_SHIFT;
    }
}

static void load_config(void)
{
    char buf[32];
    cfg_enabled      = ini_int("general", "enabled", 1);
    cfg_log          = ini_int("general", "log", 1);
    cfg_enforce      = ini_int("lod", "enforce", 1);
    cfg_enforce_down = ini_int("lod", "enforce_downscale", 1);
    cfg_mesh         = ini_float("lod", "mesh_scale", 4.0f);
    cfg_foliage      = ini_float("lod", "foliage_scale", 2.0f);
    cfg_hotkeys      = ini_int("hotkeys", "enabled", 1);

    GetPrivateProfileStringA("hotkeys", "cycle_mesh", "Ctrl+F9", buf, sizeof buf, g_ini);
    parse_hotkey(buf, "Ctrl+F9", &cfg_vk_mesh, &cfg_mod_mesh);
    GetPrivateProfileStringA("hotkeys", "cycle_foliage", "Ctrl+F10", buf, sizeof buf, g_ini);
    parse_hotkey(buf, "Ctrl+F10", &cfg_vk_foliage, &cfg_mod_foliage);
    cfg_suppress = ini_int("hotkeys", "suppress_mod_keys", 1);
    cfg_winkeys = ini_int("hotkeys", "windows_keys", 1);

    cfg_distance = ini_int("distance", "enabled", 0);
    cfg_visibility = ef_distance_factor(ini_float("distance", "visibility_scale", 1.0f));
    cfg_shadow_range = ef_distance_factor(ini_float("distance", "shadow_scale", 1.0f));
    ef_range_sq = cfg_visibility * cfg_visibility;
    ef_shadow_sq = cfg_shadow_range * cfg_shadow_range;
    GetPrivateProfileStringA("distance", "cycle_visibility", "Ctrl+Shift+F9", buf, sizeof buf, g_ini);
    parse_hotkey(buf, "Ctrl+Shift+F9", &cfg_vk_visibility, &cfg_mod_visibility);
    GetPrivateProfileStringA("distance", "cycle_shadow", "Ctrl+Shift+F10", buf, sizeof buf, g_ini);
    parse_hotkey(buf, "Ctrl+Shift+F10", &cfg_vk_shadow_range, &cfg_mod_shadow_range);

    load_shadow_config();

    cfg_osd      = ini_int("osd", "enabled", 1);
    cfg_osd_ms   = ini_int("osd", "ms", 2500);
    cfg_osd_size = ini_int("osd", "size", 34);
    if (cfg_osd_ms < 300) cfg_osd_ms = 300;
    if (cfg_osd_size < 10) cfg_osd_size = 10;
    if (cfg_osd_size > 120) cfg_osd_size = 120;

    cfg_save_trace = ini_int("diagnostics", "save_trace", 0);
    cfg_save_thumbs = ini_int("saves", "fix_thumbnails", 1);
    cfg_savelist_trace = ini_int("diagnostics", "savelist_trace", 0);
    cfg_list_leak = ini_int("saves", "fix_list_leak", 1);
    cfg_fast_list = ini_int("saves", "fast_list", 1);
    cfg_light_previews = ini_int("saves", "light_previews", 1);
    cfg_skip_intro = ini_int("startup", "skip_intro", 0);
    cfg_fast_start = ini_int("startup", "fast_start", 1);
    cfg_file_trace = ini_int("diagnostics", "file_trace", 0);
    cfg_load_trace = ini_int("diagnostics", "load_trace", 0);
    cfg_mem_trace = ini_int("diagnostics", "mem_trace", 0);
    if (cfg_mem_trace < 0) cfg_mem_trace = 0;
    if (cfg_mem_trace > 240) cfg_mem_trace = 240;
    cfg_script_bindings = ini_int("diagnostics", "script_bindings", 0);
    cfg_fast_doors = ini_int("loading", "fast_doors", 1);
    cfg_door_fade = ini_float("loading", "door_fade", 0.05f);
    cfg_area_fade = ini_float("loading", "area_fade", 0.25f);
    if (cfg_area_fade < 0.05f || cfg_area_fade > 2.0f) cfg_area_fade = 0.5f;
    // One switch for users; the separate keys below still override it (docs/fixes/interior-preload.md).
    cfg_interiors = ini_int("loading", "interiors", 2);
    if (cfg_interiors < 0 || cfg_interiors > 2) cfg_interiors = 2;
    cfg_keep_outdoors = ini_int("loading", "keep_outdoors", cfg_interiors >= 1);
    cfg_keep_interiors = ini_int("loading", "keep_interiors", cfg_interiors >= 1);
    cfg_preload_interiors = ini_int("loading", "preload_interiors", cfg_interiors >= 2);
    cfg_indoor_outdoors = ini_int("loading", "indoor_outdoors", 1);
    cfg_skip_door_gc = ini_int("loading", "skip_door_gc", 1);
    cfg_release_kept = ini_int("loading", "release_kept", 30);
    if (cfg_release_kept < 0 || cfg_release_kept > 1000) cfg_release_kept = 30;
    cfg_hide_kept = ini_int("loading", "hide_kept", 1);
    cfg_preload_areas = ini_int("loading", "preload_areas", 1);
    if (!cfg_keep_outdoors || !cfg_preload_interiors) cfg_preload_areas = 0;   // they ride on the interiors' preload, and a door must keep them
    cfg_instant_doors = ini_int("loading", "instant_doors", 1);
    cfg_door_light_blend = ini_float("loading", "door_light_blend", 1.0f);
    if (cfg_door_light_blend < 0.0f || cfg_door_light_blend > 5.0f) cfg_door_light_blend = 1.0f;
    cfg_preload_street = ini_int("loading", "preload_street", 1);
    cfg_door_spinner = ini_int("loading", "door_spinner", 0);
    cfg_preload_memory_mb = ini_int("loading", "preload_memory_mb", 1280);
    if (cfg_preload_memory_mb < 0 || cfg_preload_memory_mb > 3500) cfg_preload_memory_mb = 1280;
    cfg_door_memory_mb = ini_int("loading", "door_memory_mb", 1024);
    if (cfg_door_memory_mb < 0 || cfg_door_memory_mb > 3500) cfg_door_memory_mb = 1024;
    cfg_area_memory_mb = ini_int("loading", "area_memory_mb", 1536);
    if (cfg_area_memory_mb < 0 || cfg_area_memory_mb > 3500) cfg_area_memory_mb = 1536;
    if (cfg_door_fade < 0.05f || cfg_door_fade > 2.0f) cfg_door_fade = 0.5f;
    cfg_book_marker = ini_int("scriptpatches", "book_marker", 1);
    cfg_continue = ini_int("scriptpatches", "continue", 1);
    cfg_pause_quit = ini_int("scriptpatches", "pause_quit", 1);
    cfg_window_mode = ini_int("scriptpatches", "window_mode", 1);
    cfg_medallion = ini_int("scriptpatches", "medallion", 1);
    cfg_wf_enabled = ini_int("window", "enabled", 1);
    cfg_wf_borderless = ini_int("window", "borderless", 0);
    cfg_wf_dpi = ini_int("window", "dpi_aware", 1);
    cfg_crash_dump = ini_int("diagnostics", "crash_dump", 1);
    cfg_exit_trace = ini_int("diagnostics", "exit_trace", 1);
    cfg_ray_guard = ini_int("stability", "raycast_guard", 1);
    cfg_cpu32_guard = ini_int("stability", "cpu32_guard", 1);
    cfg_quit_guard = ini_int("stability", "quit_guard", 1);
    cfg_storage_leak = ini_int("stability", "layer_storage_leak", 1);
    cfg_clean_quit = ini_int("stability", "clean_quit", 1);
    cfg_path_guard = ini_int("stability", "path_guard", 1);
    cfg_text_guard = ini_int("stability", "text_guard", 1);
    cfg_lscr_cleanup = ini_int("stability", "loading_screen_cleanup", 1);

    cfg_mesh_step_n    = parse_steps("mesh_steps", cfg_mesh_steps);
    cfg_foliage_step_n = parse_steps("foliage_steps", cfg_foliage_steps);
    if (cfg_mesh_step_n == 0)    { cfg_mesh_steps[0] = 1.0f; cfg_mesh_steps[1] = 2.0f; cfg_mesh_steps[2] = 4.0f; cfg_mesh_steps[3] = 8.0f; cfg_mesh_step_n = 4; }
    if (cfg_foliage_step_n == 0) { cfg_foliage_steps[0] = 1.0f; cfg_foliage_steps[1] = 2.0f; cfg_foliage_steps[2] = 3.0f; cfg_foliage_steps[3] = 5.0f; cfg_foliage_step_n = 4; }
    load_presets();
}

// ---- on-screen display --------------------------------------------------
// A topmost layered window colour-keyed on black. DXVK presents through DWM, so a
// topmost window is composited above the swapchain; true exclusive fullscreen would
// not show it (that is why W1 draws inside the D3D scene instead).

static LRESULT CALLBACK osd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT rc; GetClientRect(wnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));   // colour-keyed away
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        if (g_osdFont) {
            HGDIOBJ old = SelectObject(dc, g_osdFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0xF0, 0xE6, 0xD2));
            DrawTextA(dc, g_osdText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
        EndPaint(wnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static void osd_init(void)
{
    if (!cfg_osd || g_osdWnd) return;
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = osd_proc;
    wc.hInstance = g_self;
    wc.lpszClassName = "W2EngineFixOSD";
    RegisterClassExA(&wc);
    g_osdWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
        "W2EngineFixOSD", "W2EngineFix", WS_POPUP,
        0, 0, 10, 10, NULL, NULL, g_self, NULL);
    if (!g_osdWnd) {
        logf_("osd: CreateWindowEx failed (%lu)", (unsigned long)GetLastError());
        return;
    }
    SetLayeredWindowAttributes(g_osdWnd, RGB(0, 0, 0), 255, LWA_COLORKEY);
    g_osdFont = CreateFontA(cfg_osd_size, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    logf_("osd: window ready (font %d px)", cfg_osd_size);
}

static void osd_show(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(g_osdText, sizeof g_osdText, fmt, ap);
    va_end(ap);
    logf_("osd: %s", g_osdText);
    if (!cfg_osd) return;
    if (!g_osdWnd) osd_init();
    if (!g_osdWnd) return;

    RECT rc = { 0 };
    HWND game = GetForegroundWindow();
    DWORD pid = 0;
    if (game) GetWindowThreadProcessId(game, &pid);
    if (game == g_osdWnd || pid != GetCurrentProcessId()) game = NULL;

    int h = cfg_osd_size * 2;
    if (game && GetWindowRect(game, &rc)) {
        int w = (rc.right - rc.left) / 3;
        if (w < 420) w = 420;
        int x = rc.left + ((rc.right - rc.left) - w) / 2;
        int y = rc.top + (rc.bottom - rc.top) / 12;
        SetWindowPos(g_osdWnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        int w = sw / 3; if (w < 420) w = 420;
        SetWindowPos(g_osdWnd, HWND_TOPMOST, (sw - w) / 2, sh / 12, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    g_osdUntil = GetTickCount() + (DWORD)cfg_osd_ms;
    InvalidateRect(g_osdWnd, NULL, TRUE);
    UpdateWindow(g_osdWnd);
}

static void osd_tick(void)
{
    if (!g_osdWnd || !g_osdUntil) return;
    if ((LONG)(GetTickCount() - g_osdUntil) >= 0) {
        ShowWindow(g_osdWnd, SW_HIDE);
        g_osdUntil = 0;
    }
}

static void osd_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

// ---- memory helpers -----------------------------------------------------

static int range_readable(const void* p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!p) return 0;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return 0;
    return ((uintptr_t)p + n) <= ((uintptr_t)mbi.BaseAddress + mbi.RegionSize);
}

static void* read_ptr(const void* at)
{
    void* v = NULL;
    __try { v = *(void* const*)at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = NULL; }
    return v;
}

static int read_f32(const void* at, float* out)
{
    __try { *out = *(const float*)at; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int write_f32(void* at, float v)
{
    __try { *(float*)at = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int write_u8(void* at, unsigned char v)
{
    __try { *(unsigned char*)at = v; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int sane_scale(float v)
{
    return (v == v) && v > 0.0f && v < 100.0f;   // NaN-safe, reject absurd values
}

// ---- patch --------------------------------------------------------------

typedef struct {
    void*  obj;
    float  mesh;
    float  foliage;
} settings_view;

static int grab_settings(settings_view* out)
{
    uintptr_t rva = ga_rva(RVA_SETTINGS);
    if (!rva || !g_base) return 0;
    const void* slot = (const void*)(g_base + rva);
    if (!range_readable(slot, sizeof(void*))) return 0;
    out->obj = read_ptr(slot);
    if (!out->obj) return 0;
    if (!range_readable(out->obj, SETTINGS_SPAN)) return 0;
    if (!read_f32((char*)out->obj + OFF_MESH, &out->mesh)) return 0;
    if (!read_f32((char*)out->obj + OFF_FOLIAGE, &out->foliage)) return 0;
    // The singleton is created lazily; until it is valid these fields are garbage.
    if (!sane_scale(out->mesh) || !sane_scale(out->foliage)) return 0;
    return 1;
}

static int  g_applied;          // 0 = not yet, 1 = applied at least once
static float g_last_mesh = -1.0f, g_last_foliage = -1.0f;
static int  g_step_mesh = -1, g_step_foliage = -1;

static void preset_show(void)
{
    if (ef_distance_active)
        osd_show("Graphics: %s  (%d/%d)", cfg_presets[g_preset].name, g_preset + 1, PRESET_COUNT);
    else
        osd_show("Graphics: %s  (LOD only)", cfg_presets[g_preset].name);
}

static void apply_lod(void)
{
    settings_view sv;
    if (!grab_settings(&sv)) return;

    if (!g_applied) {
        logf_("settings object 0x%08X found: mesh=%.3f foliage=%.3f",
              (unsigned)(uintptr_t)sv.obj, sv.mesh, sv.foliage);
        g_applied = 1;
        // Log only: at this point the game's window may not exist yet, so the OSD would float
        // over the launcher or the desktop. The OSD is for changes the player makes.
        if (cfg_preset_mode) logf_("preset: %s at startup", cfg_presets[g_preset].name);
        else logf_("lod: active at startup, mesh %.1f foliage %.1f", cfg_mesh, cfg_foliage);
    }

    if (cfg_enforce && sane_scale(cfg_mesh) && sane_scale(cfg_foliage)) {
        if (sv.mesh != cfg_mesh)     write_f32((char*)sv.obj + OFF_MESH, cfg_mesh);
        if (sv.foliage != cfg_foliage) write_f32((char*)sv.obj + OFF_FOLIAGE, cfg_foliage);
        if (cfg_mesh != g_last_mesh || cfg_foliage != g_last_foliage) {
            logf_("lod enforced: mesh=%.3f foliage=%.3f", cfg_mesh, cfg_foliage);
            g_last_mesh = cfg_mesh; g_last_foliage = cfg_foliage;
        }
    }
    if (cfg_enforce_down) {
        write_u8((char*)sv.obj + OFF_MESH_DOWNSCALE, 0);
        write_u8((char*)sv.obj + OFF_CHAR_DOWNSCALE, 0);
    }
}

static void poll_hotkeys(void)
{
    static int preset_down;
    if (cfg_preset_mode) {
        int pressed = (GetAsyncKeyState(cfg_vk_preset) & 0x8000) != 0;
        DWORD pid = 0;
        HWND foreground = GetForegroundWindow();
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        if (pressed && !preset_down && mods_held() == cfg_mod_preset &&
            pid == GetCurrentProcessId()) {
            select_preset((g_preset + 1) % PRESET_COUNT);
            apply_lod();
            InterlockedExchange(&g_ddSync, 1);   // the Draw distance row follows the hotkey
            logf_("preset: %s mesh=%.2f foliage=%.2f visibility=%.2fx shadow=%.2fx billboards=%.2fx; distance_active=%d",
                  cfg_presets[g_preset].name, cfg_mesh, cfg_foliage,
                  cfg_visibility, cfg_shadow_range, ef_billboard_factor, ef_distance_active);
            preset_show();
        }
        preset_down = pressed;
        return;
    }
    static int mesh_down, foliage_down;
    int mods = mods_held();
    int mesh_pressed = (GetAsyncKeyState(cfg_vk_mesh) & 0x8000) != 0;
    int foliage_pressed = (GetAsyncKeyState(cfg_vk_foliage) & 0x8000) != 0;
    int m = mesh_pressed && mods == cfg_mod_mesh;
    int f = foliage_pressed && mods == cfg_mod_foliage;

    if (m && !mesh_down && cfg_mesh_step_n > 0) {
        g_step_mesh = (g_step_mesh + 1) % cfg_mesh_step_n;
        cfg_mesh = cfg_mesh_steps[g_step_mesh];
        logf_("hotkey: mesh_scale -> %.2f", cfg_mesh);
        osd_show("Mesh LOD  %.1f   (%d/%d)", cfg_mesh, g_step_mesh + 1, cfg_mesh_step_n);
    }
    if (f && !foliage_down && cfg_foliage_step_n > 0) {
        g_step_foliage = (g_step_foliage + 1) % cfg_foliage_step_n;
        cfg_foliage = cfg_foliage_steps[g_step_foliage];
        logf_("hotkey: foliage_scale -> %.2f", cfg_foliage);
        osd_show("Foliage LOD  %.1f   (%d/%d)", cfg_foliage, g_step_foliage + 1, cfg_foliage_step_n);
    }
    // Track physical edges: releasing Shift while F9 remains held must not
    // turn a distance-key press into an additional ordinary LOD-key press.
    mesh_down = mesh_pressed; foliage_down = foliage_pressed;
}

static float next_distance_scale(float current)
{
    static const float steps[] = { 1.0f, 1.5f, 2.0f, 3.0f };
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (steps[i] > current + 0.01f) return steps[i];
    return 1.0f;
}

static void poll_distance_hotkeys(void)
{
    if (cfg_preset_mode) return; // preset mode replaces all four separate controls
    static int visibility_down, shadow_down;
    int mods = mods_held();
    int visibility_pressed = (GetAsyncKeyState(cfg_vk_visibility) & 0x8000) != 0;
    int shadow_pressed = (GetAsyncKeyState(cfg_vk_shadow_range) & 0x8000) != 0;
    int v = visibility_pressed && mods == cfg_mod_visibility;
    int s = shadow_pressed && mods == cfg_mod_shadow_range;
    if (v && !visibility_down) {
        cfg_visibility = next_distance_scale(cfg_visibility);
        ef_range_sq = cfg_visibility * cfg_visibility;
        logf_("distance hotkey: visibility=%.2fx (all mesh proxies)", cfg_visibility);
        osd_show("Visibility range  %.1fx  (all meshes)", cfg_visibility);
    }
    if (s && !shadow_down) {
        cfg_shadow_range = next_distance_scale(cfg_shadow_range);
        ef_shadow_sq = cfg_shadow_range * cfg_shadow_range;
        logf_("distance hotkey: shadow=%.2fx (mesh caster gate)", cfg_shadow_range);
        osd_show("Shadow range  %.1fx  (experimental)", cfg_shadow_range);
    }
    visibility_down = visibility_pressed;
    shadow_down = shadow_pressed;
}

static void distance_tick(void)
{
    static DWORD last_attempt, last_stats;
    static int attempts;
    DWORD now = GetTickCount();
    if (!cfg_distance) return;
    if (!ef_distance_active) {
        settings_view view;
        unsigned char* base = (unsigned char*)GetModuleHandleA(NULL);
        size_t span = 0;
        HMODULE pinned;
        if (attempts >= 30 || (last_attempt && now-last_attempt < 1000) ||
            !grab_settings(&view)) return;
        last_attempt = now;
        attempts++;
        ef_range_rva = ga_rva(EF_RANGE_RVA);
        ef_shadow_rva = ga_rva(EF_SHADOW_RVA);
        ef_select_build(g_build == BUILD_GOG);   // GOG: own bytes and range bridge
        __try {
            const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
            const IMAGE_NT_HEADERS32* pe = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && pe->Signature == IMAGE_NT_SIGNATURE &&
                pe->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                pe->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
                span = pe->OptionalHeader.SizeOfImage;
        } __except (EXCEPTION_EXECUTE_HANDLER) { span = 0; }
        // Installed jumps must not outlive this DLL if a caller releases its handle.
        if (!span || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)(uintptr_t)ef_range_bridge, &pinned) ||
                !ef_distance_install(base, span)) {
            if (attempts == 1 || attempts == 30)
                logf_("distance: install deferred/refused (signature, PE or thread check), attempt=%d", attempts);
            return;
        }
        logf_("distance: hooks installed at RVA %08X / %08X; visibility=%.2fx shadow=%.2fx",
              (unsigned)ef_range_rva, (unsigned)ef_shadow_rva, cfg_visibility, cfg_shadow_range);
        if (cfg_preset_mode) {
            if (ef_billboard_install((unsigned char*)ga(VA_BILLBOARD_SHOW)))
                logf_("billboards: hook installed, factor %.2fx", ef_billboard_factor);
            else
                logf_("billboards: install refused, plants keep the game's billboard distance");
        }
        last_stats = now;
    }
    if (cfg_hotkeys) poll_distance_hotkeys();
    if (now-last_stats >= 5000) {
        LONG range_hits = InterlockedExchange(&ef_range_hits, 0);
        LONG shadow_hits = InterlockedExchange(&ef_shadow_hits, 0);
        LONG rejected = InterlockedExchange(&ef_range_rejected, 0);
        LONG admitted = InterlockedExchange(&ef_range_admitted, 0);
        if (range_hits || shadow_hits)
            logf_("distance stats: mesh=%ld shadow=%ld baseline_reject=%ld admitted=%ld (%.2fx/%.2fx)",
                  range_hits, shadow_hits, rejected, admitted, cfg_visibility, cfg_shadow_range);
        {
            LONG new_billboards = InterlockedExchange(&ef_billboard_hits, 0);
            LONG moved = InterlockedExchange(&ef_billboard_moved, 0);
            if (new_billboards)
                logf_("billboards: %ld new proxies, %ld moved; last %.1f m -> %.1f m (%.2fx)", new_billboards, moved,
                      ef_billboard_last_file, ef_billboard_last_used, ef_billboard_factor);
        }
        last_stats = now;
    }
}

// Shadow quality (docs/fixes/npc-streaming-sun-shadows.md, distance_patch.h).
#define VA_SUN_ATLAS_SIZE       0x009DC72Fu   // mov dword ptr [eax+20h],400h in the renderer constructor 009DC712
#define VA_SUN_ATLAS_ALLOC      0x00B12E39u   // __stdcall (renderer), RET 4: creates the atlas from +0x20
#define VA_SUN_ATLAS_CALL       0x009DDC39u   // its call in the device reset 009DDBD3
#define VA_SUN_POISSON_SET      0x009D0C70u   // sets a pixel constant: EAX device, stack (register, vector), RET 8
#define VA_SUN_POISSON_CALL     0x00AECA7Au   // its call for vPoissonSamples, in FUN_00AEC932
#define VA_RENDERER             0x0160C6ECu   // the renderer: +0x20 cascade size, +0x24/+0x28 the atlas
#define VA_SUN_CASCADE_FIRST    0x009C016Au   // movss xmm0,[00F11340] in SetupRenderFrameCollector 009C011E
#define VA_SUN_CASCADE_FIRST_LEN 0x00F11340u  // the float 7.0 that loads, shared with other code
#define VA_SUN_CASCADE_MID      0x009C019Au   // movss xmm0,[00F117B4] in the same function
#define VA_SUN_CASCADE_MID_LEN  0x00F117B4u   // the float 17.0 that loads, shared with other code
#define VA_SUN_CASCADE_FAR      0x009C01AAu   // movss xmm0,[00F11328] in the same function
#define VA_SUN_CASCADE_FAR_LEN  0x00F11328u   // the float 40.0 that loads, shared with other code

static int cascade_pressed(int vk, int mods, int* down)
{
    int pressed = (GetAsyncKeyState(vk) & 0x8000) != 0, fire = 0;
    if (pressed && !*down && mods_held() == mods) {
        DWORD pid = 0;
        HWND foreground = GetForegroundWindow();
        if (foreground) GetWindowThreadProcessId(foreground, &pid);
        fire = pid == GetCurrentProcessId();
    }
    *down = pressed;
    return fire;
}

// A new level. The zones and the filter apply on the next frame, the viewport tick switches the atlas.
static void shadow_change(int level, int from_hotkey)
{
    const shadow_level* l = &cfg_shadow[level];
    shadow_select(level);
    logf_("shadows: %s, zones %.0f / %.0f / %.0f m, atlas %d, filter %.2f (%s)", l->name,
          ef_cascade_first(l->first), ef_cascade_seam_end(l->first, l->seam),
          ef_cascade_far_end(l->first, l->seam, l->far_end), l->atlas, l->filter,
          from_hotkey ? "hotkey" : "options menu");
    if (from_hotkey) {
        osd_show("Shadow quality: %s  (%d/%d)", l->name, level + 1, SHADOW_LEVELS);
        InterlockedExchange(&g_shadowSync, 1);   // the row follows the hotkey
    }
    if (l->atlas != g_shadowAtlasNow && !g_shadowLive) {
        logf_("shadows: atlas %d applies at the next start", l->atlas);
        if (from_hotkey) osd_show("Shadow quality: %s, restart for the full effect", l->name);
    }
}

static void cascade_tick(void)
{
    static DWORD last_attempt;
    static int attempts, key_down, atlas_logged;
    LONG menu, result;
    if (!cfg_cascade) return;
    if (!atlas_logged) {
        // What the renderer really got: the proof that the atlas patch came in time.
        void** slot = (void**)ga(VA_RENDERER);
        unsigned char* renderer = range_readable(slot, sizeof(void*)) ? (unsigned char*)read_ptr(slot) : NULL;
        if (renderer && range_readable(renderer + 0x20, 4)) {
            logf_("shadows: atlas %d per cascade, wanted %ld", *(int*)(renderer + 0x20), g_shadowAtlasWant);
            atlas_logged = 1;
        }
    }
    if (!ef_cascade_active) {
        DWORD now = GetTickCount();
        unsigned char* sites[3];
        uintptr_t lens[3];
        settings_view view;
        HMODULE pinned;
        int readable = 1;
        if (attempts >= 30 || (last_attempt && now-last_attempt < 1000) ||
            !grab_settings(&view)) return;
        last_attempt = now;
        attempts++;
        sites[0] = (unsigned char*)ga(VA_SUN_CASCADE_FIRST);
        lens[0] = ga(VA_SUN_CASCADE_FIRST_LEN);
        sites[1] = (unsigned char*)ga(VA_SUN_CASCADE_MID);
        lens[1] = ga(VA_SUN_CASCADE_MID_LEN);
        sites[2] = (unsigned char*)ga(VA_SUN_CASCADE_FAR);
        lens[2] = ga(VA_SUN_CASCADE_FAR_LEN);
        for (int i = 0; i < 3; i++)
            if (!range_readable(sites[i], 8) || !range_readable((const void*)lens[i], 4)) readable = 0;
        // The patched loads must not outlive this DLL if a caller releases its handle.
        if (!readable || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)(uintptr_t)&ef_cascade_len, &pinned) ||
            !ef_cascade_install(sites, lens)) {
            if (attempts == 1 || attempts == 30)
                logf_("shadows: zones install deferred/refused (bytes or thread check), attempt=%d", attempts);
            return;
        }
        logf_("shadows: zones patched at %08X, %08X and %08X", (unsigned)(uintptr_t)sites[0],
              (unsigned)(uintptr_t)sites[1], (unsigned)(uintptr_t)sites[2]);
    }
    menu = InterlockedExchange(&g_shadowMenu, -1);
    if (menu >= 0 && menu < SHADOW_LEVELS && menu != g_shadow) shadow_change((int)menu, 0);
    result = InterlockedExchange(&g_shadowResult, 0);
    if (result > 0) {
        logf_("shadows: atlas switched to %ld per cascade", g_shadowAtlasNow);
    } else if (result < 0) {
        logf_("shadows: atlas switch refused, stays %ld per cascade", g_shadowAtlasNow);
        osd_show("Shadow quality: restart the game to apply");
    }
    // Preset mode only: without it this key is the legacy mesh shadow control.
    if (cfg_hotkeys && cfg_preset_mode && cascade_pressed(cfg_vk_shadow, cfg_mod_shadow, &key_down))
        shadow_change((g_shadow + 1) % SHADOW_LEVELS, 1);
}

// In the device reset, after the old atlas is gone: the size is applied here, on the game's thread,
// in step with the new atlas, because the per-cascade viewport reads it every frame.
static void __stdcall shadow_atlas_alloc(void* renderer)
{
    LONG want = g_shadowAtlasWant;
    *(LONG*)((char*)renderer + 0x20) = want;
    InterlockedExchange(&g_shadowAtlasNow, want);
    InterlockedExchange(&g_shadowAllocRan, 1);
    ((void (__stdcall*)(void*))ga(VA_SUN_ATLAS_ALLOC))(renderer);
}

// Runs before the renderer is constructed, polling until the stub has decrypted the code.
static DWORD WINAPI shadow_thread(LPVOID param)
{
    unsigned char* site = (unsigned char*)ga(VA_SUN_ATLAS_SIZE);
    int stored = wf_user_ini_shadow_quality();
    int atlas_left, poisson_left = 1, alloc_left = 1;
    HMODULE pinned;
    (void)param;
    if (stored >= 0 && cfg_wf_enabled && cfg_window_mode) {
        shadow_select(stored);
        logf_("shadows: startup=%s (options menu)", cfg_shadow[stored].name);
    }
    InterlockedExchange(&g_shadowSync, 1);   // the row opens on the level in use
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)shadow_atlas_alloc, &pinned)) {
        logf_("shadows: could not pin the DLL - the filter and the atlas stay the game's");
        return 0;
    }
    atlas_left = g_shadowAtlasWant != 1024;
    ef_poisson_target = (void*)ga(VA_SUN_POISSON_SET);
    for (int i = 0; i < 30000 && (atlas_left || poisson_left || alloc_left); i++) {
        if (atlas_left && range_readable(site, sizeof ef_atlas_signature) &&
            ef_atlas_patch(site, (int)g_shadowAtlasWant)) {
            InterlockedExchange(&g_shadowAtlasNow, g_shadowAtlasWant);
            atlas_left = 0;
        }
        if (poisson_left && patch_call(ga(VA_SUN_POISSON_CALL), ga(VA_SUN_POISSON_SET), (void*)ef_poisson_bridge))
            poisson_left = 0;
        if (alloc_left && patch_call(ga(VA_SUN_ATLAS_CALL), ga(VA_SUN_ATLAS_ALLOC), (void*)shadow_atlas_alloc))
            alloc_left = 0;
        if (atlas_left || poisson_left || alloc_left) Sleep(1);
    }
    logf_("shadows: level %s; atlas at start %ld%s, filter %s, live atlas %s", cfg_shadow[g_shadow].name,
          g_shadowAtlasNow, atlas_left ? " (patch missed)" : "", poisson_left ? "off" : "on", alloc_left ? "off" : "on");
    return 0;
}

// ---- save thumbnails ----------------------------------------------------
// Missing save previews ([saves] fix_thumbnails=1). The save tick 009B63DE queues an
// in-memory 640x360 capture (009B6BC5: buffer +0x3C, size +0x40) and then saves in the
// same tick (009B79AD -> 009B7149). The capture is encoded about a frame later, so
// 009B7149 still sees size 0 and writes no _640x360.bmp. We keep the finished BMP from
// D3DXSaveSurfaceToFileInMemory and write it through the game's own ISteamRemoteStorage
// right after, under the name the game would have used. If the game did write the BMP
// itself, nothing is added.
//
// Diagnostics ([diagnostics] save_trace=1) log the same path without changing anything.
// No debugger, no code patch - only import slots (found by name, each verified against
// GetProcAddress first) and slot 0 of the ISteamRemoteStorage vtable.
// See docs/fixes/save-previews.md.
//
//   kernel32 DeleteFileW, CreateFileW, CopyFileW (Steam slots 00CAD314, 00CAD320, 00CAD308)
//   d3dx9_39 D3DXSaveSurfaceToFileW, D3DXSaveSurfaceToFileInMemory (00CAD4A8, 00CAD4AC)
//   steam_api SteamRemoteStorage (00CAD624; the accessor, called, never patched)
//   DAT_01404950 game/engine: +0x234 CloudStorage flag, +0x230 provider, +0x114 storage state
//   DAT_0160D0ED capture pending, DAT_0160D0EE capture requested (009B747E / 009B733A)

#define VA_GAME            0x01404950u   // CGame*: +0x40 world, +0xE8 queued world requests, +0x234 cloud flag
#define VA_CAPTURE_PENDING 0x0160D0EDu
#define VA_CAPTURE_QUEUED  0x0160D0EEu

static int patch_vtable(void* obj, int index, void* fn, void** orig);

static int trace_path(const wchar_t* p)
{
    return p && (wcsstr(p, L"640x360") || wcsstr(p, L".bmp") || wcsstr(p, L".BMP"));
}

typedef HANDLE (WINAPI *CreateFileW_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *DeleteFileW_t)(LPCWSTR);
typedef BOOL (WINAPI *CopyFileW_t)(LPCWSTR, LPCWSTR, BOOL);
// D3DXSaveSurfaceToFileW(dest, D3DXIMAGE_FILEFORMAT, IDirect3DSurface9*, PALETTEENTRY*, RECT*)
typedef HRESULT (WINAPI *SaveFile_t)(LPCWSTR, DWORD, void*, const void*, const RECT*);
// D3DXSaveSurfaceToFileInMemory(ID3DXBuffer**, D3DXIMAGE_FILEFORMAT, IDirect3DSurface9*, ...)
typedef HRESULT (WINAPI *SaveMem_t)(void**, DWORD, void*, const void*, const RECT*);
// ISteamRemoteStorage004 slot 0, thiscall: bool FileWrite(const char*, const void*, int32).
// Confirmed at 0052AD91..ADA5: ECX = interface, three pushes, callee cleans.
typedef unsigned char (__fastcall *FileWrite_t)(void* self, void* edx, const char* name, const void* data, int size);
typedef void* (__cdecl *SteamRS_t)(void);

static CreateFileW_t g_origCreateFileW;
static DeleteFileW_t g_origDeleteFileW;
static CopyFileW_t   g_origCopyFileW;
static SaveFile_t    g_origSaveFile;
static SaveMem_t     g_origSaveMem;
static FileWrite_t   g_origFileWrite;

// IDirect3DSurface9::GetDesc is vtable slot 12; D3DSURFACE_DESC is eight DWORDs
// (Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width, Height).
static void log_surface(const char* what, void* surf, DWORD* width, DWORD* height)
{
    DWORD desc[8] = { 0 };
    HRESULT hr = E_POINTER;
    __try {
        if (surf) hr = ((HRESULT (WINAPI*)(void*, DWORD*))(*(void***)surf)[12])(surf, desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) { hr = E_FAIL; }
    *width = desc[6]; *height = desc[7];
    if (cfg_save_trace)
        logf_("save-trace: %s source %lux%lu format=%lu pool=%lu msaa=%lu (GetDesc %08lX)",
              what, desc[6], desc[7], desc[0], desc[3], desc[4], (unsigned long)hr);
}

// Thumbnail hand-over between the render thread (D3DX hook), the game thread
// (FileWrite hook) and the worker, which performs the late write.
#define THUMB_W        640
#define THUMB_H        360
#define THUMB_MAX      0x200000   // the game's capture buffer (009B70DA)
#define THUMB_WINDOW   10000      // ms a .sav waits for its capture
#define D3DXIFF_BMP_ID 0

static CRITICAL_SECTION g_thumbLock;
static void*          g_remoteStorage;
static unsigned char* g_thumbData;     // latest unclaimed 640x360 BMP
static DWORD          g_thumbSize, g_thumbTick;
static char           g_saveName[MAX_PATH];   // .sav still waiting for its BMP
static DWORD          g_saveTick;
static char           g_lastBmp[MAX_PATH];    // last BMP the game wrote itself
static DWORD          g_lastBmpTick;

static int ends_with(const char* s, const char* tail)
{
    size_t n = strlen(s), t = strlen(tail);
    return n >= t && _stricmp(s + n - t, tail) == 0;
}

// "ManualSave_0010.sav" -> "ManualSave_0010_640x360.bmp" (format from 009B70DA)
static int thumb_name(const char* sav, char* out, size_t cap)
{
    size_t n = strlen(sav);
    if (n < 5 || n - 4 + 13 >= cap) return 0;
    memcpy(out, sav, n - 4);
    sprintf_s(out + n - 4, cap - (n - 4), "_%dx%d.bmp", THUMB_W, THUMB_H);
    return 1;
}

static void thumb_capture(const void* data, DWORD size)
{
    unsigned char* copy;
    if (!size || size > THUMB_MAX || !(copy = (unsigned char*)malloc(size))) return;
    __try { memcpy(copy, data, size); }
    __except (EXCEPTION_EXECUTE_HANDLER) { free(copy); return; }
    EnterCriticalSection(&g_thumbLock);
    free(g_thumbData);
    g_thumbData = copy; g_thumbSize = size; g_thumbTick = GetTickCount();
    LeaveCriticalSection(&g_thumbLock);
}

// The Load menu's image registry (vtable 00EC6194) keeps a preview per file name for
// the whole session: AddImage (slot 2, 00B28657) ignores a name it already holds. A
// QuickSave rewrites the same QuickSave_640x360.bmp, so the menu kept showing the old
// picture until a restart. Names rewritten this session are evicted with the
// registry's own Remove (slot 3, 00B285C6) just before the menu adds them again.
#define VA_IMAGES_VTBL_ADD 0x00EC619Cu   // 00EC6194 + 8
#define VA_IMAGES_ADD      0x00B28657u   // thiscall AddImage(String* name, stream)
#define VA_IMAGES_REMOVE   0x00B285C6u   // thiscall Remove(key)
#define VA_IMAGE_KEY       0x00B30E9Fu   // stdcall key = ImageKey(String* name)
#define STALE_MAX          32

// The save list leak ([saves] fix_list_leak=1). For every save 009A49B9 opens the
// preview through Steam (whole BMP in memory, reader vtable 00F02C2C) and passes the
// stream to AddImage, then overwrites the pointer (EDI, 009A4B51..4BD8). AddImage only
// takes the stream when the name is new; for a name it already holds it returns at
// 00B2867F and the ~0.9 MB stream is never freed. Measured: +50 MB per menu open with
// 60 saves and no new previews. Inside the list builder we do the same lookup first
// and, when the name is held, delete the stream with its own destructor (slot 0, 1).
#define VA_MAP_FIND        0x004A4A03u   // bool Find(EAX = map, key*, out*), ret 8
#define VA_STEAM_STREAM    0x00F02C2Cu   // vtable of the provider's read stream (0052ADFF)
#define VA_STREAM_DTOR     0x0052AFAAu   // its scalar deleting destructor, slot 0

typedef void (__fastcall *AddImage_t)(void* self, void* edx, void* name, void* stream);
typedef void (__fastcall *RemoveImage_t)(void* self, void* edx, unsigned key);
typedef unsigned (__stdcall *ImageKey_t)(void* name);

static AddImage_t g_origAddImage;
static wchar_t    g_stale[STALE_MAX][64];   // previews rewritten since the menu last read them

// Save-list counters ([diagnostics] savelist_trace), filled by the hooks while the list
// builder 009A49B9 runs on the game thread. See "save list trace" below.
#define SEEN_MAX 4096
static volatile LONG g_listDepth;
static LONG     g_listImages, g_listNewImages, g_listReadsSav, g_listReadsOther, g_listFreed;
static LONG     g_freedTotal;
static int      g_leakFixOn;    // set once the lookup and the destructor are recognised
static LONGLONG g_listBytes;
static DWORD    g_listReadMs;
static LONGLONG g_listAddUs;        // time in AddImage (decoding new previews)
static unsigned g_seen[SEEN_MAX];   // name hashes AddImage has seen this session
static int      g_seenCount;

static LONGLONG qpc_us(void)
{
    static LONGLONG freq;
    LARGE_INTEGER t;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart ? f.QuadPart : 1; }
    QueryPerformanceCounter(&t);
    return t.QuadPart / freq * 1000000 + t.QuadPart % freq * 1000000 / freq;
}

static unsigned name_hash(const wchar_t* s)
{
    unsigned h = 2166136261u;   // FNV-1a, case-folded
    for (; *s; s++) h = (h ^ (unsigned)towlower(*s)) * 16777619u;
    return h ? h : 1;
}

static void savelist_note_image(const wchar_t* text)
{
    unsigned h = name_hash(text);
    int i;
    g_listImages++;
    for (i = 0; i < g_seenCount; i++) if (g_seen[i] == h) return;
    if (g_seenCount < SEEN_MAX) g_seen[g_seenCount++] = h;
    g_listNewImages++;
}

static void thumb_mark_stale(const char* bmp)
{
    static int next;   // ring: with more than STALE_MAX unread rewrites the oldest goes
    wchar_t wide[64];
    if (MultiByteToWideChar(CP_ACP, 0, bmp, -1, wide, 64) <= 0) return;
    EnterCriticalSection(&g_thumbLock);
    for (int i = 0; i < STALE_MAX; i++)
        if (!_wcsicmp(g_stale[i], wide)) { LeaveCriticalSection(&g_thumbLock); return; }
    wcscpy_s(g_stale[next], 64, wide);
    next = (next + 1) % STALE_MAX;
    LeaveCriticalSection(&g_thumbLock);
}

// AddImage's own check (00B28663..2867F): key from the name, Find in the map at +4.
static int registry_holds(void* self, void* name)
{
    unsigned key, slot = 0;
    unsigned char found = 0;
    void* map = (char*)self + 4;
    uintptr_t find = ga(VA_MAP_FIND);
    __try {
        key = ((ImageKey_t)ga(VA_IMAGE_KEY))(name);
        __asm {
            lea  ecx, slot
            push ecx
            lea  ecx, key
            push ecx
            mov  eax, map
            call find
            mov  found, al
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    return found != 0;
}

static void __fastcall hook_AddImage(void* self, void* edx, void* name, void* stream)
{
    const wchar_t* text = NULL;
    int stale = 0;
    __try { text = name ? *(const wchar_t* const*)name : NULL; }   // String: wchar_t* first
    __except (EXCEPTION_EXECUTE_HANDLER) { text = NULL; }
    if (text) {
        EnterCriticalSection(&g_thumbLock);
        for (int i = 0; i < STALE_MAX; i++) {
            __try {
                if (g_stale[i][0] && !_wcsicmp(g_stale[i], text)) { g_stale[i][0] = 0; stale = 1; }
            } __except (EXCEPTION_EXECUTE_HANDLER) { stale = 0; break; }
        }
        LeaveCriticalSection(&g_thumbLock);
        if (g_listDepth) __try { savelist_note_image(text); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (stale) {
        unsigned key = ((ImageKey_t)ga(VA_IMAGE_KEY))(name);
        ((RemoveImage_t)ga(VA_IMAGES_REMOVE))(self, NULL, key);
        logf_("thumbnail: menu reloads %ls", text);
    }
    if (g_leakFixOn && g_listDepth && stream && !stale && registry_holds(self, name)) {
        __try {
            if (*(void**)stream == (void*)ga(VA_STEAM_STREAM)) {
                ((void (__fastcall*)(void*, void*, int))ga(VA_STREAM_DTOR))(stream, NULL, 1);
                g_listFreed++; g_freedTotal++;
                return;   // the original would return here too, without the stream
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_listDepth && cfg_savelist_trace) {
        LONGLONG t0 = qpc_us();
        g_origAddImage(self, edx, name, stream);
        g_listAddUs += qpc_us() - t0;
        return;
    }
    g_origAddImage(self, edx, name, stream);
}

static int code_matches(uintptr_t at, const char* hex)
{
    unsigned char want[16];
    size_t n = 0;
    if (!hex) return 0;
    for (const char* p = hex; *p && n < sizeof want; ) {
        unsigned v;
        if (sscanf_s(p, "%2x", &v) != 1) return 0;
        want[n++] = (unsigned char)v;
        p += 2;
        while (*p == ' ') p++;
    }
    return range_readable((void*)at, n) && memcmp((void*)at, want, n) == 0;
}

// The bytes one check expects on the running build: the builds were compiled apart, so a site's
// bytes are proven per build. NULL (never matches) on a build without its own.
static const char* by_build(const char* steam, const char* gog)
{
    return g_build == BUILD_STEAM ? steam : g_build == BUILD_GOG ? gog : NULL;
}

static void hook_image_registry(void)
{
    DWORD old = 0, tmp = 0;
    void** slot = (void**)ga(VA_IMAGES_VTBL_ADD);
    if (!range_readable(slot, sizeof(void*)) || read_ptr(slot) != (void*)ga(VA_IMAGES_ADD) ||
        !code_matches(ga(VA_IMAGES_ADD), "55 8b ec 51 53 56 57 ff 75 08 8b d9") ||
        !code_matches(ga(VA_IMAGES_REMOVE), "55 8b ec 83 ec 10 83 65 fc 00 56 8d") ||
        !code_matches(ga(VA_IMAGE_KEY), "55 8b ec 83 ec 18 53 56 57 33 db 68")) {
        logf_("thumbnail: menu image registry not recognised - QuickSave preview refresh and list leak fix off");
        return;
    }
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    g_origAddImage = (AddImage_t)*slot;
    *slot = (void*)hook_AddImage;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    logf_("thumbnail: menu image registry hooked (QuickSave preview refresh)");
}

// Called from the FileWrite hook after every successful game write.
static void thumb_note_write(const char* name)
{
    char bmp[MAX_PATH];
    DWORD now = GetTickCount();
    EnterCriticalSection(&g_thumbLock);
    if (ends_with(name, ".bmp")) {
        lstrcpynA(g_lastBmp, name, sizeof g_lastBmp);
        g_lastBmpTick = now;
        thumb_mark_stale(name);
    } else if (ends_with(name, ".sav") && thumb_name(name, bmp, sizeof bmp)) {
        // 009B7149 writes the BMP before the .sav writer closes; if it did, we are done.
        if (!_stricmp(bmp, g_lastBmp) && now - g_lastBmpTick < THUMB_WINDOW) {
            g_saveName[0] = 0;
            g_lastBmp[0] = 0;   // one preview, one save: a second QuickSave must not reuse it
            free(g_thumbData); g_thumbData = NULL; g_thumbSize = 0;
        } else {
            lstrcpynA(g_saveName, name, sizeof g_saveName);
            g_saveTick = now;
        }
    }
    LeaveCriticalSection(&g_thumbLock);
}

// GOG has no storage provider (CGame+0x230 is never set; no ICloudStorage at all): the save
// manager writes <Documents>\Witcher 2\gamesaves\<name>.sav and, when its capture buffer is
// filled, <name>_640x360.bmp as local files. The race is the same as on Steam (save tick
// 004EB955: capture queue 004E64CB, then the save writer 004E721B, whose BMP step 004E6DF6
// writes only when the size at +0x40 is set), and so is the order: the BMP, then the .sav when
// its writer is released (004E74BB, DeleteFileW and a CreateFileW for writing). So
// hook_CreateFileW reports every successful open for writing, with the full path, and the late
// capture is written next to the .sav. See docs/fixes/save-previews.md, "GOG".
static wchar_t g_saveNameW[MAX_PATH];   // .sav (full path) still waiting for its BMP
static wchar_t g_lastBmpW[MAX_PATH];    // last preview the game wrote itself

static int ends_with_w(const wchar_t* s, const wchar_t* tail)
{
    size_t n = wcslen(s), t = wcslen(tail);
    return n >= t && _wcsicmp(s + n - t, tail) == 0;
}

// "...\ManualSave_0010.sav" -> "...\ManualSave_0010_640x360.bmp"
static int thumb_name_w(const wchar_t* sav, wchar_t* out, size_t cap)
{
    size_t n = wcslen(sav);
    if (n < 5 || n - 4 + 13 >= cap) return 0;
    wmemcpy(out, sav, n - 4);
    swprintf_s(out + n - 4, cap - (n - 4), L"_%dx%d.bmp", THUMB_W, THUMB_H);
    return 1;
}

static void thumb_note_open_w(const wchar_t* path)
{
    wchar_t bmp[MAX_PATH];
    DWORD now = GetTickCount();
    int sav = 0, own = 0;
    if (!path || wcslen(path) >= MAX_PATH) return;
    EnterCriticalSection(&g_thumbLock);
    if (ends_with_w(path, L"_640x360.bmp")) {
        lstrcpynW(g_lastBmpW, path, MAX_PATH);
        g_lastBmpTick = now;
    } else if (ends_with_w(path, L".sav") && thumb_name_w(path, bmp, MAX_PATH)) {
        sav = 1;
        if (!_wcsicmp(bmp, g_lastBmpW) && now - g_lastBmpTick < THUMB_WINDOW) {
            own = 1;
            g_saveNameW[0] = 0;
            g_lastBmpW[0] = 0;   // one preview, one save: a second QuickSave must not reuse it
            free(g_thumbData); g_thumbData = NULL; g_thumbSize = 0;
        } else {
            lstrcpynW(g_saveNameW, path, MAX_PATH);
            g_saveTick = now;
        }
    }
    LeaveCriticalSection(&g_thumbLock);
    if (sav && cfg_save_trace)
        logf_("thumbnail: %ls - %s", path, own ? "the game wrote its preview" : "no preview yet, waiting for the capture");
}

// hook_CreateFileW sits in the file trace section below: save_trace and file_trace
// share the one slot, so both behaviours live in a single chain.

static BOOL WINAPI hook_DeleteFileW(LPCWSTR path)
{
    BOOL ok = g_origDeleteFileW(path);
    if (trace_path(path)) {
        DWORD err = GetLastError();
        logf_("save-trace: DeleteFileW \"%ls\" -> %d (err %lu)", path, ok, err);
        SetLastError(err);
    }
    return ok;
}

static BOOL WINAPI hook_CopyFileW(LPCWSTR from, LPCWSTR to, BOOL fail_if_exists)
{
    BOOL ok = g_origCopyFileW(from, to, fail_if_exists);
    if (trace_path(from) || trace_path(to)) {
        DWORD err = GetLastError();
        logf_("save-trace: CopyFileW \"%ls\" -> \"%ls\" -> %d (err %lu)", from, to, ok, err);
        SetLastError(err);
    }
    return ok;
}

static HRESULT WINAPI hook_SaveFile(LPCWSTR dest, DWORD fmt, void* surf, const void* pal, const RECT* rc)
{
    DWORD w, h;
    log_surface("D3DXSaveSurfaceToFileW", surf, &w, &h);
    HRESULT hr = g_origSaveFile(dest, fmt, surf, pal, rc);
    logf_("save-trace: D3DXSaveSurfaceToFileW \"%ls\" fmt=%lu -> %08lX",
          dest ? dest : L"(null)", fmt, (unsigned long)hr);
    return hr;
}

static HRESULT WINAPI hook_SaveMem(void** buf, DWORD fmt, void* surf, const void* pal, const RECT* rc)
{
    DWORD w, h;
    log_surface("D3DXSaveSurfaceToFileInMemory", surf, &w, &h);
    HRESULT hr = g_origSaveMem(buf, fmt, surf, pal, rc);
    DWORD size = 0;
    const void* data = NULL;
    __try {   // ID3DXBuffer: GetBufferPointer is vtable slot 3, GetBufferSize slot 4
        if (SUCCEEDED(hr) && buf && *buf) {
            data = ((void* (WINAPI*)(void*))(*(void***)*buf)[3])(*buf);
            size = ((DWORD (WINAPI*)(void*))(*(void***)*buf)[4])(*buf);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { data = NULL; size = 0; }
    if (cfg_save_trace)
        logf_("save-trace: D3DXSaveSurfaceToFileInMemory fmt=%lu -> %08lX, %lu bytes",
              fmt, (unsigned long)hr, size);
    if (cfg_save_thumbs && data && fmt == D3DXIFF_BMP_ID && w == THUMB_W && h == THUMB_H)
        thumb_capture(data, size);
    return hr;
}

static unsigned char read_u8(uintptr_t at)
{
    unsigned char v = 0xFF;
    __try { v = *(const unsigned char*)at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { v = 0xFF; }
    return v;
}

static void log_engine_storage(void)
{
    unsigned char* engine = (unsigned char*)read_ptr((void*)ga(VA_GAME));
    if (!engine || !range_readable(engine, 0x238)) { logf_("save-trace: engine not readable"); return; }
    logf_("save-trace: engine cloud=%u provider=%08X state=%d pending=%u queued=%u",
          read_u8((uintptr_t)engine + 0x234), (unsigned)(uintptr_t)read_ptr(engine + 0x230),
          (int)(uintptr_t)read_ptr(engine + 0x114),
          read_u8(ga(VA_CAPTURE_PENDING)), read_u8(ga(VA_CAPTURE_QUEUED)));
}

static unsigned char __fastcall hook_FileWrite(void* self, void* edx, const char* name, const void* data, int size)
{
    unsigned char ok = g_origFileWrite(self, edx, name, data, size);
    if (cfg_save_trace) {
        logf_("save-trace: Steam FileWrite \"%s\" %d bytes -> %u", name ? name : "(null)", size, ok);
        if (name && strstr(name, ".sav")) log_engine_storage();
    }
    if (cfg_save_thumbs && ok && name && self == g_remoteStorage) thumb_note_write(name);
    return ok;
}

// GOG: the same pairing, and the BMP goes next to the .sav with plain file calls (the DLL's
// own CreateFileW import, so the game's hook chain is not involved).
static void thumb_tick_local(void)
{
    wchar_t sav[MAX_PATH], bmp[MAX_PATH];
    unsigned char* data = NULL;
    DWORD size = 0, now = GetTickCount(), wrote = 0;
    HANDLE f;
    BOOL ok = FALSE;
    EnterCriticalSection(&g_thumbLock);
    if (g_saveNameW[0]) {
        LONG age = (LONG)(g_thumbTick - g_saveTick);
        if (g_thumbData && age < -2000) {   // left over from before this save
            free(g_thumbData); g_thumbData = NULL; g_thumbSize = 0;
        }
        if (g_thumbData && age < THUMB_WINDOW) {
            lstrcpynW(sav, g_saveNameW, MAX_PATH);
            data = g_thumbData; size = g_thumbSize;
            g_thumbData = NULL; g_thumbSize = 0;
            g_saveNameW[0] = 0;
        } else if (now - g_saveTick >= THUMB_WINDOW) {
            logf_("thumbnail: no capture arrived for %ls", g_saveNameW);
            g_saveNameW[0] = 0;
        }
    }
    LeaveCriticalSection(&g_thumbLock);
    if (!data) return;
    if (thumb_name_w(sav, bmp, MAX_PATH)) {
        f = CreateFileW(bmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            ok = WriteFile(f, data, size, &wrote, NULL) && wrote == size;
            CloseHandle(f);
            if (!ok) DeleteFileW(bmp);   // no half preview
        }
        logf_("thumbnail: wrote %ls (%lu bytes) -> %d", bmp, size, ok);
    }
    free(data);
}

// Worker thread: pair a .sav that got no BMP with the capture that finished after it.
static void thumb_tick(void)
{
    char sav[MAX_PATH], bmp[MAX_PATH];
    unsigned char* data = NULL;
    DWORD size = 0, now = GetTickCount();
    if (!cfg_save_thumbs) return;
    if (g_build == BUILD_GOG) { thumb_tick_local(); return; }
    if (!g_origFileWrite || !g_remoteStorage) return;
    EnterCriticalSection(&g_thumbLock);
    if (g_saveName[0]) {
        // The capture must belong to this save: finished at most 2 s before the .sav
        // closed (between 009B7149 and the writer close) and within the window after.
        LONG age = (LONG)(g_thumbTick - g_saveTick);
        if (g_thumbData && age < -2000) {   // left over from before this save
            free(g_thumbData); g_thumbData = NULL; g_thumbSize = 0;
        }
        if (g_thumbData && age < THUMB_WINDOW) {
            lstrcpynA(sav, g_saveName, sizeof sav);
            data = g_thumbData; size = g_thumbSize;
            g_thumbData = NULL; g_thumbSize = 0;
            g_saveName[0] = 0;
        } else if (now - g_saveTick >= THUMB_WINDOW) {
            logf_("thumbnail: no capture arrived for %s", g_saveName);
            g_saveName[0] = 0;
        }
    }
    LeaveCriticalSection(&g_thumbLock);
    if (!data) return;
    if (thumb_name(sav, bmp, sizeof bmp)) {
        unsigned char ok = g_origFileWrite(g_remoteStorage, NULL, bmp, data, (int)size);
        logf_("thumbnail: wrote %s (%lu bytes) for %s -> %u", bmp, size, sav, ok);
        if (ok) thumb_mark_stale(bmp);
    }
    free(data);
}

// ---- save list trace ----------------------------------------------------
// [diagnostics] savelist_trace=1: measure what building the save list costs, without
// changing it. 009A49B9 builds the list at game start (009A515C, call 009A5175) and
// whenever the GUI asks for it (009A5245, call 009A524F). Per save it reads the whole
// .sav through Steam (GetFileSize + FileRead, slot 1, via 0052ADFF) and hands the
// preview BMP to the image registry, which keeps it for the session (AddImage, see
// above). Both call sites are redirected to a wrapper that logs time, Steam reads,
// previews added (new this session or already held) and process memory before/after.
// A memory line is also logged every 60 s. See docs/fixes/save-bloat.md.
#define VA_SAVELIST        0x009A49B9u   // stdcall void BuildSaveList(panel)
#define VA_SAVELIST_CALL_A 0x009A5175u   // E8 rel32, startup / main menu
#define VA_SAVELIST_CALL_B 0x009A524Fu   // E8 rel32, GUI request

// ISteamRemoteStorage004 slot 1, thiscall: int32 FileRead(const char*, void*, int32).
// Shape from 0052AEB6..AECA: ECX = interface, three pushes, callee cleans.
typedef int (__fastcall *FileRead_t)(void* self, void* edx, const char* name, void* data, int size);
typedef void (__stdcall *SaveList_t)(void* panel);
typedef void (__fastcall *SaveListGog_t)(void* panel, void* edx);   // GOG 005072D1: ECX = panel, plain RET

static FileRead_t g_origFileRead;

static int __fastcall hook_FileRead(void* self, void* edx, const char* name, void* data, int size)
{
    DWORD t0 = g_listDepth ? GetTickCount() : 0;
    int got = g_origFileRead(self, edx, name, data, size);
    if (g_listDepth) {
        g_listReadMs += GetTickCount() - t0;
        if (got > 0) g_listBytes += got;
        if (name && ends_with(name, ".sav")) g_listReadsSav++; else g_listReadsOther++;
    }
    return got;
}

// ---- fast save list reads ---------------------------------------------------
// [saves] fast_list=1. A .sav is a DZIP with one entry (the SAVY save) made of
// independently LZF-compressed 64 KiB blocks; a late chapter-1 save is 1.7 MB on disk and
// 12.5 MB unpacked. For every save the list opens it through the Steam provider (slot
// +0x18, 00931DB7 -> 0052ADFF) in mode 1: whole file from Steam, written to
// "Documents\Witcher 2\<name>.tmp", reopened and unpacked in full (00A4E42E) into a memory
// stream - only for 009B76FD to read timeInfo, saveInfo, worldInfo and timeManger, which
// sit in two or three blocks. Inside the list builder we open in mode 0 instead (same
// Steam read, compressed bytes in a memory stream, no temp file) and point that memory
// stream at reserved, uncommitted address space the size of the unpacked save. The first
// read of a 64 KiB page faults; the handler commits the page and unpacks that one block
// into it. Whatever the reader touches is byte-identical to the full unpack.
// One region is live at a time: 009B76FD destroys the reader before the next save opens.
#define VA_PROV_VTBL_OPEN  0x00F02B90u   // Steam provider vtable slot +0x18
#define VA_PROV_OPEN       0x00931DB7u   // thiscall Stream* Open(String* name, bool mode1)
#define VA_PROV_STREAM     0x0052ADFFu   // the provider stream's constructor
#define VA_MEM_STREAM      0x00E77440u   // memory stream vtable: [+0x10] array* {data,size}, [+0x14] pos
#define VA_MEM_SERIALIZE   0x00ABF380u   // its slot 1
#define VA_MEM_SIZE        0x00ABF379u   // its slot 3
#define VA_PROV_FORWARD    0x0052AF96u   // provider stream slot 1: forwards to the inner stream (+0x20)
// GOG has no storage provider: the save open 00AEBEDA builds the reader in place with the
// CDZipFileReader constructor (see "fast save list reads on GOG" below). Steam's own is used
// only by its provider stream, so these three are GOG's.
#define VA_DZIP_READER      0x00A4E42Eu   // CDZipFileReader ctor, (String* path), RET 4; this in EDI (ESI on GOG)
#define VA_DZIP_READER_CALL 0x00A980BCu   // its call in the save open 00A9805B (GOG 00AEBEDA, after push eax)
#define VA_DZIP_READER_VT   0x00E8BA80u   // its vtable: slots 1-4 forward to the memory stream at +0x30
#define LAZY_BLOCK         0x10000u
#define LAZY_MAX           (256u << 20)

typedef void* (__fastcall *ProvOpen_t)(void* self, void* edx, void* name, int mode);

static ProvOpen_t g_origProvOpen;
static int        g_fastBuild;      // this build uses the lazy reads
static struct {
    unsigned char* region;          // MEM_RESERVE only until a page is read
    size_t         reserved;
    void*          arr[2];          // what the memory stream reads: {data, size}
    sp_dzip        z;               // borrows the compressed bytes: the provider stream's, or own
    sp_buf         own;             // GOG: the .sav read by the DLL, freed with the region
    LONG           blocks, bad;     // pages unpacked, short or failed blocks
} g_lazy;
static LONG     g_lazyOpens, g_lazyFallbacks, g_lazyBlocks, g_lazyBlocksAll, g_lazyBad;
static LONG     g_savOpens, g_bmpOpens;
static LONGLONG g_savOpenUs, g_bmpOpenUs, g_lazyUnpackUs;

static void lazy_release(void)
{
    if (!g_lazy.region) return;
    g_lazyBlocks += g_lazy.blocks;
    g_lazyBad += g_lazy.bad;
    g_lazy.arr[1] = 0;              // a stream that outlived its reader now reads nothing
    VirtualFree(g_lazy.region, 0, MEM_RELEASE);
    sp_dzip_close(&g_lazy.z);
    sp_free(&g_lazy.own);
    g_lazy.region = NULL;
    g_lazy.reserved = 0;
    g_lazy.blocks = g_lazy.bad = 0;
}

static LONG CALLBACK lazy_fault(EXCEPTION_POINTERS* ep)
{
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    uintptr_t at, base = (uintptr_t)g_lazy.region;
    unsigned char* page;
    long long want, got;
    LONGLONG t0;
    if (!base || er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    at = (uintptr_t)er->ExceptionInformation[1];
    if (at < base || at - base >= g_lazy.reserved) return EXCEPTION_CONTINUE_SEARCH;
    page = (unsigned char*)(base + ((at - base) & ~(uintptr_t)(LAZY_BLOCK - 1)));
    if (!VirtualAlloc(page, LAZY_BLOCK, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
    t0 = qpc_us();
    want = g_lazy.z.e[0].usize - (long long)(page - g_lazy.region);
    if (want > LAZY_BLOCK) want = LAZY_BLOCK;
    got = sp_dzip_block(&g_lazy.z, 0, (long long)(page - g_lazy.region) / LAZY_BLOCK, page);
    if (got != want) g_lazy.bad++;  // the page stays (partly) zero, as a short unpack would
    g_lazy.blocks++;
    g_lazyUnpackUs += qpc_us() - t0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// The DZIP in `b` behind a reserved region the size of its unpacked entry, in g_lazy. With
// `own` the buffer is the DLL's: kept until lazy_release, or freed here on failure.
static int lazy_prepare(sp_buf* b, int own)
{
    long long usize;
    lazy_release();
    if (!b->p || !sp_dzip_open(b, &g_lazy.z)) goto fail;
    usize = g_lazy.z.count >= 1 ? g_lazy.z.e[0].usize : 0;
    if (usize <= 0 || usize > LAZY_MAX) {
        sp_dzip_close(&g_lazy.z);
        goto fail;
    }
    g_lazy.reserved = ((size_t)usize + LAZY_BLOCK - 1) & ~(size_t)(LAZY_BLOCK - 1);
    g_lazy.region = (unsigned char*)VirtualAlloc(NULL, g_lazy.reserved, MEM_RESERVE, PAGE_READWRITE);
    if (!g_lazy.region) { g_lazy.reserved = 0; sp_dzip_close(&g_lazy.z); goto fail; }
    if (own) { g_lazy.own = *b; b->p = NULL; b->n = 0; }
    g_lazy.arr[0] = g_lazy.region;
    g_lazy.arr[1] = (void*)(uintptr_t)usize;
    g_lazyBlocksAll += (LONG)(g_lazy.reserved / LAZY_BLOCK);
    return 1;
fail:
    if (own) sp_free(b);
    return 0;
}

// `outer` is a mode-0 provider stream: inner memory stream over the compressed file.
static int lazy_attach(void* outer)
{
    void** o = (void**)outer;
    void** inner;
    sp_buf b;
    __try {
        if (o[0] != (void*)ga(VA_STEAM_STREAM)) return 0;
        inner = (void**)o[0x20 / 4];
        if (!inner || inner[0] != (void*)ga(VA_MEM_STREAM) || inner[0x10 / 4] != (void*)&o[0x10 / 4])
            return 0;
        b.p = (unsigned char*)o[0x10 / 4];
        b.n = (size_t)o[0x14 / 4];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    if (!lazy_prepare(&b, 0)) return 0;
    inner[0x10 / 4] = (void*)g_lazy.arr;
    return 1;
}

static int is_sav_name(void* name)
{
    const wchar_t* s = NULL;
    size_t n;
    __try { s = name ? *(const wchar_t* const*)name : NULL; n = s ? wcslen(s) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return n > 4 && !_wcsicmp(s + n - 4, L".sav");
}

static void* __fastcall hook_ProvOpen(void* self, void* edx, void* name, int mode)
{
    void* s;
    LONGLONG t0;
    int sav;
    if (!g_listDepth) return g_origProvOpen(self, edx, name, mode);
    t0 = qpc_us();
    sav = is_sav_name(name);
    if ((mode & 0xFF) && sav && g_fastBuild) {
        s = g_origProvOpen(self, edx, name, 0);
        if (s && lazy_attach(s)) {
            g_lazyOpens++;
        } else {
            if (s) __try { ((void (__fastcall*)(void*, void*, int))ga(VA_STREAM_DTOR))(s, NULL, 1); }
                   __except (EXCEPTION_EXECUTE_HANDLER) {}
            g_lazyFallbacks++;
            s = g_origProvOpen(self, edx, name, mode);
        }
    } else {
        s = g_origProvOpen(self, edx, name, mode);
    }
    if (sav) { g_savOpens++; g_savOpenUs += qpc_us() - t0; }
    else     { g_bmpOpens++; g_bmpOpenUs += qpc_us() - t0; }
    return s;
}

static int hook_provider_open(void)
{
    DWORD old = 0, tmp = 0;
    void** slot = (void**)ga(VA_PROV_VTBL_OPEN);
    void** mem = (void**)ga(VA_MEM_STREAM);
    if (!mem || !range_readable(slot, sizeof(void*)) || read_ptr(slot) != (void*)ga(VA_PROV_OPEN) ||
        !code_matches(ga(VA_PROV_OPEN), "55 8b ec 56 57 6a 00 6a 03 6a 3c 8b f9") ||
        !code_matches(ga(VA_PROV_STREAM), "55 8b ec 83 e4 f8 81 ec 30 01 00 00") ||
        !code_matches(ga(VA_PROV_FORWARD), "55 8b ec 8b 49 20 8b 01 5d ff 60 04") ||
        !code_matches(ga(VA_STREAM_DTOR), "55 8b ec 56 8b f1 8b 4e 20 c7 06 2c 2c f0 00") ||
        read_ptr((void*)ga(VA_STEAM_STREAM)) != (void*)ga(VA_STREAM_DTOR) ||
        read_ptr(&mem[1]) != (void*)ga(VA_MEM_SERIALIZE) || read_ptr(&mem[3]) != (void*)ga(VA_MEM_SIZE) ||
        !code_matches(ga(VA_MEM_SERIALIZE), "55 8b ec 56 8b f1 8b 4e 14 8b 46 10") ||
        !code_matches(ga(VA_MEM_SIZE), "8b 41 10 8b 40 04 c3")) {
        logf_("savelist: Steam provider open not recognised - fast_list off");
        return 0;
    }
    if (!AddVectoredExceptionHandler(1, lazy_fault)) return 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return 0;
    g_origProvOpen = (ProvOpen_t)*slot;
    *slot = (void*)hook_ProvOpen;
    VirtualProtect(slot, sizeof(void*), old, &tmp);
    return 1;
}

// ---- fast save list reads on GOG ---------------------------------------------
// GOG has no storage provider. Its save open 00AEBEDA (thiscall, ECX = the gamesaves folder)
// builds the reader in place with the CDZipFileReader ctor 00AA1DFC: ESI = this (a 0x34-byte
// block the caller allocated and later frees), one stack argument (the full path String),
// RET 4, EAX = this. It reads the .sav from disk and unpacks it in full into its array at
// +0x20, with a memory stream at +0x30 over that array; the open then reads only the size
// (slot 3) and the four header sections. Inside the list builder the DLL reads the file
// itself, opens it as a DZIP and reserves the region first. Then the ctor runs with an empty
// path: its open of L"" fails cleanly, so there is no read, no unpack and no buffer. The
// stream at +0x30 is pointed at the lazy array. The reader's destructor frees +0x20, the
// stream object and the path, and nothing frees or writes through the stream's +0x10. If
// anything does not match, the reader is destroyed in place and built from the real path.
static uintptr_t g_dzipReader;   // this build's ctor, set before its call is redirected
static int       g_dzipHooked;

#pragma warning(push)
#pragma warning(disable: 4100)   // the arguments are read by the asm
__declspec(naked) static void* __stdcall dzip_reader_orig_gog(void* self, void* path)
{
    __asm {
        push esi
        mov esi, [esp + 8]                ; self
        push dword ptr [esp + 12]         ; path
        call dword ptr [g_dzipReader]     ; RET 4, EAX = self
        pop esi
        ret 8
    }
}
#pragma warning(pop)

// The empty reader as the ctor leaves it, with its stream pointed at the lazy array.
static int lazy_attach_reader(void* reader)
{
    void** r = (void**)reader;
    void** inner;
    __try {
        if (r[0] != (void*)ga(VA_DZIP_READER_VT)) return 0;
        inner = (void**)r[0x30 / 4];
        if (!inner || inner[0] != (void*)ga(VA_MEM_STREAM) || inner[0x10 / 4] != (void*)&r[0x20 / 4] ||
            r[0x20 / 4] || r[0x24 / 4])   // the empty open allocated nothing
            return 0;
        inner[0x10 / 4] = (void*)g_lazy.arr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    return 1;
}

static void* __stdcall dzip_reader_gog(void* self, void* path)
{
    static const DWORD empty[4] = { 0, 0, 0, 0 };   // String {data, size, capacity, element size}
    sp_buf file = { NULL, 0 };
    const wchar_t* text = NULL;
    LONGLONG t0;
    void* r;
    if (!g_listDepth || !is_sav_name(path)) return dzip_reader_orig_gog(self, path);
    t0 = qpc_us();
    __try { text = *(const wchar_t* const*)path; } __except (EXCEPTION_EXECUTE_HANDLER) { text = NULL; }
    if (g_fastBuild && text && sp_read_file(text, &file) && lazy_prepare(&file, 1)) {
        r = dzip_reader_orig_gog(self, (void*)empty);
        if (r == self && lazy_attach_reader(r)) {
            g_lazyOpens++;
        } else {
            // Destroy in place (scalar deleting destructor, flag 0: the block stays) and build
            // the reader from the real path, as the game would have.
            __try { ((void (__fastcall*)(void*, void*, int))(*(void***)self)[0])(self, NULL, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            lazy_release();
            g_lazyFallbacks++;
            r = dzip_reader_orig_gog(self, path);
        }
    } else {
        if (g_fastBuild) g_lazyFallbacks++;
        r = dzip_reader_orig_gog(self, path);
    }
    g_savOpens++;
    g_savOpenUs += qpc_us() - t0;
    return r;
}

// Replaces `call ctor` at 00AEBF37: ESI = this, [esp+4] = the path String.
__declspec(naked) static void dzip_reader_hook_gog(void)
{
    __asm {
        push dword ptr [esp + 4]          ; path
        push esi                          ; this
        call dzip_reader_gog              ; stdcall, EAX = the reader
        ret 4
    }
}

static int hook_dzip_reader_gog(void)
{
    uintptr_t call = ga(VA_DZIP_READER_CALL), ctor = ga(VA_DZIP_READER);
    void** vt = (void**)ga(VA_DZIP_READER_VT);
    void** mem = (void**)ga(VA_MEM_STREAM);
    if (!call || !ctor || !vt || !mem ||
        !code_matches(ctor, "55 8b ec 51 83 66 0c 00 53 57 ff 75 08 8d 4e 10") ||
        !code_matches(call - 1, "50 e8") || !range_readable(vt, 5 * sizeof(void*)) ||
        !code_matches((uintptr_t)read_ptr(&vt[1]), "55 8b ec 8b 49 30 8b 01 5d ff 60 04") ||
        !code_matches((uintptr_t)read_ptr(&vt[3]), "8b 49 30 8b 01 ff 60 0c") ||
        read_ptr(&mem[1]) != (void*)ga(VA_MEM_SERIALIZE) || read_ptr(&mem[3]) != (void*)ga(VA_MEM_SIZE) ||
        !code_matches(ga(VA_MEM_SERIALIZE), "55 8b ec 56 8b f1 8b 4e 14 8b 46 10") ||
        !code_matches(ga(VA_MEM_SIZE), "8b 41 10 8b 40 04 c3")) {
        logf_("savelist: save reader not recognised - fast_list off");
        return 0;
    }
    if (!AddVectoredExceptionHandler(1, lazy_fault)) return 0;
    g_dzipReader = ctor;
    if (!patch_call(call, ctor, (void*)dzip_reader_hook_gog)) {
        logf_("savelist: save reader call could not be patched - fast_list off");
        return 0;
    }
    g_dzipHooked = 1;
    return 1;
}

typedef struct mem_view { DWORD private_mb, commit_mb, reserve_mb, free_mb, largest_free_mb; } mem_view;

// Private bytes plus a walk of the 32-bit address space: an image that cannot be
// allocated fails on the largest free block, not on the total.
static void mem_snapshot(mem_view* m)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    MEMORY_BASIC_INFORMATION mbi;
    SIZE_T commit = 0, reserve = 0, freeb = 0, largest = 0;
    uintptr_t at = 0x10000;
    memset(m, 0, sizeof *m);
    pmc.cb = sizeof pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc))
        m->private_mb = (DWORD)(pmc.PrivateUsage >> 20);
    while (VirtualQuery((void*)at, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT) commit += mbi.RegionSize;
        else if (mbi.State == MEM_RESERVE) reserve += mbi.RegionSize;
        else { freeb += mbi.RegionSize; if (mbi.RegionSize > largest) largest = mbi.RegionSize; }
        if ((uintptr_t)mbi.BaseAddress + mbi.RegionSize <= at) break;
        at = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    m->commit_mb = (DWORD)(commit >> 20);
    m->reserve_mb = (DWORD)(reserve >> 20);
    m->free_mb = (DWORD)(freeb >> 20);
    m->largest_free_mb = (DWORD)(largest >> 20);
}

static void log_mem(const char* what)
{
    mem_view m;
    mem_snapshot(&m);
    logf_("savelist: %s private=%lu MB commit=%lu reserve=%lu free=%lu largest_free=%lu MB, previews held=%d",
          what, m.private_mb, m.commit_mb, m.reserve_mb, m.free_mb, m.largest_free_mb, g_seenCount);
}

static void savelist_call(void* panel)
{
    if (g_build == BUILD_GOG) ((SaveListGog_t)ga(VA_SAVELIST))(panel, NULL);
    else ((SaveList_t)ga(VA_SAVELIST))(panel);
}

static void savelist_run(void* panel, const char* who)
{
    static int builds;
    mem_view a, b;
    LONGLONG t0;
    builds++;
    // fast_list=2 alternates (odd builds fast) so one session measures both ways.
    g_fastBuild = (g_origProvOpen || g_dzipHooked) && (cfg_fast_list == 1 || (cfg_fast_list == 2 && (builds & 1)));
    if (!cfg_savelist_trace) {
        InterlockedIncrement(&g_listDepth);
        savelist_call(panel);
        InterlockedDecrement(&g_listDepth);
        lazy_release();
        return;
    }
    mem_snapshot(&a);
    g_listImages = g_listNewImages = g_listReadsSav = g_listReadsOther = g_listFreed = 0;
    g_listBytes = 0; g_listReadMs = 0; g_listAddUs = 0;
    g_lazyOpens = g_lazyFallbacks = g_lazyBlocks = g_lazyBlocksAll = g_lazyBad = 0;
    g_savOpens = g_bmpOpens = 0;
    g_savOpenUs = g_bmpOpenUs = g_lazyUnpackUs = 0;
    t0 = qpc_us();
    InterlockedIncrement(&g_listDepth);
    savelist_call(panel);
    InterlockedDecrement(&g_listDepth);
    lazy_release();
    t0 = qpc_us() - t0;
    mem_snapshot(&b);
    logf_("savelist: build %d (%s, %s reads) %lu ms; Steam reads %ld .sav + %ld other, %lu MB in %lu ms; "
          "previews %ld (%ld new, %d held this session), leaked streams freed %ld",
          builds, who, g_fastBuild ? "fast" : "full", (DWORD)(t0 / 1000), g_listReadsSav, g_listReadsOther,
          (DWORD)(g_listBytes >> 20), g_listReadMs, g_listImages, g_listNewImages, g_seenCount, g_listFreed);
    logf_("savelist:   .sav opens %ld in %lu ms, preview opens %ld in %lu ms, preview decode %lu ms, "
          "rest (header parse, GUI) %ld ms",
          g_savOpens, (DWORD)(g_savOpenUs / 1000), g_bmpOpens, (DWORD)(g_bmpOpenUs / 1000),
          (DWORD)(g_listAddUs / 1000),
          (long)((t0 - g_savOpenUs - g_bmpOpenUs - g_listAddUs) / 1000));
    if (g_fastBuild)
        logf_("savelist:   fast reads %ld, fallbacks %ld, blocks unpacked %ld of %ld (%lu ms), bad blocks %ld",
              g_lazyOpens, g_lazyFallbacks, g_lazyBlocks, g_lazyBlocksAll, (DWORD)(g_lazyUnpackUs / 1000), g_lazyBad);
    logf_("savelist:   private %lu -> %lu MB, commit %lu -> %lu MB, largest free %lu -> %lu MB",
          a.private_mb, b.private_mb, a.commit_mb, b.commit_mb, a.largest_free_mb, b.largest_free_mb);
}

static void __stdcall savelist_startup(void* panel) { savelist_run(panel, "startup"); }
static void __stdcall savelist_menu(void* panel)    { savelist_run(panel, "menu"); }
static void __fastcall savelist_startup_gog(void* panel, void* edx) { (void)edx; savelist_run(panel, "startup"); }
static void __fastcall savelist_menu_gog(void* panel, void* edx)    { (void)edx; savelist_run(panel, "menu"); }

// Swap one verified `call target` for a call to `hook`. The rel32 lies inside one
// cache line at every site used, so the locked exchange is seen whole by the game thread.
// The same for a `jmp target` (op 0xE9, a tail call); the opcode is kept.
static int patch_rel32(uintptr_t at, unsigned char op, uintptr_t target, void* hook)
{
    LONG want = (LONG)(target - (at + 5)), mine = (LONG)((uintptr_t)hook - (at + 5));
    DWORD old = 0, tmp = 0;
    if (!range_readable((void*)at, 5) || *(unsigned char*)at != op) return 0;
    if (*(LONG*)(at + 1) == mine) return 1;
    if (*(LONG*)(at + 1) != want) return 0;
    if (!VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    InterlockedCompareExchange((volatile LONG*)(at + 1), mine, want);
    VirtualProtect((void*)at, 5, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return *(LONG*)(at + 1) == mine;
}

static int patch_call(uintptr_t at, uintptr_t target, void* hook)
{
    return patch_rel32(at, 0xE8, target, hook);
}

// Needs the image registry hook (g_origAddImage) for the leak fix.
static void savelist_install(void)
{
    int a, b;
    int gog = g_build == BUILD_GOG;
    if (!code_matches(ga(VA_SAVELIST), by_build("55 8b ec 83 e4 f8 81 ec ec 01 00 00",
                                                 "55 8b ec 81 ec 00 02 00 00 53 56 33 db 8b f1"))) {
        logf_("savelist: list builder not recognised - leak fix, fast_list and trace off");
        return;
    }
    if (cfg_list_leak) {
        if (g_origAddImage &&
            code_matches(ga(VA_MAP_FIND), "55 8b ec 83 ec 0c 56 ff 75 08 8b f0") &&
            code_matches(ga(VA_STREAM_DTOR), "55 8b ec 56 8b f1 8b 4e 20 c7 06 2c 2c f0 00") &&
            read_ptr((void*)ga(VA_STEAM_STREAM)) == (void*)ga(VA_STREAM_DTOR))
            g_leakFixOn = 1;
        else
            logf_("savelist: registry lookup or stream destructor not recognised - leak fix off");
    }
    if (!g_leakFixOn && !cfg_savelist_trace && !cfg_fast_list) return;
    if (!gog && (cfg_fast_list || cfg_savelist_trace) && !hook_provider_open()) cfg_fast_list = 0;
    if (gog && (cfg_fast_list || cfg_savelist_trace) && !hook_dzip_reader_gog()) cfg_fast_list = 0;
    a = patch_call(ga(VA_SAVELIST_CALL_A), ga(VA_SAVELIST), gog ? (void*)savelist_startup_gog : (void*)savelist_startup);
    b = patch_call(ga(VA_SAVELIST_CALL_B), ga(VA_SAVELIST), gog ? (void*)savelist_menu_gog : (void*)savelist_menu);
    logf_("savelist: startup call %s, menu call %s; leak fix=%d fast_list=%d trace=%d",
          a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked", g_leakFixOn, cfg_fast_list, cfg_savelist_trace);
    if (cfg_savelist_trace) log_mem("at install");
}

// ---- skip the startup videos ---------------------------------------------
// [startup] skip_intro=1: start at the main menu, without the logos and the intro movie,
// by doing what Esc does on each startup movie. See docs/fixes/skip-intro.md.
// 009B581C (game start, once) builds the movie list from globals\video_start.csv with
// 009A2190 (EAX = player object; list +0x9C, count +0xA0) and ticks the player 10 times.
// The player's tick 009A211D (vtable 00ECAF04 + 0x74) plays entry [+0xB0] while no movie
// plays (+0xAD == 0) and, after the last one, sets +0xAC and calls "movies done". Esc or a
// mouse/pad button (009A1FC6) and the Flash "MovieStopped" event (009A1F92) both end the
// current movie through 009A1F2E (ESI = player). Calling "done" from the build itself
// crashed the game silently (2026-10-30: it ran inside 009B581C's ticks, before the GUI).
// So the build call at 009B588F only arms the skip.
//
// Playing is 009A2005: the Flash call PlayVideo on ui_video.swf, which opens a NetStream on the
// .usm; StopVideo (in 009A1F2E) closes it. Up to 0.12.0 the wrapped tick let the first movie
// start and stopped it 300 ms later. That was suspected of leaving the Act 1 -> Act 2 movie
// black (2026-11-07), and was refuted the same day: the black screen came back with no startup
// movie started (docs/fixes/skip-intro.md). The change stays: no startup movie is started at all:
// before the first play the wrapped tick sets the index to the count and +0xAD ("playing"), so
// the game's tick only ticks the panel. After STARTVIDEO_MIN_MS (outside 009B581C's ticks) it
// calls 009A1F2E as Esc would, StopVideo on a stream that never opened (Esc in the black wait of
// an empty list did that in game, 2026-10-30), and the next tick takes the game's own "all
// played" branch (+0xAC, "movies done"); with an empty list 009A1F2E sets done itself.
#define VA_STARTVIDEO_LIST  0x009A2190u   // builds the startup list, EAX = player object
#define VA_STARTVIDEO_CALL  0x009B588Fu   // E8 rel32 in 009B581C (after mov eax,esi)
#define VA_STARTVIDEO_TICK  0x009A211Du   // thiscall void Tick(float), ret 4
#define VA_STARTVIDEO_STOP  0x009A1F2Eu   // ends the current movie, ESI = player
#define VA_STARTVIDEO_VSLOT 0x00ECAF78u   // player vtable 00ECAF04 + 0x74 = Tick
#define STARTVIDEO_MIN_MS   300
#define STARTVIDEO_MAX_MS   60000          // give up if the sequence never finishes

typedef void (__fastcall *StartTick_t)(void* self, void* edx, float dt);

static void* volatile g_skipPlayer;        // set by the build hook, cleared when done
static DWORD g_skipArmedAt, g_skipPlayAt;
static volatile LONG g_skipStops, g_skipDone;
static uintptr_t g_startvideoList, g_startvideoStop, g_startvideoTick;   // this build's, set before the patch

static void __stdcall startvideo_after(void* player)
{
    g_skipArmedAt = GetTickCount();
    g_skipPlayAt = 0;
    InterlockedExchangePointer((void* volatile*)&g_skipPlayer, player);
    logf_("startup: movie list built (%lu entries), skip armed", *(DWORD*)((unsigned char*)player + 0xA0));
}

// Same convention as 009A2190 (object in EAX, no stack arguments, plain ret).
__declspec(naked) static void startvideo_build_hook(void)
{
    __asm {
        push eax                       ; player object, also the argument below
        mov ecx, dword ptr [g_startvideoList]
        call ecx
        call startvideo_after          ; stdcall, pops the object
        ret
    }
}

#pragma warning(push)
#pragma warning(disable: 4100)   // player is read by the asm
__declspec(naked) static void __stdcall startvideo_stop(void* player)
{
    __asm {
        push esi
        mov esi, [esp + 8]
        mov eax, dword ptr [g_startvideoStop]
        call eax
        pop esi
        ret 4
    }
}

// GOG 3.5.0.26g compiled the list builder (00509DF5) and the stop (00509D26) as thiscall: the
// player comes in ECX, not EAX/ESI. The builder is called from the start wrapper 00509EEB, which
// keeps the player in ESI and writes [ESI+0xAC] after the call; only ECX/EAX/EDX change here.
__declspec(naked) static void startvideo_build_hook_gog(void)
{
    __asm {
        push ecx                       ; player object, also the argument below
        mov eax, dword ptr [g_startvideoList]
        call eax                       ; ECX = player, as the wrapper passed it
        call startvideo_after          ; stdcall, pops the object
        ret
    }
}

__declspec(naked) static void __stdcall startvideo_stop_gog(void* player)
{
    __asm {
        mov ecx, [esp + 4]
        mov eax, dword ptr [g_startvideoStop]
        call eax
        ret 4
    }
}
#pragma warning(pop)

static int g_skipHeld;   // 1: the list is marked as played and +0xAD held, 2: stopped (game thread)

static void __fastcall startvideo_tick(void* self, void* edx, float dt)
{
    unsigned char* p = (unsigned char*)self;
    // Before the game's tick can start the first movie: every entry counts as played, and the
    // player counts as busy, so the tick only runs the panel. No PlayVideo, no stream.
    if (self == g_skipPlayer && !g_skipHeld && !p[0xAC] && !p[0xAD]) {
        DWORD count = *(DWORD*)(p + 0xA0);
        if (*(DWORD*)(p + 0xB0) < count) *(DWORD*)(p + 0xB0) = count;
        p[0xAD] = 1;
        g_skipHeld = 1;
        g_skipPlayAt = GetTickCount();
        InterlockedExchange(&g_skipStops, (LONG)count);
        logf_("startup: %lu startup movie(s) marked as played, none started", count);
    }
    ((StartTick_t)g_startvideoTick)(self, edx, dt);
    if (self != g_skipPlayer) return;
    if (p[0xAC] || GetTickCount() - g_skipArmedAt > STARTVIDEO_MAX_MS) {
        g_skipPlayer = NULL;
        InterlockedExchange(&g_skipDone, p[0xAC] ? 1 : 2);
        return;
    }
    if (g_skipHeld != 1 || GetTickCount() - g_skipPlayAt < STARTVIDEO_MIN_MS) return;
    // What Esc does; the stream it closes was never opened. The next tick finishes the list.
    g_skipHeld = 2;
    if (g_build == BUILD_GOG) startvideo_stop_gog(self); else startvideo_stop(self);
}

// 1 when the call site (`call` = VA_STARTVIDEO_CALL) and the three functions hold the running
// build's code. GOG: `jz; call builder` in the start wrapper; the tick's prologue is 4 bytes
// shorter; the stop check ends in the low 16 bits of a relocated push (stable over 64 KB load
// steps). tools/buildprobe runs this against the real GOG exe.
static int startvideo_site_ok(uintptr_t call)
{
    return code_matches(call - 2, by_build("8b c6 e8 fc c8 fe ff", "74 05 e8 f9 fe ff ff"));
}

static int startvideo_code_ok(void)
{
    return code_matches(g_startvideoList, by_build("55 8b ec 83 e4 f8 83 ec 34 53 56 05 9c 00 00 00",
                                                   "55 8b ec 83 ec 30 56 57 81 c1 9c 00 00 00 33 f6")) &&
           code_matches(g_startvideoTick + (g_build == BUILD_GOG ? 0x1B : 0x1F),
                        "8b 86 b0 00 00 00 8b 8e a0 00 00 00 3b c1 72 1b") &&
           code_matches(g_startvideoStop, by_build("55 8b ec 83 e4 f8 83 ec 10 53 33 db 57 68 1c 38",
                                                   "55 8b ec 83 ec 10 53 33 db 56 8b f1 68 18 3e"));
}

// The code is SteamStub-encrypted until the stub has run, and 009B581C runs a few
// seconds later, so poll closely from DllMain's thread start instead of the worker's
// 1.5 s delay.
static DWORD WINAPI startvideo_thread(LPVOID param)
{
    DWORD t0 = GetTickCount(), old = 0, tmp = 0;
    uintptr_t call = ga(VA_STARTVIDEO_CALL), vslot = ga(VA_STARTVIDEO_VSLOT);
    (void)param;
    g_startvideoList = ga(VA_STARTVIDEO_LIST);
    g_startvideoStop = ga(VA_STARTVIDEO_STOP);
    g_startvideoTick = ga(VA_STARTVIDEO_TICK);
    if (!call || !vslot || !g_startvideoList || !g_startvideoStop || !g_startvideoTick) {
        logf_("startup: movie code not on this build - skip_intro off");
        return 0;
    }
    while (g_running && GetTickCount() - t0 < 120000) {
        if (startvideo_site_ok(call)) {
            if (!startvideo_code_ok() || read_ptr((void*)vslot) != (void*)g_startvideoTick) {
                logf_("startup: movie code not recognised - skip_intro off");
                return 0;
            }
            if (!VirtualProtect((void*)vslot, 4, PAGE_READWRITE, &old)) {
                logf_("startup: movie tick slot not writable - skip_intro off");
                return 0;
            }
            InterlockedExchangePointer((void* volatile*)vslot, (void*)startvideo_tick);
            VirtualProtect((void*)vslot, 4, old, &tmp);
            if (patch_call(call, g_startvideoList, g_build == BUILD_GOG ? (void*)startvideo_build_hook_gog
                                                                        : (void*)startvideo_build_hook))
                logf_("startup: movie list and tick hooked after %lu ms", GetTickCount() - t0);
            else
                logf_("startup: movie list call could not be patched - skip_intro off");
            return 0;
        }
        Sleep(1);
    }
    logf_("startup: movie list call never appeared - skip_intro off");
    return 0;
}

// ---- import slots -------------------------------------------------------
// The exe's import slots the DLL hooks, found by DLL and function name in witcher2.exe's own
// import directory (imports_find, from DllMain). The packed Steam exe keeps the original
// directory unencrypted and the loader fills it before any DllMain runs
// (docs/fixes/dll-script-patches.md, Q2), so on Steam these are exactly the slots 0.9.0 had
// fixed (00CAD07C..00CAD624). GOG's lie elsewhere and move with its load address.
// 0 = the running build does not import it (GOG has no steam_api.dll).
enum {
    IMP_CREATEFILEW, IMP_CREATEFILEA, IMP_DELETEFILEW, IMP_COPYFILEW, IMP_CREATEFILEMAPPINGW,
    IMP_GETFILEATTREXW, IMP_GETFILEATTRW, IMP_GETFILEATTRA, IMP_GETFILEINFO,
    IMP_FINDFIRSTFILEW, IMP_FINDFIRSTFILEA, IMP_FINDFIRSTFILEEXW, IMP_FINDNEXTFILEW,
    IMP_FINDNEXTFILEA, IMP_GETFILESIZE, IMP_GETFILESIZEEX, IMP_READFILE, IMP_SLEEP,
    IMP_WAITSINGLE, IMP_WAITSINGLEEX, IMP_GETPROCAFFINITY, IMP_EXITPROCESS, IMP_TERMINATEPROCESS,
    IMP_GETPROCADDRESS, IMP_D3DX_SAVEFILE, IMP_D3DX_SAVEMEM, IMP_STEAM_RS, IMP_COUNT
};
static const struct { const char* dll; const char* fn; } k_imports[IMP_COUNT] = {
    { "kernel32.dll",  "CreateFileW" },
    { "kernel32.dll",  "CreateFileA" },
    { "kernel32.dll",  "DeleteFileW" },
    { "kernel32.dll",  "CopyFileW" },
    { "kernel32.dll",  "CreateFileMappingW" },
    { "kernel32.dll",  "GetFileAttributesExW" },
    { "kernel32.dll",  "GetFileAttributesW" },
    { "kernel32.dll",  "GetFileAttributesA" },
    { "kernel32.dll",  "GetFileInformationByHandle" },
    { "kernel32.dll",  "FindFirstFileW" },
    { "kernel32.dll",  "FindFirstFileA" },
    { "kernel32.dll",  "FindFirstFileExW" },
    { "kernel32.dll",  "FindNextFileW" },
    { "kernel32.dll",  "FindNextFileA" },
    { "kernel32.dll",  "GetFileSize" },
    { "kernel32.dll",  "GetFileSizeEx" },
    { "kernel32.dll",  "ReadFile" },
    { "kernel32.dll",  "Sleep" },
    { "kernel32.dll",  "WaitForSingleObject" },
    { "kernel32.dll",  "WaitForSingleObjectEx" },
    { "kernel32.dll",  "GetProcessAffinityMask" },
    { "kernel32.dll",  "ExitProcess" },
    { "kernel32.dll",  "TerminateProcess" },
    { "kernel32.dll",  "GetProcAddress" },
    { "d3dx9_39.dll",  "D3DXSaveSurfaceToFileW" },
    { "d3dx9_39.dll",  "D3DXSaveSurfaceToFileInMemory" },
    { "steam_api.dll", "SteamRemoteStorage" },
};
static uintptr_t g_imp[IMP_COUNT];

// Calls `cb` for every import by name of a loaded image, with its IAT slot. Reads only the
// image's own headers and import names: safe in DllMain.
typedef void (*import_cb)(void* ctx, const char* dll, const char* fn, uintptr_t slot);

static void imports_walk(const unsigned char* base, import_cb cb, void* ctx)
{
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS32* nt;
        const IMAGE_DATA_DIRECTORY* dir;
        const IMAGE_IMPORT_DESCRIPTOR* d;
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT) return;
        dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir->VirtualAddress) return;
        for (d = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dir->VirtualAddress); d->Name; d++) {
            const char* dll = (const char*)(base + d->Name);
            const IMAGE_THUNK_DATA32* names;
            DWORD i;
            // Without the name array (bound imports) the slots already hold addresses: skip.
            if (!d->OriginalFirstThunk || !d->FirstThunk) continue;
            names = (const IMAGE_THUNK_DATA32*)(base + d->OriginalFirstThunk);
            for (i = 0; names[i].u1.AddressOfData; i++) {
                const IMAGE_IMPORT_BY_NAME* by;
                if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
                by = (const IMAGE_IMPORT_BY_NAME*)(base + names[i].u1.AddressOfData);
                cb(ctx, dll, (const char*)by->Name, (uintptr_t)(base + d->FirstThunk) + 4 * i);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void imports_find_cb(void* ctx, const char* dll, const char* fn, uintptr_t slot)
{
    int k;
    (void)ctx;
    for (k = 0; k < IMP_COUNT; k++)
        if (!g_imp[k] && !strcmp(fn, k_imports[k].fn) && !_stricmp(dll, k_imports[k].dll))
            g_imp[k] = slot;
}

static void imports_find(void)
{
    imports_walk((const unsigned char*)g_base, imports_find_cb, NULL);
}

// What an import slot holds now; NULL when the build does not import it.
static void* imp_value(int imp)
{
    uintptr_t slot = g_imp[imp];
    return slot && range_readable((void*)slot, sizeof(void*)) ? read_ptr((void*)slot) : NULL;
}

// Replace one import slot (of the exe or another module) only if it still holds the expected
// export.
static int patch_import_slot(const char* tag, const char* dll, const char* fn, uintptr_t slot,
                             void* hook, void** orig)
{
    HMODULE mod = GetModuleHandleA(dll);
    void* expect = mod ? (void*)GetProcAddress(mod, fn) : NULL;
    void* have = slot && range_readable((void*)slot, sizeof(void*)) ? read_ptr((void*)slot) : NULL;
    if (!slot) {
        logf_("%s: %s!%s is not imported by this build - not hooked", tag, dll, fn);
        return 0;
    }
    if (have == hook) return 1;
    if (!expect || have != expect) {
        logf_("%s: %s!%s slot %08X holds %08X, expected %08X - not hooked",
              tag, dll, fn, (unsigned)slot, (unsigned)(uintptr_t)have, (unsigned)(uintptr_t)expect);
        return 0;
    }
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &old)) return 0;
    *orig = have;
    *(void**)slot = hook;
    VirtualProtect((void*)slot, sizeof(void*), old, &tmp);
    logf_("%s: hooked %s!%s", tag, dll, fn);
    return 1;
}

static int patch_iat(const char* tag, int imp, void* hook, void** orig)
{
    return patch_import_slot(tag, k_imports[imp].dll, k_imports[imp].fn, g_imp[imp], hook, orig);
}

// ---- 32 logical processors ---------------------------------------------
// [stability] cpu32_guard=1. The game's CPU topology count FUN_00930a6e (called once from
// FUN_00aa6e94) only runs when the process and system affinity masks are equal, and walks them
// with `mask = 1 << i` until `mask > system mask` or `mask == 0` (SHL EDI,CL / TEST / JNZ at
// 00930BBD). x86 masks a shift to 5 bits, so mask is never 0, and a system mask of 0xFFFFFFFF
// (32 or more logical processors; a 32-bit process sees at most 32) is never exceeded: the
// loop never ends and the game sits in the background without a window. Taking one core away
// in Task Manager makes the masks differ, which skips the count. The hook reports that same
// case without giving up the core. The only other caller, FUN_006b9a30, overwrites the value
// with 0xFFFFFFFF on success, so it is not affected. See docs/fixes/cpu32-hang.md.

typedef BOOL (WINAPI *GetProcessAffinityMask_t)(HANDLE, PDWORD_PTR, PDWORD_PTR);
static GetProcessAffinityMask_t g_realGetProcAffinity;

static BOOL WINAPI hook_GetProcessAffinityMask(HANDLE process, PDWORD_PTR proc_mask, PDWORD_PTR sys_mask)
{
    static LONG logged;
    BOOL ok = g_realGetProcAffinity(process, proc_mask, sys_mask);

    if (ok && proc_mask && sys_mask && *proc_mask == 0xFFFFFFFFu && *sys_mask == 0xFFFFFFFFu) {
        *proc_mask = 0x7FFFFFFFu;
        if (!InterlockedExchange(&logged, 1)) {
            logf_("cpu32: 32 logical processors seen, the game's CPU count is skipped");
        }
    }
    return ok;
}

// 1 = hooked or nothing more to try, 0 = the slot does not hold the export yet.
static int cpu32_try(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    void* want = k32 ? (void*)GetProcAddress(k32, "GetProcessAffinityMask") : NULL;
    void* have = imp_value(IMP_GETPROCAFFINITY);

    if (!want || have != want) return 0;
    patch_iat("cpu32", IMP_GETPROCAFFINITY, (void*)hook_GetProcessAffinityMask,
              (void**)&g_realGetProcAffinity);
    return 1;
}

static DWORD WINAPI cpu32_thread(LPVOID param)
{
    (void)param;
    for (int i = 0; i < 3000; i++) {
        if (cpu32_try()) return 0;
        Sleep(1);
    }
    logf_("cpu32: GetProcessAffinityMask slot never held the kernel32 export - off");
    return 0;
}

// ---- file trace (research for the DLL script patches) -------------------
// [diagnostics] file_trace=1 answers how the game opens its data - questions Q1-Q6, with
// the results in docs/fixes/dll-script-patches.md.
// Passive: every hook calls the real function first and changes nothing. The slots are
// kernel32 imports found by name (k_imports); on Steam they are the ones re-checked against
// the import table of witcher2.exe.unpacked.exe (docs/fixes/dll-script-patches.md):
//   CreateFileMappingW, GetFileAttributesExW, GetFileAttributesW, FindFirstFileExW,
//   FindNextFileW, FindFirstFileW, GetFileSize, GetFileSizeEx, CreateFileW.
// The game imports the ANSI variants as well (CreateFileA, GetFileAttributesA,
// FindFirstFileA, FindNextFileA) and reports sizes/times through GetFileInformationByHandle,
// so those are traced too - missing them would make a "not opened" answer wrong.
// CreateFileW is shared with save_trace: one hook chains both behaviours.
//
// The Windows loader fills the IAT (Steam: the original KERNEL32 thunks at 0x00CAD070) before
// any DllMain runs, so the slots are already usable there; filetrace_thread still polls for
// the real kernel32 export and keeps watching for a later rewrite (Q2).

#define FT_SLOTS       14       // patched IAT slots below
#define FT_MAX_LINES   100000   // log cap: the startup loose-file scan alone is ~4k lines
#define FT_MAX_HANDLES 256      // handle -> path memory for the size/mapping queries

typedef HANDLE (WINAPI *CreateFileA_t)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL   (WINAPI *GetFileAttributesExW_t)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
typedef DWORD  (WINAPI *GetFileAttributesA_t)(LPCSTR);
typedef DWORD  (WINAPI *GetFileAttributesW_t)(LPCWSTR);
typedef BOOL   (WINAPI *GetFileInformationByHandle_t)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
typedef HANDLE (WINAPI *FindFirstFileA_t)(LPCSTR, LPWIN32_FIND_DATAA);
typedef HANDLE (WINAPI *FindFirstFileW_t)(LPCWSTR, LPWIN32_FIND_DATAW);
typedef HANDLE (WINAPI *FindFirstFileExW_t)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
typedef BOOL   (WINAPI *FindNextFileA_t)(HANDLE, LPWIN32_FIND_DATAA);
typedef BOOL   (WINAPI *FindNextFileW_t)(HANDLE, LPWIN32_FIND_DATAW);
typedef DWORD  (WINAPI *GetFileSize_t)(HANDLE, LPDWORD);
typedef BOOL   (WINAPI *GetFileSizeEx_t)(HANDLE, PLARGE_INTEGER);
typedef HANDLE (WINAPI *CreateFileMappingW_t)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);

static CreateFileA_t          g_origCreateFileA;
static GetFileAttributesExW_t g_origGetFileAttributesExW;
static GetFileAttributesA_t   g_origGetFileAttributesA;
static GetFileAttributesW_t   g_origGetFileAttributesW;
static GetFileInformationByHandle_t g_origGetFileInformationByHandle;
static FindFirstFileA_t       g_origFindFirstFileA;
static FindFirstFileW_t       g_origFindFirstFileW;
static FindFirstFileExW_t     g_origFindFirstFileExW;
static FindNextFileA_t        g_origFindNextFileA;
static FindNextFileW_t        g_origFindNextFileW;
static GetFileSize_t          g_origGetFileSize;
static GetFileSizeEx_t        g_origGetFileSizeEx;
static CreateFileMappingW_t   g_origCreateFileMappingW;

static volatile LONG g_ftLines;
static DWORD         g_ftT0;
static CRITICAL_SECTION g_ftLock;
typedef struct { HANDLE h; wchar_t what[MAX_PATH]; unsigned char is_find; } ft_handle;
static ft_handle g_ftHandles[FT_MAX_HANDLES];
static int g_ftNext;

// Case-insensitive substring search ('needle' not empty).
static const wchar_t* wcsistr(const wchar_t* hay, const wchar_t* needle)
{
    size_t n = wcslen(needle);
    for (; *hay; hay++)
        if (!_wcsnicmp(hay, needle, n)) return hay;
    return NULL;
}

// Q1/Q4 targets: anything under CookedPC plus the archive, movie and icon extensions.
static int ft_match(const wchar_t* p)
{
    size_t n;
    if (!p || !*p) return 0;
    if (wcsistr(p, L"cookedpc")) return 1;
    n = wcslen(p);
    if (n >= 5 && !_wcsicmp(p + n - 5, L".dzip")) return 1;
    if (n >= 4 && (!_wcsicmp(p + n - 4, L".swf") || !_wcsicmp(p + n - 4, L".dds"))) return 1;
    return 0;
}

// Entries of a directory listing are logged only for the files whose discovery matters;
// CookedPC holds thousands of loose .dds and .xbm files.
static int ft_match_entry(const wchar_t* name)
{
    size_t n = name ? wcslen(name) : 0;
    if (n >= 5 && !_wcsicmp(name + n - 5, L".dzip")) return 1;
    return n >= 4 && !_wcsicmp(name + n - 4, L".swf");
}

static int ft_relative(const wchar_t* p)
{
    if (p[0] == L'\\' || p[0] == L'/') return 0;   // rooted, UNC or device path
    return !(p[0] && p[1] == L':');               // "C:..."
}

// One trace line: local time, ms since DllMain (Q2), thread id, then the caller text.
static void ft_log(const char* fmt, ...)
{
    LONG n;
    SYSTEMTIME st;
    if (!cfg_file_trace || !g_log || !cfg_log) return;
    n = InterlockedIncrement(&g_ftLines);
    if (n > FT_MAX_LINES) {
        if (n == FT_MAX_LINES + 1)
            logf_("file-trace: %d lines reached - further lines suppressed", FT_MAX_LINES);
        return;
    }
    GetLocalTime(&st);
    EnterCriticalSection(&g_logLock);
    fprintf(g_log, "%02d:%02d:%02d.%03d file-trace: t=+%lums tid=%lu ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetTickCount() - g_ftT0, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

static void ft_note_handle(HANDLE h, const wchar_t* what, int is_find)
{
    int i;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !what || !*what) return;
    EnterCriticalSection(&g_ftLock);
    for (i = 0; i < FT_MAX_HANDLES; i++)
        if (g_ftHandles[i].h == h) break;
    if (i == FT_MAX_HANDLES) {   // reuse the oldest slot
        i = g_ftNext;
        g_ftNext = (g_ftNext + 1) % FT_MAX_HANDLES;
    }
    g_ftHandles[i].h = h;
    g_ftHandles[i].is_find = (unsigned char)(is_find != 0);
    lstrcpynW(g_ftHandles[i].what, what, MAX_PATH);
    LeaveCriticalSection(&g_ftLock);
}

static int ft_lookup(HANDLE h, wchar_t* what, int* is_find)
{
    int i, found = 0;
    if (h == NULL || h == INVALID_HANDLE_VALUE) return 0;
    EnterCriticalSection(&g_ftLock);
    for (i = 0; i < FT_MAX_HANDLES; i++) {
        if (g_ftHandles[i].h == h) {
            if (what) lstrcpynW(what, g_ftHandles[i].what, MAX_PATH);
            if (is_find) *is_find = g_ftHandles[i].is_find;
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_ftLock);
    return found;
}

static long long ft_handle_size(HANDLE h)
{
    LARGE_INTEGER size;
    if (h == NULL || h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size)) return -1LL;
    return size.QuadPart;
}

static long long ft_find_size(const WIN32_FIND_DATAW* d)
{
    return d ? (((long long)d->nFileSizeHigh << 32) | d->nFileSizeLow) : -1LL;
}

static long long ft_find_size_a(const WIN32_FIND_DATAA* d)
{
    return d ? (((long long)d->nFileSizeHigh << 32) | d->nFileSizeLow) : -1LL;
}

// Widen an A-variant path or have name; 0 when the system code page cannot express it.
static int ft_wide(const char* a, wchar_t* w)
{
    w[0] = 0;
    if (!a || !*a) return 0;
    return MultiByteToWideChar(CP_ACP, 0, a, -1, w, MAX_PATH) > 0;
}

// Full path of a relative name; empty when the name is already absolute (Q1).
static void ft_full_path(const wchar_t* path, wchar_t* full)
{
    DWORD n;
    full[0] = 0;
    if (!path || !*path || !ft_relative(path)) return;
    n = GetFullPathNameW(path, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) full[0] = 0;   // too long or failed: keep it empty
}

static HANDLE WINAPI hook_CreateFileA(LPCSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    wchar_t wide[MAX_PATH];
    HANDLE h = g_origCreateFileA(path, access, share, sa, disp, flags, tmpl);
    DWORD err = GetLastError();
    if (cfg_file_trace && ft_wide(path, wide) && ft_match(wide)) {
        wchar_t full[MAX_PATH];
        ft_full_path(wide, full);
        ft_log("CreateFileA \"%s\" full=\"%ls\" access=%08lX share=%08lX disp=%lu flags=%08lX -> %s handle=%08X size=%lld (err %lu)",
               path, full, access, share, disp, flags, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok",
               (unsigned)(uintptr_t)h, ft_handle_size(h), err);
        if (h != INVALID_HANDLE_VALUE) ft_note_handle(h, full[0] ? full : wide, 0);
    }
    SetLastError(err);
    return h;
}

// Absolute form of a path the script redirect might own: CookedPC paths and the scripts.dzip
// of user content packages (Documents\Witcher 2\UserContent), and the game opens its data with
// absolute paths (docs/fixes/dll-script-patches.md, Q1).
static int script_full_path(const wchar_t* path, wchar_t* full)
{
    DWORD n;

    full[0] = 0;
    if (!path || !*path) return 0;
    if (path[0] == L'\\' || (path[0] && path[1] == L':')) {
        if (!wcsistr(path, L"cookedpc") && !wcsistr(path, L"\\usercontent\\")) return 0;
        wcscpy_s(full, MAX_PATH, path);
        return 1;
    }
    n = GetFullPathNameW(path, MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) {
        full[0] = 0;
        return 0;
    }
    return 1;
}

// Load trace (below): handle kinds and open times, counted from hook_CreateFileW.
static DWORD g_ltTls = TLS_OUT_OF_INDEXES;
static DWORD g_ltMainTid;
static void lt_note_open(const wchar_t* path, HANDLE h, LONGLONG us);

static HANDLE WINAPI hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    wchar_t scriptFull[MAX_PATH], scriptCache[MAX_PATH];
    const wchar_t* use = path;
    HANDLE h;
    DWORD err;
    LONGLONG t0 = cfg_load_trace ? qpc_us() : 0;

    if (g_scriptHooks && script_full_path(path, scriptFull) &&
        sp_hooks_path(scriptFull, scriptCache, MAX_PATH)) {
        use = scriptCache;
        if (InterlockedIncrement(&g_scriptRedirects) <= 8)
            logf_("script patches: CreateFileW \"%ls\" -> cached copy", path);
    }
    h = g_origCreateFileW(use, access, share, sa, disp, flags, tmpl);
    err = GetLastError();
    // The cached archives belong together (a package's scripts.dzip and base_scripts.dzip call
    // into each other), so a passing lock on a new cache file, such as a virus scanner reading
    // it, gets a few retries before this one falls back to the original.
    for (int retry = 0; use != path && h == INVALID_HANDLE_VALUE && retry < 4; retry++) {
        Sleep(50);
        h = g_origCreateFileW(use, access, share, sa, disp, flags, tmpl);
        err = GetLastError();
    }
    if (use != path && h == INVALID_HANDLE_VALUE) {
        logf_("script patches: cached copy for \"%ls\" did not open (err %lu) - using the original",
              path, err);
        h = g_origCreateFileW(path, access, share, sa, disp, flags, tmpl);
        err = GetLastError();
    }
    if (cfg_load_trace) lt_note_open(path, h, qpc_us() - t0);
    if (cfg_save_thumbs && g_build == BUILD_GOG && h != INVALID_HANDLE_VALUE && (access & GENERIC_WRITE))
        thumb_note_open_w(path);
    if (cfg_save_trace && trace_path(path))
        logf_("save-trace: CreateFileW \"%ls\" access=%08lX disp=%lu -> %s (err %lu)",
              path, access, disp, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", err);
    if (cfg_file_trace && ft_match(path)) {
        wchar_t full[MAX_PATH];
        ft_full_path(path, full);
        ft_log("CreateFileW \"%ls\" full=\"%ls\" access=%08lX share=%08lX disp=%lu flags=%08lX -> %s handle=%08X size=%lld (err %lu)",
               path, full, access, share, disp, flags, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok",
               (unsigned)(uintptr_t)h, ft_handle_size(h), err);
        if (h != INVALID_HANDLE_VALUE) ft_note_handle(h, full[0] ? full : path, 0);
    }
    SetLastError(err);
    return h;
}

static BOOL WINAPI hook_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    BOOL ok = g_origGetFileAttributesExW(path, level, info);
    DWORD err = GetLastError();
    if (cfg_file_trace && ft_match(path)) {
        long long size = -1;
        DWORD attr = 0;
        if (ok && info && level == GetFileExInfoStandard) {
            const WIN32_FILE_ATTRIBUTE_DATA* d = (const WIN32_FILE_ATTRIBUTE_DATA*)info;
            size = ((long long)d->nFileSizeHigh << 32) | d->nFileSizeLow;
            attr = d->dwFileAttributes;
        }
        ft_log("GetFileAttributesExW \"%ls\" level=%d -> %d size=%lld attr=%08lX (err %lu)",
               path, (int)level, ok, size, attr, err);
    }
    SetLastError(err);
    return ok;
}

static DWORD WINAPI hook_GetFileAttributesA(LPCSTR path)
{
    wchar_t wide[MAX_PATH];
    DWORD attr = g_origGetFileAttributesA(path);
    DWORD err = GetLastError();
    if (cfg_file_trace && ft_wide(path, wide) && ft_match(wide))
        ft_log("GetFileAttributesA \"%s\" -> %08lX (err %lu)", path, attr, err);
    SetLastError(err);
    return attr;
}

static DWORD WINAPI hook_GetFileAttributesW(LPCWSTR path)
{
    DWORD attr = g_origGetFileAttributesW(path);
    DWORD err = GetLastError();
    if (cfg_file_trace && ft_match(path))
        ft_log("GetFileAttributesW \"%ls\" -> %08lX (err %lu)", path, attr, err);
    SetLastError(err);
    return attr;
}

static HANDLE WINAPI hook_FindFirstFileA(LPCSTR pattern, LPWIN32_FIND_DATAA data)
{
    wchar_t wide[MAX_PATH];
    HANDLE h = g_origFindFirstFileA(pattern, data);
    DWORD err = GetLastError();
    if (cfg_file_trace && ft_wide(pattern, wide) && ft_match(wide)) {
        ft_log("FindFirstFileA \"%s\" -> %s handle=%08X name=\"%s\" size=%lld (err %lu)",
               pattern, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", (unsigned)(uintptr_t)h,
               (h != INVALID_HANDLE_VALUE && data) ? data->cFileName : "",
               (h != INVALID_HANDLE_VALUE) ? ft_find_size_a(data) : -1LL, err);
        if (h != INVALID_HANDLE_VALUE) ft_note_handle(h, wide, 1);
    }
    SetLastError(err);
    return h;
}

static HANDLE WINAPI hook_FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data)
{
    HANDLE h;
    DWORD err;

    // Before the game's CookedPC\*.dzip listing: restore old on-disk patches, build the overlay.
    if (g_scriptHooks) sp_hooks_prepare(pattern);
    h = g_origFindFirstFileW(pattern, data);
    err = GetLastError();
    if (g_scriptHooks && h != INVALID_HANDLE_VALUE && data) sp_hooks_find(pattern, data);
    if (cfg_file_trace && ft_match(pattern)) {
        ft_log("FindFirstFileW \"%ls\" -> %s handle=%08X name=\"%ls\" size=%lld (err %lu)",
               pattern, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", (unsigned)(uintptr_t)h,
               (h != INVALID_HANDLE_VALUE && data) ? data->cFileName : L"",
               (h != INVALID_HANDLE_VALUE) ? ft_find_size(data) : -1LL, err);
        if (h != INVALID_HANDLE_VALUE) ft_note_handle(h, pattern, 1);
    }
    SetLastError(err);
    return h;
}

static HANDLE WINAPI hook_FindFirstFileExW(LPCWSTR pattern, FINDEX_INFO_LEVELS level, LPVOID data,
                                           FINDEX_SEARCH_OPS op, LPVOID filter, DWORD flags)
{
    HANDLE h = g_origFindFirstFileExW(pattern, level, data, op, filter, flags);
    DWORD err = GetLastError();
    if (cfg_file_trace && ft_match(pattern)) {
        const WIN32_FIND_DATAW* d = NULL;
        if (h != INVALID_HANDLE_VALUE && data &&
            (level == FindExInfoStandard || level == FindExInfoBasic))
            d = (const WIN32_FIND_DATAW*)data;
        ft_log("FindFirstFileExW \"%ls\" level=%d search=%d flags=%08lX -> %s handle=%08X name=\"%ls\" size=%lld (err %lu)",
               pattern, (int)level, (int)op, flags, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok",
               (unsigned)(uintptr_t)h, d ? d->cFileName : L"", ft_find_size(d), err);
        if (h != INVALID_HANDLE_VALUE) ft_note_handle(h, pattern, 1);
    }
    SetLastError(err);
    return h;
}

static BOOL WINAPI hook_FindNextFileA(HANDLE find, LPWIN32_FIND_DATAA data)
{
    BOOL ok = g_origFindNextFileA(find, data);
    DWORD err = GetLastError();
    if (cfg_file_trace && ok && data) {
        wchar_t name[MAX_PATH], pattern[MAX_PATH];
        if (ft_wide(data->cFileName, name) && ft_match_entry(name)) {
            int known = ft_lookup(find, pattern, NULL);
            ft_log("FindNextFileA handle=%08X pattern=\"%ls\" name=\"%s\" size=%lld -> %d (err %lu)",
                   (unsigned)(uintptr_t)find, known ? pattern : L"", data->cFileName,
                   ft_find_size_a(data), ok, err);
        }
    }
    SetLastError(err);
    return ok;
}

static BOOL WINAPI hook_FindNextFileW(HANDLE find, LPWIN32_FIND_DATAW data)
{
    BOOL ok = g_origFindNextFileW(find, data);
    DWORD err = GetLastError();
    if (g_scriptHooks && ok && data) sp_hooks_find(NULL, data);
    if (cfg_file_trace && ok && data && ft_match_entry(data->cFileName)) {
        wchar_t pattern[MAX_PATH];
        int known = ft_lookup(find, pattern, NULL);
        ft_log("FindNextFileW handle=%08X pattern=\"%ls\" name=\"%ls\" size=%lld -> %d (err %lu)",
               (unsigned)(uintptr_t)find, known ? pattern : L"", data->cFileName,
               ft_find_size(data), ok, err);
    }
    SetLastError(err);
    return ok;
}

static DWORD WINAPI hook_GetFileSize(HANDLE h, LPDWORD high)
{
    DWORD size = g_origGetFileSize(h, high);
    DWORD err = GetLastError();
    if (cfg_file_trace) {
        wchar_t what[MAX_PATH];
        if (ft_lookup(h, what, NULL) && ft_match(what))
            ft_log("GetFileSize handle=%08X \"%ls\" -> %lu high=%lu (err %lu)",
                   (unsigned)(uintptr_t)h, what, size,
                   (size != INVALID_FILE_SIZE && high) ? *high : 0, err);
    }
    SetLastError(err);
    return size;
}

static BOOL WINAPI hook_GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    BOOL ok = g_origGetFileSizeEx(h, size);
    DWORD err = GetLastError();
    if (cfg_file_trace) {
        wchar_t what[MAX_PATH];
        if (ft_lookup(h, what, NULL) && ft_match(what))
            ft_log("GetFileSizeEx handle=%08X \"%ls\" -> %d size=%lld (err %lu)",
                   (unsigned)(uintptr_t)h, what, ok, (ok && size) ? size->QuadPart : -1LL, err);
    }
    SetLastError(err);
    return ok;
}

static BOOL WINAPI hook_GetFileInformationByHandle(HANDLE h, LPBY_HANDLE_FILE_INFORMATION info)
{
    BOOL ok = g_origGetFileInformationByHandle(h, info);
    DWORD err = GetLastError();
    if (cfg_file_trace) {
        wchar_t what[MAX_PATH];
        if (ft_lookup(h, what, NULL) && ft_match(what)) {
            long long size = (ok && info) ? (((long long)info->nFileSizeHigh << 32) | info->nFileSizeLow) : -1LL;
            ft_log("GetFileInformationByHandle handle=%08X \"%ls\" -> %d size=%lld (err %lu)",
                   (unsigned)(uintptr_t)h, what, ok, size, err);
        }
    }
    SetLastError(err);
    return ok;
}

static HANDLE WINAPI hook_CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                                             DWORD max_high, DWORD max_low, LPCWSTR name)
{
    HANDLE map = g_origCreateFileMappingW(file, sa, protect, max_high, max_low, name);
    DWORD err = GetLastError();
    if (cfg_file_trace) {
        wchar_t what[MAX_PATH];
        int known = ft_lookup(file, what, NULL);
        if ((known && ft_match(what)) || ft_match(name)) {
            long long maxsize = ((long long)max_high << 32) | max_low;
            ft_log("CreateFileMappingW handle=%08X \"%ls\" protect=%08lX maxsize=%lld name=\"%ls\" -> %s map=%08X (err %lu)",
                   (unsigned)(uintptr_t)file, known ? what : L"", protect, maxsize,
                   name ? name : L"", map ? "ok" : "FAILED", (unsigned)(uintptr_t)map, err);
        }
    }
    SetLastError(err);
    return map;
}

// Install all trace slots; returns how many hold our hooks afterwards.
static int ft_install(void)
{
    int n = 0;
    n += patch_iat("file-trace", IMP_CREATEFILEW, (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
    n += patch_iat("file-trace", IMP_CREATEFILEA, (void*)hook_CreateFileA, (void**)&g_origCreateFileA);
    n += patch_iat("file-trace", IMP_GETFILEATTREXW, (void*)hook_GetFileAttributesExW, (void**)&g_origGetFileAttributesExW);
    n += patch_iat("file-trace", IMP_GETFILEATTRW, (void*)hook_GetFileAttributesW, (void**)&g_origGetFileAttributesW);
    n += patch_iat("file-trace", IMP_GETFILEATTRA, (void*)hook_GetFileAttributesA, (void**)&g_origGetFileAttributesA);
    n += patch_iat("file-trace", IMP_GETFILEINFO, (void*)hook_GetFileInformationByHandle, (void**)&g_origGetFileInformationByHandle);
    n += patch_iat("file-trace", IMP_FINDFIRSTFILEW, (void*)hook_FindFirstFileW, (void**)&g_origFindFirstFileW);
    n += patch_iat("file-trace", IMP_FINDFIRSTFILEA, (void*)hook_FindFirstFileA, (void**)&g_origFindFirstFileA);
    n += patch_iat("file-trace", IMP_FINDFIRSTFILEEXW, (void*)hook_FindFirstFileExW, (void**)&g_origFindFirstFileExW);
    n += patch_iat("file-trace", IMP_FINDNEXTFILEW, (void*)hook_FindNextFileW, (void**)&g_origFindNextFileW);
    n += patch_iat("file-trace", IMP_FINDNEXTFILEA, (void*)hook_FindNextFileA, (void**)&g_origFindNextFileA);
    n += patch_iat("file-trace", IMP_GETFILESIZE, (void*)hook_GetFileSize, (void**)&g_origGetFileSize);
    n += patch_iat("file-trace", IMP_GETFILESIZEEX, (void*)hook_GetFileSizeEx, (void**)&g_origGetFileSizeEx);
    n += patch_iat("file-trace", IMP_CREATEFILEMAPPINGW, (void*)hook_CreateFileMappingW, (void**)&g_origCreateFileMappingW);
    return n;
}

static int ft_hooked(void)
{
    int n = 0;
    n += imp_value(IMP_CREATEFILEW) == (void*)hook_CreateFileW;
    n += imp_value(IMP_CREATEFILEA) == (void*)hook_CreateFileA;
    n += imp_value(IMP_GETFILEATTREXW) == (void*)hook_GetFileAttributesExW;
    n += imp_value(IMP_GETFILEATTRW) == (void*)hook_GetFileAttributesW;
    n += imp_value(IMP_GETFILEATTRA) == (void*)hook_GetFileAttributesA;
    n += imp_value(IMP_GETFILEINFO) == (void*)hook_GetFileInformationByHandle;
    n += imp_value(IMP_FINDFIRSTFILEW) == (void*)hook_FindFirstFileW;
    n += imp_value(IMP_FINDFIRSTFILEA) == (void*)hook_FindFirstFileA;
    n += imp_value(IMP_FINDFIRSTFILEEXW) == (void*)hook_FindFirstFileExW;
    n += imp_value(IMP_FINDNEXTFILEW) == (void*)hook_FindNextFileW;
    n += imp_value(IMP_FINDNEXTFILEA) == (void*)hook_FindNextFileA;
    n += imp_value(IMP_GETFILESIZE) == (void*)hook_GetFileSize;
    n += imp_value(IMP_GETFILESIZEEX) == (void*)hook_GetFileSizeEx;
    n += imp_value(IMP_CREATEFILEMAPPINGW) == (void*)hook_CreateFileMappingW;
    return n;
}

// Started from DllMain while file_trace=1. The SteamStub fills the IAT after DllMain
// returned, so poll for the real kernel32 export instead of the worker's 1.5 s delay.
static DWORD WINAPI filetrace_thread(LPVOID param)
{
    DWORD t0 = GetTickCount(), hooked_at;
    HMODULE k32 = NULL;
    void* expect = NULL;
    int n, last_n, tries;
    wchar_t cwd[MAX_PATH];
    (void)param;

    logf_("file-trace: thread started, CreateFileW slot holds %08X",
          (unsigned)(uintptr_t)imp_value(IMP_CREATEFILEW));
    for (;;) {
        if (!k32) k32 = GetModuleHandleA("kernel32.dll");
        if (k32 && !expect) expect = (void*)GetProcAddress(k32, "CreateFileW");
        if (expect && imp_value(IMP_CREATEFILEW) == expect) break;
        if (!g_running || GetTickCount() - t0 > 120000) {
            logf_("file-trace: CreateFileW slot never held the kernel32 export - not hooked");
            return 0;
        }
        Sleep(1);
    }

    // Import slots must not outlive this DLL if a caller releases its handle.
    HMODULE pinned;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)hook_CreateFileW, &pinned)) {
        logf_("file-trace: could not pin module - file trace off");
        cfg_file_trace = 0;
        return 0;
    }
    n = ft_install();
    hooked_at = GetTickCount();
    cwd[0] = 0;
    GetCurrentDirectoryW(MAX_PATH, cwd);
    logf_("file-trace: hooked after %lu ms (%d/%d slots), current directory \"%ls\"",
          hooked_at - t0, n, FT_SLOTS, cwd);

    // If anything rewrites the slots later, re-hook so no call is lost (Q2).
    for (last_n = n, tries = 0; g_running && tries < 20 && GetTickCount() - t0 < 60000; ) {
        Sleep(GetTickCount() - t0 < 10000 ? 100 : 1000);
        if (!g_running) break;
        n = ft_hooked();
        if (n == last_n) continue;
        tries++;
        logf_("file-trace: %d/%d slots still hooked after %lu ms - re-hooking",
              n, FT_SLOTS, GetTickCount() - t0);
        last_n = ft_install();
    }
    n = ft_hooked();
    if (n < FT_SLOTS)
        logf_("file-trace: %d/%d slots hooked - the rest are not redirectable", n, FT_SLOTS);
    return 0;
}

// Started from DllMain when a [scriptpatches] feature is on: the Task 1 pattern, because the
// redirect must be in place before the game's first CookedPC access (~235 ms). Only the three
// slots the redirect needs are patched; save_trace and file_trace patch the same ones through
// patch_iat, which treats a slot that already holds our hook as done.
static DWORD WINAPI scripthooks_thread(LPVOID param)
{
    DWORD t0 = GetTickCount();
    HMODULE k32 = NULL, pinned;
    void* expect = NULL;
    int n;
    (void)param;

    for (;;) {
        if (!k32) k32 = GetModuleHandleA("kernel32.dll");
        if (k32 && !expect) expect = (void*)GetProcAddress(k32, "CreateFileW");
        if (expect && imp_value(IMP_CREATEFILEW) == expect) break;
        if (!g_running || GetTickCount() - t0 > 120000) {
            logf_("script patches: CreateFileW slot never held the kernel32 export - off");
            g_scriptHooks = 0;
            return 0;
        }
        Sleep(1);
    }
    // Import slots must not outlive this DLL if a caller releases its handle.
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)hook_CreateFileW, &pinned)) {
        logf_("script patches: could not pin module - off");
        g_scriptHooks = 0;
        return 0;
    }
    n = 0;
    n += patch_iat("script-patcher", IMP_CREATEFILEW, (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
    n += patch_iat("script-patcher", IMP_FINDFIRSTFILEW, (void*)hook_FindFirstFileW, (void**)&g_origFindFirstFileW);
    n += patch_iat("script-patcher", IMP_FINDNEXTFILEW, (void*)hook_FindNextFileW, (void**)&g_origFindNextFileW);
    logf_("script patches: hooks installed after %lu ms (%d/3 slots)", GetTickCount() - t0, n);
    if (imp_value(IMP_CREATEFILEW) != (void*)hook_CreateFileW) {
        logf_("script patches: CreateFileW not hooked - the patched files cannot be served, off");
        g_scriptHooks = 0;
    }
    return 0;
}

// ---- load trace ---------------------------------------------------------
// [diagnostics] load_trace=1: where the time goes while a loading screen is up (load from
// the menu, area transitions). Passive: the hooks call the real function first and only
// count; nothing is changed. Results in docs/fixes/load-times.md.
//
// Window: the render thread object (DAT_0160D0A4, set by its loop FUN_009d1e96) holds the
// loading-screen movie at +0x268 and the screen object at +0x26C; the render command
// 009DC3FC -> FUN_009d1b9f sets +0x268 when a loading screen starts and clears it at the
// end. While +0x268 is set and no command is queued, the render thread spins on
// FUN_009d1d61, which draws only when 1/30 s has passed (QPC + PeekMessage, no wait).
// The trace polls those two fields every 5 ms; a window is +0x268 going non-zero to zero.
//
// Counters per thread (one slot each, written only by that thread):
//   ReadFile (00CAD27C)      count, bytes, time, split by the file the handle was opened as
//                            (.dzip / other CookedPC / anything else); the dzip library
//                            reads its 64 KiB blocks with the CRT's _lseeki64 + _read
//                            (0068E8C1 / 0068E8D4), which end in this ReadFile
//   CreateFileW (00CAD320)   opens and their time (shared hook)
//   block unpack             FUN_0068f3b0, the dzip/save block decompressor (cdecl in, in_len,
//                            out, 0x10000); its only call 0068E8E7 is redirected
//   Sleep (00CAD304)         count, Sleep(0)/Sleep(1)/longer, time actually slept
//   WaitForSingleObject(Ex) (00CAD2B0 / 00CAD07C) count and time
//   IDirect3DDevice9::Present (vtable slot 17 of the device at 01626788) count and time
// plus each thread's CPU time (GetThreadTimes) at both ends of the window. A timeline line
// every 250 ms sums the counters over all threads.
//
// Interiors have no loading screen: a streaming door (constructor FUN_0095de8e) starts a task
// in FUN_0095dcc4, keeps it at door+0x228 and ticks it from its OnTimer method FUN_0095db00
// (door vtable slot 00EF01BC, thiscall, RET 8) until the task's Tick (slot 1) returns done;
// the interior task (vtable 00EF0438, Tick 00502dc0) waits for the layer streaming and fades
// to black after 2.5 s (+0x14 = 1). The slot is wrapped to time that span as a "door" window.
// Outside any window, a 250 ms interval that read or unpacked more than 4 MB is logged as a
// streaming burst (the open world streams without a marker).

#define VA_RENDER_THREAD    0x0160D0A4u   // render thread object while its loop runs
#define OFF_RT_LOAD_MOVIE   0x268         // non-zero while a loading screen is up
#define OFF_RT_LOAD_SCREEN  0x26C         // the screen object; byte +0x10 = still shown
#define VA_UNPACK_CALL      0x0068E8E7u   // call FUN_0068f3b0 in FUN_0068e890
#define VA_UNPACK           0x0068F3B0u
#define LT_THREADS          128
#define LT_HANDLES          4096          // open-addressing table: handle -> file kind
#define LT_TICK_MS          250
#define LT_MAX_WINDOWS      40
#define LT_CPU_MAX          256
#define VA_DOOR_TIMER_SLOT  0x00EF01BCu   // streaming door OnTimer
#define VA_DOOR_TIMER       0x0095DB00u
#define OFF_DOOR_TASK       0x228
#define VA_DOOR_INTERIOR_VT 0x00EF0438u   // the interior task: +0x14 state (1 = faded out)
#define LT_BURST_BYTES      (4 << 20)

enum { LT_DZIP, LT_LOOSE, LT_OTHER, LT_UNKNOWN, LT_KINDS };
static const char* const k_ltKind[LT_KINDS] = { "dzip", "loose", "other", "unknown" };

typedef struct {
    volatile LONG tid;
    LONG     reads[LT_KINDS];
    LONGLONG readBytes[LT_KINDS], readUs[LT_KINDS];
    LONG     readsAsync;
    LONG     opens;
    LONGLONG openUs;
    LONG     unpacks;
    LONGLONG unpackIn, unpackOut, unpackUs;
    LONG     sleeps, sleep0, sleep1;
    LONGLONG sleepUs;
    LONG     waits, waitsZero;
    LONGLONG waitUs;
    LONG     presents;
    LONGLONG presentUs, presentMaxUs;
} lt_thread;

typedef struct { DWORD tid; ULONGLONG cpu; uintptr_t start; } lt_cpu;

typedef BOOL  (WINAPI *ReadFile_t)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef VOID  (WINAPI *Sleep_t)(DWORD);
typedef DWORD (WINAPI *WaitSingle_t)(HANDLE, DWORD);
typedef DWORD (WINAPI *WaitSingleEx_t)(HANDLE, DWORD, BOOL);
typedef int   (__cdecl *Unpack_t)(const void*, int, void*, int);
typedef HRESULT (WINAPI *Present_t)(void*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef LONG  (WINAPI *NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
typedef LONG  (WINAPI *NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);
typedef void  (__fastcall *DoorTimer_t)(void* door, void* edx, void* name, float dt);

static uintptr_t      g_unpack;           // this build's FUN_0068f3b0, set before its call is hooked
static ReadFile_t     g_origReadFile;
static Sleep_t        g_origSleep;
static WaitSingle_t   g_origWaitSingle;
static WaitSingleEx_t g_origWaitSingleEx;
static Present_t      g_origPresent;
static DoorTimer_t    g_origDoorTimer;
static void* volatile g_ltDoorTask;     // the task being timed, set by the game thread
static void*          g_ltDoorVt;
static volatile DWORD g_ltDoorStart, g_ltDoorEnd;
static volatile LONG  g_ltDoorTicks, g_ltDoorFaded, g_ltDoorOverlap;
static lt_thread      g_lt[LT_THREADS];
static lt_thread      g_ltStart[LT_THREADS];
static struct { volatile LONG h; LONG kind; } g_ltHandles[LT_HANDLES];

static lt_thread* lt_self(void)
{
    lt_thread* t;
    DWORD tid;
    int i;
    if (g_ltTls == TLS_OUT_OF_INDEXES) return NULL;
    t = (lt_thread*)TlsGetValue(g_ltTls);
    if (t) return t;
    tid = GetCurrentThreadId();
    for (i = 0; i < LT_THREADS; i++) {
        if (g_lt[i].tid == (LONG)tid ||
            InterlockedCompareExchange(&g_lt[i].tid, (LONG)tid, 0) == 0) {
            TlsSetValue(g_ltTls, &g_lt[i]);
            return &g_lt[i];
        }
    }
    return NULL;
}

static unsigned lt_hslot(HANDLE h) { return (((unsigned)(uintptr_t)h >> 2) * 2654435761u) >> 20; }

// Called from hook_CreateFileW: remember what kind of file a handle is, count the open.
static void lt_note_open(const wchar_t* path, HANDLE h, LONGLONG us)
{
    DWORD err = GetLastError();
    lt_thread* t = lt_self();
    if (t) { t->opens++; t->openUs += us; }
    if (h != INVALID_HANDLE_VALUE && h && path) {
        size_t n = wcslen(path);
        LONG kind = (n >= 5 && !_wcsicmp(path + n - 5, L".dzip")) ? LT_DZIP
                  : wcsistr(path, L"cookedpc") ? LT_LOOSE : LT_OTHER;
        unsigned i = lt_hslot(h), k;
        for (k = 0; k < 64; k++, i = (i + 1) % LT_HANDLES) {   // the handle's old slot or a free one
            LONG have = g_ltHandles[i].h;
            if (have == (LONG)(uintptr_t)h || have == 0) {
                g_ltHandles[i].kind = kind;
                InterlockedExchange(&g_ltHandles[i].h, (LONG)(uintptr_t)h);
                break;
            }
        }
    }
    SetLastError(err);
}

static int lt_kind(HANDLE h)
{
    unsigned i = lt_hslot(h), k;
    for (k = 0; k < 64; k++, i = (i + 1) % LT_HANDLES) {
        LONG have = g_ltHandles[i].h;
        if (have == (LONG)(uintptr_t)h) return g_ltHandles[i].kind;
        if (have == 0) break;
    }
    return LT_UNKNOWN;
}

static BOOL WINAPI hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
    LONGLONG t0 = qpc_us();
    BOOL ok = g_origReadFile(h, buf, n, got, ov);
    LONGLONG us = qpc_us() - t0;
    DWORD err = GetLastError();
    lt_thread* t = lt_self();
    if (t) {
        int k = lt_kind(h);
        t->reads[k]++;
        t->readUs[k] += us;
        t->readBytes[k] += (ok && got) ? *got : (ov ? n : 0);
        if (ov) t->readsAsync++;
    }
    SetLastError(err);
    return ok;
}

static VOID WINAPI hook_Sleep(DWORD ms)
{
    LONGLONG t0 = qpc_us();
    DWORD err;
    lt_thread* t;
    g_origSleep(ms);
    err = GetLastError();
    t = lt_self();
    if (t) {
        t->sleeps++;
        if (ms == 0) t->sleep0++; else if (ms == 1) t->sleep1++;
        t->sleepUs += qpc_us() - t0;
    }
    SetLastError(err);
}

static void lt_count_wait(DWORD ms, LONGLONG us)
{
    DWORD err = GetLastError();
    lt_thread* t = lt_self();
    if (t) {
        t->waits++;
        if (ms == 0) t->waitsZero++;
        t->waitUs += us;
    }
    SetLastError(err);
}

static DWORD WINAPI hook_WaitSingle(HANDLE h, DWORD ms)
{
    LONGLONG t0 = qpc_us();
    DWORD r = g_origWaitSingle(h, ms);
    lt_count_wait(ms, qpc_us() - t0);
    return r;
}

static DWORD WINAPI hook_WaitSingleEx(HANDLE h, DWORD ms, BOOL alertable)
{
    LONGLONG t0 = qpc_us();
    DWORD r = g_origWaitSingleEx(h, ms, alertable);
    lt_count_wait(ms, qpc_us() - t0);
    return r;
}

static int __cdecl hook_Unpack(const void* in, int in_len, void* out, int out_len)
{
    LONGLONG t0 = qpc_us();
    int r = ((Unpack_t)g_unpack)(in, in_len, out, out_len);
    LONGLONG us = qpc_us() - t0;
    DWORD err = GetLastError();
    lt_thread* t = lt_self();
    if (t) {
        t->unpacks++;
        t->unpackIn += in_len;
        if (r > 0) t->unpackOut += r;
        t->unpackUs += us;
    }
    SetLastError(err);
    return r;
}

// IDirect3DDevice9::Present: four parameters plus this.
static HRESULT WINAPI hook_Present(void* self, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty)
{
    LONGLONG t0 = qpc_us();
    HRESULT hr = g_origPresent(self, src, dst, wnd, dirty);
    LONGLONG us = qpc_us() - t0;
    DWORD err = GetLastError();
    lt_thread* t = lt_self();
    if (t) {
        t->presents++;
        t->presentUs += us;
        if (us > t->presentMaxUs) t->presentMaxUs = us;
    }
    SetLastError(err);
    return hr;
}

// ---- partition trace ----------------------------------------------------
// Part of load_trace: what each world-partition switch swaps, research for interior
// preloading (docs/fixes/interior-preload.md). Interiors are partitions: lists of layer
// groups, one current at a time. FUN_00a91bf9 (thiscall world, (String* name, mode,
// String* loadingScreen), RET 0xC; mode 0 = StreamWorldPartition, 1 = LoadWorldPartition,
// 4 = house door) diffs the current partition's layers against the target's and builds a
// job (FUN_00a90fc4): +0x2C target name, +0x0C/+0x10 layers to unload, +0x1C/+0x20 load
// buckets by layer priority (bucket +0x08/+0x0C layers). Mode 0 keeps the job at world+0x30C
// and streams it over the next frames; every other mode runs it at once through FUN_00a91952
// (stdcall job, RET 4; called at 00A91DAD). The partition requests reach FUN_00a91bf9 from
// 00AAA1FF (the game's request queue) and 00AAB089. A layer info's name is at +0x78. The
// player is CWitcherGame+0x250 (m_player); an entity's world position is at +0xA0.
// Passive unless keep_outdoors or preload_interiors is on: the hooks log and call the real
// function.
//
// keep_outdoors: a house door (mode 4) unloads the outdoor layers around it (bushes, rocks,
// animals, fog, collision; in Flotsam also far buildings, ships and the town's sound) and
// loads the house's rooms, which sit inside its own shell; leaving reloads them (runs 5, 11).
// For a door within one partition folder (a building and its street; an area change such as
// the Flotsam gate is between folders) the job's unload list is cut to the layers a partition
// swaps for its own (ko_class: the area's `fake` rooms, sound, occluders, the indoor
// environment, low-poly stand-ins), so the rest of the outdoors stays loaded. The lists are
// pointed at the DLL's own arrays for the run and put back after it. The loader skips layers
// already loaded (FUN_00a91600, layer info +0x38) and the unloader skips unloaded ones
// (FUN_00a5aa35).
//
// keep_interiors (needs keep_outdoors): leaving a house also unloads its rooms and reloads
// the area's `fake` layer (one per area, fake_interiors\<area>\fake.w2l), ~45 ms and a dark
// box popping up around Geralt. With keep_interiors=1 the rooms stay and `fake` is not loaded
// back, so re-entering loads nothing; other houses' open doors show no fake room until the
// next area change.
//
// preload_interiors: an area change (mode 1, or a door between folders) also loads every
// building interior of the world (layer infos under `\buildings\<name>\interior\`, found in
// the world's layer tree, appended to the job's last load bucket) and drops `fake`; no job
// unloads them again. Open doors then show the real rooms and no house door loads its rooms.
//
// skip_door_gc: every job runs the same stages whatever is in it (FUN_00a91952: flush,
// unload, "GC...", load, flush the async loader, attach entities, apply the partition), and
// the GC stage (00A91AD7: call FUN_00abd770, stdcall, a full object GC; it locks and unlocks
// its own counter) cost a house door 25-40 ms with nothing loaded or unloaded (run 9). For a
// job keep_outdoors handled the call is skipped: at most the swapped layers' objects wait
// for the next GC (area changes, save loads).
#define VA_PART_SWITCH      0x00A91BF9u
#define VA_PART_SWITCH_A    0x00AAA1FFu   // call from the request queue
#define VA_PART_SWITCH_B    0x00AAB089u
#define VA_PART_RUN         0x00A91952u   // stdcall (job), RET 4; job in EDI on GOG
#define VA_PART_RUN_CALL    0x00A91DADu   // the synchronous run in FUN_00a91bf9
#define OFF_GAME_PLAYER     0x250
#define OFF_NODE_POS        0xA0
#define PT_MAX_NAMES        80
#define VA_PART_GC_CALL     0x00A91AD7u   // call FUN_00abd770 (GC) in FUN_00a91952
#define VA_GC               0x00ABD770u

static int g_ptMode = -1;   // mode of the switch in progress (game thread)
static int g_koDeferOcc;     // occluders wait for Geralt to clear the door (ko_frame)
static wchar_t g_ptFrom[160], g_ptTo[160];   // its partitions
// instant_doors: door switches (from, to) that keep_interiors/preload_interiors have made
// load nothing; forgotten on an area change unless every interior stays loaded.
// With preload_interiors every door within one folder is taken as ready (no fade) until one
// is measured slow (KO_SLOW_MS for the job itself): those pairs keep door_fade.
#define KO_PAIRS 128
#define KO_SLOW_MS 50
static DWORD g_koPairs[KO_PAIRS];
static int   g_koPairCount;
static DWORD g_koSlow[KO_PAIRS];
static int   g_koSlowCount;

static DWORD ko_pair(const wchar_t* from, const wchar_t* to)
{
    DWORD h = 2166136261u;
    for (; *from; from++) h = (h ^ (DWORD)towlower(*from)) * 16777619u;
    h = (h ^ 0x7C) * 16777619u;
    for (; *to; to++) h = (h ^ (DWORD)towlower(*to)) * 16777619u;
    return h ? h : 1;
}

static int ko_pair_in(const DWORD* set, int n, const wchar_t* from, const wchar_t* to)
{
    DWORD h = ko_pair(from, to);
    int i;
    for (i = 0; i < n; i++) if (set[i] == h) return 1;
    return 0;
}

static void ko_pair_put(DWORD* set, int* n, const wchar_t* from, const wchar_t* to)
{
    if (!from[0] || !to[0] || ko_pair_in(set, *n, from, to)) return;
    set[*n < KO_PAIRS ? (*n)++ : (KO_PAIRS - 1)] = ko_pair(from, to);
}

static void ko_pair_del(DWORD* set, int* n, const wchar_t* from, const wchar_t* to)
{
    DWORD h = ko_pair(from, to);
    int i;
    for (i = 0; i < *n; i++)
        if (set[i] == h) { set[i] = set[--*n]; return; }
}

static int ko_pair_known(const wchar_t* from, const wchar_t* to) { return ko_pair_in(g_koPairs, g_koPairCount, from, to); }
static void ko_pair_add(const wchar_t* from, const wchar_t* to) { ko_pair_put(g_koPairs, &g_koPairCount, from, to); }

// Length of a partition name's folder part (up to the last backslash).
static int part_folder(const wchar_t* name)
{
    const wchar_t* slash = wcsrchr(name, L'\\');
    return slash ? (int)(slash - name) : 0;
}

typedef unsigned char (__fastcall *PartSwitch_t)(void* world, void* edx, void* name, int mode, void* screen);
typedef void (__stdcall *PartRun_t)(void* job);
typedef void (__stdcall *Gc_t)(void* arg);

static int      g_koSkipGc;      // set by hook_PartRun for the job it runs (game thread)
static int      g_ptGcSkipped;
static LONGLONG g_ptGcUs;
// This build's FUN_00abd770, FUN_00a91bf9, FUN_00a91952 and FUN_00aa7e18: set by pt_install
// before any call is redirected (the hooks run on the game thread, often).
static uintptr_t g_gc, g_partSwitch, g_partRun, g_partRequest;

static void __stdcall hook_PartGc(void* arg)
{
    LONGLONG t0;
    if (g_koSkipGc) { g_ptGcSkipped = 1; return; }
    t0 = qpc_us();
    ((Gc_t)g_gc)(arg);
    g_ptGcUs = qpc_us() - t0;
}

// A String's text (first field), copied under SEH; "" when unreadable.
static const wchar_t* pt_text(const void* str, wchar_t* buf, int cap)
{
    buf[0] = 0;
    __try {
        const wchar_t* s = str ? *(const wchar_t* const*)str : NULL;
        int i;
        for (i = 0; s && i < cap - 1 && s[i]; i++) buf[i] = s[i];
        buf[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; }
    return buf;
}

static const char* pt_mode(int mode)
{
    static char other[16];
    switch (mode) {
    case 0: return "stream";
    case 1: return "load";
    case 4: return "door";
    }
    sprintf_s(other, sizeof other, "mode %d", mode);
    return other;
}

static void pt_where(const char* what, const void* door)
{
    void* game = read_ptr((void*)ga(VA_GAME));
    void* player = game ? read_ptr((char*)game + OFF_GAME_PLAYER) : NULL;
    float p[3] = { 0, 0, 0 }, d[3] = { 0, 0, 0 };
    int i, havePlayer = player != NULL, haveDoor = door != NULL;
    for (i = 0; i < 3; i++) {
        if (havePlayer) havePlayer = read_f32((const char*)player + OFF_NODE_POS + 4 * i, &p[i]);
        if (haveDoor) haveDoor = read_f32((const char*)door + OFF_NODE_POS + 4 * i, &d[i]);
    }
    if (door)
        logf_("partition: %s - player %s(%.2f, %.2f, %.2f), door (%.2f, %.2f, %.2f)", what,
              havePlayer ? "" : "unknown ", p[0], p[1], p[2], d[0], d[1], d[2]);
    else
        logf_("partition: %s - player %s(%.2f, %.2f, %.2f)", what, havePlayer ? "" : "unknown ", p[0], p[1], p[2]);
}

// Research: the other text fields of a layer info (its path or group), as " [+off text]".
// A field counts when it points at >= 3 printable UTF-16 characters.
static void pt_probe(const void* layer, char* out, size_t cap)
{
    wchar_t text[120];
    size_t used = 0;
    int off, found = 0;
    out[0] = 0;
    if (!layer) return;
    for (off = 0; off < 0x100 && found < 3; off += 4) {
        int i, ok = 1;
        if (off == 0x78) continue;
        pt_text((const char*)layer + off, text, 120);
        for (i = 0; text[i]; i++) if (text[i] < 0x20 || text[i] > 0x7E) { ok = 0; break; }
        if (!ok || i < 3) continue;
        used += (size_t)_snprintf_s(out + used, cap - used, _TRUNCATE, " [+%02X %ls]", off, text);
        if (used >= cap - 1) break;
        found++;
    }
}

// One list of layer infos: count, then a line per layer.
static int pt_layers(const char* what, const void* data, DWORD count, int shown)
{
    wchar_t name[200];
    DWORD i;
    for (i = 0; i < count && shown < PT_MAX_NAMES; i++, shown++) {
        void* layer = read_ptr((const char*)data + 4 * i);
        void* str = layer ? (char*)layer + 0x78 : NULL;
        char extra[400];
        pt_probe(layer, extra, sizeof extra);
        logf_("partition:   %s %ls%s", what, str ? pt_text(str, name, 200) : L"(null)", extra);
    }
    return shown;
}

static void pt_job(const char* why, const void* job)
{
    wchar_t to[160];
    DWORD unload, buckets, load = 0, i;
    const void *unloadData, *bucketData;
    int shown = 0;
    if (!job) return;
    unloadData = read_ptr((const char*)job + 0x0C);
    unload = (DWORD)(uintptr_t)read_ptr((const char*)job + 0x10);
    bucketData = read_ptr((const char*)job + 0x1C);
    buckets = (DWORD)(uintptr_t)read_ptr((const char*)job + 0x20);
    if (unload > 4096 || buckets > 5) {
        logf_("partition: %s job %08X not recognised (unload %lu, buckets %lu)", why,
              (unsigned)(uintptr_t)job, unload, buckets);
        return;
    }
    for (i = 0; i < buckets; i++) {
        void* b = read_ptr((const char*)bucketData + 4 * i);
        if (b) load += (DWORD)(uintptr_t)read_ptr((char*)b + 0x0C);
    }
    logf_("partition: %s job to '%ls': unload %lu layers, load %lu layers in %lu buckets", why,
          pt_text((const char*)job + 0x2C, to, 160), unload, load, buckets);
    shown = pt_layers("unload", unloadData, unload, shown);
    for (i = 0; i < buckets; i++) {
        void* b = read_ptr((const char*)bucketData + 4 * i);
        DWORD n = b ? (DWORD)(uintptr_t)read_ptr((char*)b + 0x0C) : 0;
        if (n > 4096) break;
        if (b) shown = pt_layers("load  ", read_ptr((char*)b + 0x08), n, shown);
    }
    if (shown >= PT_MAX_NAMES) logf_("partition:   (names cut at %d)", PT_MAX_NAMES);
}

// Layer classes for the door rules, from the layer info's path (+0x68) and name (+0x78):
//   FAKE      the area's flat "fake room" boxes behind doors and windows: Act 1's
//             `fake_interiors\<area>\fake.w2l`, Vergen's
//             `dwarf_city\windows_outside\windows_outside.w2l` (the only layers of the game's
//             five worlds that hold `fake_inside*.w2mesh`, docs/fixes/interior-preload.md)
//   SWAP      what each partition swaps for its own and must not be kept: ambient sound
//             (`\sounds\`), occluders (`occ` in the name, or any layer under an
//             `\occluders\` folder: the prologue's `occluders\basement\basement.w2l`; outdoor
//             occluders around a building the camera is inside, or an interior's own), the
//             indoor environment (an `interior` folder or file under `\environments\`: Act 1's
//             `environments\interior_city`, Vergen's `environments\environment_interior\<house>`)
//             and the rest of Vergen's `windows_outside` group (an awning), which the game takes
//             away inside every house
//   LOWPOLY   low-poly stand-ins an interior loads for the view out of its windows
//             (`city_building_lowpoly\ships_lowpoly`); the real objects stay when kept
//   INTERIOR  a building's own rooms (`\buildings\<name>\interior\`; the prologue village's
//             `island\village_b_ints\interior<N>\`)
//   ENV       an area's own environment that is no interior's (`environments\environment_dwarf_mine`,
//             `environments\camp_henselt`): lighting, fog and sky inside an area on the map. A
//             door between two whole areas swaps it as the game does (hook_PartRun); a house door
//             keeps its street's. The cave's stayed loaded after the way out and kept the quarry
//             around the cave mouth dark, with the cave's sky (owner, 2026-11-12:
//             `'AreaEnvironment_dwarf_mine' 105 at 1.00` three seconds after the door out).
// SWAP also takes the stand-ins an area puts around itself for what the game unloads: the
// black plane in a cave mouth (`black_plane*`, `dark_plane*`, `shadow_plane*`, `dark_fog`,
// `assas_cave_black`, the `dark_planes\` folder) and the box that keeps the sun out of a cave
// (`shadow_box`). Kept after the way out, the cave's plane stood between Geralt and the quarry
// until he walked through it, and its box shaded the ground outside (same run).
enum { LC_OTHER, LC_FAKE, LC_SWAP, LC_LOWPOLY, LC_INTERIOR, LC_ENV };

// Does a layer path or a layer group path name an interior's environment: `interior` somewhere
// after an `environments` folder.
static int ko_env_interior(const wchar_t* s)
{
    const wchar_t* e;
    for (e = wcsistr(s, L"environments\\"); e; e = wcsistr(e + 1, L"environments\\"))
        if ((e == s || e[-1] == L'\\') && wcsistr(e + 13, L"interior")) return 1;
    return 0;
}

// Quest markers inside buildings. A quest asks "is an entity with this tag in the world?"
// (CQuestTagsPresenceCondition) to learn where Geralt is: the Vergen dice quest sq204 looks for
// `sq204_wp_inside_building`, a lone waypoint in the layer `sq204_wp_inside` of three houses and
// the inn, and then starts the indoor game at it; sq205 looks for the inn's arm wrestling spot
// (`building_inn_dec\armwrestling`). In the game such a layer is in the world only while Geralt
// is in that building. Preloaded with the house it was always there, so the last dice player
// took the indoor branch in the street and Poker Face: Vergen could not be finished (a player,
// 0.12.1). Of the 178 layers inside the buildings of the five worlds, the ones a presence test
// can meet are these five and Act 1's `sq107_troll_head` (docs/fixes/interior-preload.md). So a
// layer inside a building that is named after a quest (`q` or `sq`, three digits, `_`), or is
// the arm wrestling spot, is not preloaded and not kept: it loads at its door and unloads at
// the door out, as in the game. It holds no room.
static int kq_marker_name(const wchar_t* name)
{
    const wchar_t* p = name;
    int d = 0;
    if (!_wcsicmp(name, L"armwrestling")) return 1;
    if (!_wcsnicmp(p, L"sq", 2)) p += 2;
    else if (*p == L'q' || *p == L'Q') p += 1;
    else return 0;
    while (p[d] >= L'0' && p[d] <= L'9') d++;
    return d >= 3 && p[d] == L'_';
}

static int ko_stand_in(const wchar_t* name, const wchar_t* path)
{
    return wcsistr(name, L"black_plane") || wcsistr(name, L"dark_plane") || wcsistr(name, L"shadow_plane") ||
           wcsistr(name, L"shadow_box") || wcsistr(name, L"dark_fog") || wcsistr(name, L"cave_black") ||
           wcsistr(path, L"\\dark_planes\\");
}

static int ko_class(const void* layer)
{
    wchar_t name[64], path[260];
    if (!layer) return LC_OTHER;
    pt_text((const char*)layer + 0x78, name, 64);
    pt_text((const char*)layer + 0x68, path, 260);
    if (!_wcsicmp(name, L"fake") || !_wcsicmp(name, L"windows_outside")) return LC_FAKE;
    if (wcsistr(path, L"\\sounds\\") || wcsistr(name, L"occ") || wcsistr(path, L"\\occluders\\") ||
        ko_env_interior(path) || wcsistr(path, L"\\windows_outside\\") || ko_stand_in(name, path))
        return LC_SWAP;
    if (wcsistr(path, L"\\environments\\")) return LC_ENV;
    if (wcsistr(path, L"_lowpoly\\")) return LC_LOWPOLY;
    if ((wcsistr(path, L"\\buildings\\") && wcsistr(path, L"\\interior\\")) || wcsistr(path, L"_ints\\"))
        return LC_INTERIOR;
    return LC_OTHER;
}

// A door between two partitions of one folder (a building and its street), not an area change.
static int ko_same_folder(const wchar_t* a, const wchar_t* b)
{
    int na = part_folder(a), nb = part_folder(b);
    return a[0] && b[0] && na == nb && !_wcsnicmp(a, b, na);
}

static int ko_loads_class(const void* job, int cls)
{
    const void* bucketData = read_ptr((const char*)job + 0x1C);
    DWORD buckets = (DWORD)(uintptr_t)read_ptr((const char*)job + 0x20), i, k;
    for (i = 0; i < buckets && i < 5; i++) {
        void* b = read_ptr((const char*)bucketData + 4 * i);
        void** d = b ? (void**)read_ptr((char*)b + 0x08) : NULL;
        DWORD n = b ? (DWORD)(uintptr_t)read_ptr((char*)b + 0x0C) : 0;
        if (!d || n > 4096) continue;
        for (k = 0; k < n; k++) if (ko_class(read_ptr(d + k)) == cls) return 1;
    }
    return 0;
}

// keep_outdoors: does this door job keep the outdoors? A door within one folder, a recognised
// job, and not the way out of a house when neither keep_interiors nor preload_interiors keeps
// the interior (that job loads `fake` back; it is left to the game as in 0.8.0).
static int ko_allowed(const void* job)
{
    DWORD unload = (DWORD)(uintptr_t)read_ptr((const char*)job + 0x10);
    DWORD buckets = (DWORD)(uintptr_t)read_ptr((const char*)job + 0x20);
    if (g_ptMode != 4 || unload > 4096 || buckets > 5 || !ko_same_folder(g_ptFrom, g_ptTo)) return 0;
    if (!cfg_keep_interiors && !cfg_preload_interiors && ko_loads_class(job, LC_FAKE)) return 0;
    return 1;
}

// release_kept: the outdoor layers keep_outdoors keeps at a door stay in the world while Geralt
// is inside, and the visibility pass walks them every frame although the interior hides them:
// deep inside the cave near Vergen ~8x the range calls and 49 fps instead of 75
// (docs/fixes/interior-preload.md, "Known cost"). A kept door records the layers it kept (not
// interiors or preloaded layers, which preload_interiors keeps on purpose) and where Geralt
// stood. Once he is release_kept metres from that spot, with no switch, request, stream job,
// waiting occluders or loading screen, kr_frame queues a switch to the partition that is already
// current (as ko_frame does for the occluders), and hook_PartRun adds the recorded layers to that
// job's unload list: the game's own job runner unloads them, between its own brackets
// (FUN_00a7015c), with its GC, entity attach and partition settings - no new engine address.
// The way out is an ordinary door job: the partition being entered lists them and the loader
// loads them again (it skips only layers still loaded, +0x38). The switch lists for unloading the
// loaded layers its partition does not hold, so only recorded layers in the release job's own
// unload list go: a layer of the partition Geralt is in is never released. Every job also keeps
// only the recorded layers its own unload list holds; a stream or area switch, or another world,
// forgets the list. A release forgets the instant-door pairs, so the door that loads the layers
// back keeps its fade.
//
// hide_kept: while at least area_memory_mb of address space is free the same switch hides the
// layers instead (docs/fixes/interior-preload.md, "Research: hiding the kept layers"). Every
// loaded layer's attachment is synced by FUN_00a5a90e (EAX = layer info), which asks
// FUN_00a5a568 whether it should be attached; its one call (VA_LAYER_VISIBLE_CALL) goes through
// kh_pred_stub, which answers "no" for the layer infos in g_khSet. hook_PartRun fills the set
// before the job runs, and the job's own attach stage (FUN_00b05a52 on the root group) detaches
// them, as HideLayerGroup() would, without unloading anything and without a GC. Back within half
// the distance, the same kind of switch empties the set and the job attaches them again. Any
// other switch first empties the set (kh_show_now): a door's or an area load's own job then
// attaches them, a stream switch gets them synced at once. Less memory free while hidden:
// unloaded as above.
#define VA_LAYER_SYNC         0x00A5A90Eu   // EAX = layer info: attach or detach to match the predicate
#define VA_LAYER_VISIBLE      0x00A5A568u   // EAX = layer info -> AL: should be attached
#define VA_LAYER_VISIBLE_CALL 0x00A5A92Au   // its only call, in FUN_00a5a90e
#define KR_MAX          1024
#define KR_SETTLE_MS    3000   // no release this soon after the kept door
#define KR_PENDING_MS   5000   // the queued switch must run within this
#define KR_LIGHT_ENTS   8      // hide_kept=1 leaves a kept layer of 1 to this many entities in the world
static void* g_krList[KR_MAX];   // kept layers that no partition since has loaded (game thread)
static int   g_krCount, g_krFull;   // g_krFull: more than KR_MAX kept - nothing is released then
static void* g_krWorld;          // the world they belong to
static float g_krAnchor[3];      // Geralt at the last kept door
static DWORD g_krSince;          // GetTickCount() of the last kept door
static int   g_krPending;        // the release switch is queued
static DWORD g_krPendingTick;
static float g_krDist;
static int   g_krBlockLogged;
static void* g_krGone[KR_MAX];   // released layers: the way out does not report them as missed
static int   g_krGoneCount;
enum { KR_UNLOAD, KR_HIDE, KR_SHOW };
static int   g_krAction;         // what the queued switch does
static uintptr_t g_khPred, g_khSync;   // this build's FUN_00a5a568 / FUN_00a5a90e; 0 = no hide mode
static void* g_khSet[KR_MAX];    // hidden layer infos, sorted (game thread; read by kh_pred_stub)
static volatile LONG g_khCount;
static void* g_khTmp[KR_MAX];

// preload_areas: layers of the small areas behind the current area's doors. They load with the
// area and stay out of the world (never attached, so the renderer does not walk them) until a
// job switches to their partition. Sorted; game thread; read by kh_pred_stub.
#define PA_SET 512
static void* g_paSet[PA_SET];
static volatile LONG g_paSetCount;
static void* g_paWorld;          // the world they belong to
static int   g_paDropPending;    // the switch that unloads them for memory is queued (pa_frame)
static DWORD g_paDropTick;
static DWORD g_paLoadLoaded, g_paLoadSkipped;   // the memory guard's count for the load in progress

static int kp_cmp(const void* a, const void* b);

static int pa_in_set(void* info)
{
    return g_paSetCount && bsearch(&info, g_paSet, (size_t)g_paSetCount, sizeof(void*), kp_cmp) != NULL;
}

static int __cdecl kh_is_hidden(void* info)
{
    if (g_khCount && bsearch(&info, g_khSet, (size_t)g_khCount, sizeof(void*), kp_cmp) != NULL) return 1;
    return pa_in_set(info);
}

// Before a switch that is not a door of this world (hook_PartSwitch, game thread): a stream
// switch, an area load or another world. The set is only emptied: whichever job comes next
// attaches the layers its partition holds and unloads the rest, and no pointer of a world a save
// load replaced is touched.
static void pa_clear(const char* why)
{
    if (g_paSetCount) logf_("preload: %ld preloaded area layers leave the hidden set (%s)", (LONG)g_paSetCount, why);
    g_paSetCount = 0;
    g_paDropPending = 0;
    g_paWorld = NULL;
}

// Replaces `call FUN_00a5a568` (EAX = layer info, result in AL; the original keeps EBX, ESI, EDI
// and EDX). A hidden layer reads as "not visible", every other one gets the game's answer.
__declspec(naked) static void kh_pred_stub(void)
{
    __asm {
        push ecx
        push edx
        push eax
        push eax
        call kh_is_hidden
        add esp, 4
        test eax, eax
        pop eax
        pop edx
        pop ecx
        jnz hidden
        jmp dword ptr [g_khPred]
    hidden:
        xor al, al
        ret
    }
}

#pragma warning(push)
#pragma warning(disable: 4100)   // the argument is read by the asm
__declspec(naked) static void __stdcall kh_sync(void* info)
{
    __asm {
        push ebx
        push esi
        push edi
        mov eax, [esp + 16]
        call dword ptr [g_khSync]
        pop edi
        pop esi
        pop ebx
        ret 4
    }
}
#pragma warning(pop)

// Empty the set before a switch that is not the DLL's (hook_PartSwitch, game thread). A door or
// an area load runs its job at once, and the job's attach stage attaches every loaded layer
// that is out of the set: nothing more to do, and no pointer of a world a save load replaced is
// touched. A stream switch runs its job over later frames: there the layers are synced here.
static void kh_show_now(void* world, int mode)
{
    LONGLONG t0 = qpc_us();
    int n = (int)g_khCount, i, attached = 0;
    if (!n) return;
    memcpy(g_khTmp, g_khSet, (size_t)n * sizeof(void*));
    g_khCount = 0;
    if (mode != 0 || world != g_krWorld || !g_khSync) {
        logf_("interiors: %d hidden layers out of the hidden set before %s - its job attaches them", n,
              mode == 4 ? "a door" : mode == 1 ? "an area load" : "a switch");
        return;
    }
    for (i = 0; i < n; i++)
        if (read_ptr((char*)g_khTmp[i] + 0x38)) { kh_sync(g_khTmp[i]); attached++; }
    logf_("interiors: %d hidden layers shown again before a stream switch (%lu ms)", attached,
          (DWORD)((qpc_us() - t0) / 1000));
}

static void kr_clear(const char* why)
{
    if (g_krCount && cfg_release_kept) logf_("interiors: %d kept outdoor layers forgotten (%s)", g_krCount, why);
    g_krCount = g_krFull = g_krPending = g_krGoneCount = 0;
    g_khCount = 0;   // callers show them first while the world is the same
    g_krWorld = NULL;
}

// hide_kept=1: is this kept layer cheap enough to stay in the world while the rest is hidden?
// With everything hidden the cracks in the cave's roof showed the sky where the kept state
// showed rock (owner, 2026-11-11 and -12). Around the Act 2 quarry the kept outdoors is ~870
// rocks in eight layers, ~500 cooked meshes in layers with no entity, three foliage layers (one
// to three CFoliageEntity each, holding thousands of plants), four terrain tiles (one entity
// with a CTerrainTileComponent each, under `hires terrain\`) and a few layers of a handful of
// plain mesh entities. Three runs on 2026-11-12 (docs/fixes/interior-preload.md):
//   everything hidden                        0.4 M range calls per 5 s at the cliff, sky
//   layers of 1-8 entities stay (26 layers)  5.3 M, as with nothing hidden; rock in the cracks
//   plain mesh layers stay, no terrain (9)   0.36 M, sky
// So what the cracks show is the terrain from below, and the cost is the foliage or the terrain.
// The rule now: a layer stays when it has 1 to KR_LIGHT_ENTS entities and every one is a plain
// CEntity - a mesh holder or a terrain tile. Any subclass (foliage, a nest, an animal manager, a
// container, an item) makes the layer heavy or alive, and it is hidden.
#define VA_ENTITY_VTABLE 0x00E807C4u   // CEntity's own vtable (RTTI, tools/dump-engine-classes.py)
static int kr_is_light(const void* info)
{
    void* layer = info ? read_ptr((const char*)info + 0x38) : NULL;
    void** ents = layer ? (void**)read_ptr((char*)layer + 0x50) : NULL;
    DWORD n = layer ? (DWORD)(uintptr_t)read_ptr((char*)layer + 0x54) : 0, i;
    void* plain = (void*)ga(VA_ENTITY_VTABLE);
    if (cfg_hide_kept >= 2 || !plain || !ents || n < 1 || n > KR_LIGHT_ENTS) return 0;
    for (i = 0; i < n; i++) {
        void* e = read_ptr(ents + i);
        if (!e || read_ptr(e) != plain) return 0;
    }
    return 1;
}

static int kr_find(void* const* set, int n, const void* layer)
{
    int i;
    for (i = 0; i < n; i++) if (set[i] == layer) return i;
    return -1;
}

// Keep only the recorded layers this job lists for unloading (loaded, and not in the partition it
// switches to); the rest belong to that partition now. The job's own list, before hook_PartRun
// points it at its array.
static void kr_keep_listed(void* const* unloadData, LONG unloadN)
{
    int i, n = 0;
    for (i = 0; i < g_krCount; i++)
        if (kr_find(unloadData, unloadN, g_krList[i]) >= 0) g_krList[n++] = g_krList[i];
    g_krCount = n;
    if (!g_krCount) g_krFull = 0;
}

static unsigned char __fastcall hook_PartSwitch(void* world, void* edx, void* name, int mode, void* screen)
{
    wchar_t from[160], to[160];
    void* streaming = read_ptr((char*)world + 0x30C);
    LONGLONG t0 = qpc_us();
    unsigned char ok;
    pt_text(streaming ? (char*)streaming + 0x2C : (char*)world + 0x310, from, 160);
    pt_text(name, to, 160);
    memcpy(g_ptFrom, from, sizeof from);
    memcpy(g_ptTo, to, sizeof to);
    if (cfg_load_trace) {
        logf_("partition: switch '%ls' -> '%ls' (%s)%s", from, to, pt_mode(mode),
              streaming ? ", a stream job is still running" : "");
        pt_where("at the switch", NULL);
    }
    g_ptMode = mode;
    g_koDeferOcc = 0;
    // hide_kept: every switch but the DLL's own finds the hidden layers attached again.
    if (g_khCount && !(g_krPending && mode == 4 && !_wcsicmp(from, to)))
        kh_show_now(world, mode);
    // A stream job is never seen by hook_PartRun, and an area load rebuilds the area.
    if (mode != 4 && g_krCount) kr_clear(mode == 0 ? "a stream switch" : "an area load");
    if (g_paSetCount && (mode != 4 || world != g_paWorld))
        pa_clear(world != g_paWorld ? "another world" : mode == 0 ? "a stream switch" : "an area load");
    ok = ((PartSwitch_t)g_partSwitch)(world, edx, name, mode, screen);
    g_ptMode = -1;
    if (cfg_load_trace) {
        streaming = read_ptr((char*)world + 0x30C);
        if (mode == 0 && streaming) pt_job("async", streaming);
        logf_("partition: switch returned %d after %lu ms", ok, (DWORD)((qpc_us() - t0) / 1000));
    }
    return ok;
}

// The world's layer tree (static analysis, FUN_00a90f59): root group *(world+0x50); a group
// holds layer infos at +0x90 (count +0x94) and child groups at +0x80 (count +0x84). Layer
// info +0x38 = loaded layer, +0x5C = load in progress.
typedef struct { DWORD total, loaded, interiors, interiorsLoaded, added, max; void** out; } ko_tree;

static int kp_in_set(void* info);

static void ko_walk(const void* group, int depth, ko_tree* t)
{
    void** infos;
    DWORD n, i;
    if (!group || depth > 40) return;
    infos = (void**)read_ptr((const char*)group + 0x90);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x94);
    for (i = 0; infos && n < 100000 && i < n; i++) {
        void* info = read_ptr(infos + i);
        int loaded;
        if (!info) continue;
        loaded = read_ptr((char*)info + 0x38) || read_ptr((char*)info + 0x5C);
        t->total++;
        if (loaded) t->loaded++;
        if (ko_class(info) != LC_INTERIOR) continue;
        {
            wchar_t nm[64];
            if (kq_marker_name(pt_text((char*)info + 0x78, nm, 64))) continue;   // a quest marker: with its door
        }
        t->interiors++;
        if (loaded) t->interiorsLoaded++;
        else if (t->out && t->added < t->max && !kp_in_set(info)) t->out[t->added++] = info;
    }
    infos = (void**)read_ptr((const char*)group + 0x80);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x84);
    for (i = 0; infos && n < 100000 && i < n; i++) ko_walk(read_ptr(infos + i), depth + 1, t);
}

static void ko_count(const char* when, void* world)
{
    ko_tree t;
    memset(&t, 0, sizeof t);
    ko_walk(world ? read_ptr((char*)world + 0x50) : NULL, 0, &t);
    logf_("interiors: %s - %lu layers in the world, %lu loaded; %lu building interiors, %lu loaded",
          when, t.total, t.loaded, t.interiors, t.interiorsLoaded);
    log_mem(when);
}

// Which layers are interiors, from the game's own data rather than folder names (the
// prologue and Act 1 name their folders differently). The world's partitions form a tree
// (static analysis: FUN_00a82c99 walks it by name, FUN_00a83097 collects a partition's layer
// groups, FUN_00aa95fa reads inherited properties): root *(*(world+0x358)+0x48); a node has
// its name at +0x04 (String), its location type at +0x28 (0 = inherit from the parent; 1 =
// LT_INDOOR), its parent at +0x60, children at +0x64 (count +0x68) and its own layer groups at
// +0x74 (count +0x78; a String path each, 0x10 apart). A partition gets its own groups plus
// its parents'. The interiors of an area are its indoor siblings (the area's outdoor partition
// and its houses share a parent, e.g. `landing_site\outside\Flotsam` and
// `landing_site\outside\03_clerk_marketplace`): their own groups hold the rooms. Layers in
// those groups are preloaded, except what a door swaps (sound, occluders, the indoor
// environment, low-poly stand-ins) and the townspeople (`comm*`, `community`), who stay with
// the door. g_kpSet remembers what was preloaded so no job unloads it.
// Only house-sized siblings count: a house has one or two groups of its own (the prologue
// village's interior<N>), while the prologue's `Basement` (31) and `OldTunnel` (12) and Act 3's
// `area_sewers` (62) are whole indoor areas. Preloading those beside the prologue's `Dream` put
// the dungeon's occluders, Xbox-only rocks and quest layers into the interrogation cutscene:
// Roche's face and the wall behind him went missing (docs/fixes/interior-preload.md).
#define KP_HOUSE_GROUPS 4
#define KP_GROUPS 512
#define KP_SET    4096
static wchar_t g_kpGroup[KP_GROUPS][200];
static int     g_kpGroups;
static void*   g_kpSet[KP_SET];
static int     g_kpSetCount;

static void* kp_find(void* world, const wchar_t* name)
{
    void* parts = world ? read_ptr((char*)world + 0x358) : NULL;
    void* node = parts ? read_ptr((char*)parts + 0x48) : NULL;
    const wchar_t* at = name;
    while (node && at && *at) {
        const wchar_t* slash = wcschr(at, L'\\');
        size_t n = slash ? (size_t)(slash - at) : wcslen(at);
        void** kids = (void**)read_ptr((char*)node + 0x64);
        DWORD k = (DWORD)(uintptr_t)read_ptr((char*)node + 0x68), i;
        void* next = NULL;
        wchar_t nm[96];
        for (i = 0; kids && k < 10000 && i < k; i++) {
            void* c = read_ptr(kids + i);
            pt_text(c ? (char*)c + 0x04 : NULL, nm, 96);
            if (wcslen(nm) == n && !_wcsnicmp(nm, at, n)) { next = c; break; }
        }
        node = next;
        at = slash ? slash + 1 : NULL;
    }
    return node;
}

static LONG kp_location(void* node)
{
    int d;
    for (d = 0; node && d < 32; d++) {
        LONG t = (LONG)(uintptr_t)read_ptr((char*)node + 0x28);
        if (t) return t;
        node = read_ptr((char*)node + 0x60);
    }
    return 0;
}

// The own layer groups of `target`'s indoor siblings into g_kpGroup; the number of siblings.
static int kp_interior_groups(void* world, const wchar_t* target)
{
    void* node = kp_find(world, target);
    void* parent = node ? read_ptr((char*)node + 0x60) : NULL;
    void** kids = parent ? (void**)read_ptr((char*)parent + 0x64) : NULL;
    DWORD k = parent ? (DWORD)(uintptr_t)read_ptr((char*)parent + 0x68) : 0, i, j;
    int sibs = 0, types[5] = { 0, 0, 0, 0, 0 };
    g_kpGroups = 0;
    for (i = 0; kids && k < 10000 && i < k; i++) {
        void* c = read_ptr(kids + i);
        char* groups;
        DWORD n;
        LONG loc;
        if (!c || c == node) continue;
        loc = kp_location(c);
        types[loc >= 0 && loc <= 3 ? loc : 4]++;
        if (cfg_load_trace) {
            wchar_t nm[96];
            logf_("preload:   sibling '%ls' location type %ld", pt_text((char*)c + 0x04, nm, 96), loc);
        }
        if (loc != 1) continue;
        groups = (char*)read_ptr((char*)c + 0x74);
        n = (DWORD)(uintptr_t)read_ptr((char*)c + 0x78);
        if (n > KP_HOUSE_GROUPS) {
            wchar_t nm[96];
            logf_("preload: indoor sibling '%ls' skipped - %lu layer groups of its own, a whole area, not a house",
                  pt_text((char*)c + 0x04, nm, 96), n);
            continue;
        }
        sibs++;
        for (j = 0; groups && n < 10000 && j < n && g_kpGroups < KP_GROUPS; j++) {
            pt_text(groups + 0x10 * j, g_kpGroup[g_kpGroups], 200);
            if (g_kpGroup[g_kpGroups][0]) g_kpGroups++;
        }
    }
    logf_("preload: '%ls' %s (type %ld); siblings by location type: default %d, indoor %d, outdoor %d, cave %d, other %d; "
          "%d layer groups from the indoor ones", target, node ? "found" : "NOT found in the partition tree",
          node ? kp_location(node) : -1L, types[0], types[1], types[2], types[3], types[4], g_kpGroups);
    return sibs;
}

// Is a layer path inside one of the groups? Group paths are matched as whole path segments.
static int kp_in_groups(const wchar_t* path)
{
    int g;
    for (g = 0; g < g_kpGroups; g++) {
        const wchar_t* hit = wcsistr(path, g_kpGroup[g]);
        size_t n = wcslen(g_kpGroup[g]);
        if (hit && (hit == path || hit[-1] == L'\\') && (hit[n] == L'\\' || hit[n] == 0)) return 1;
    }
    return 0;
}

static int kp_cmp(const void* a, const void* b)
{
    uintptr_t x = (uintptr_t)*(void* const*)a, y = (uintptr_t)*(void* const*)b;
    return x < y ? -1 : x > y;
}

static int kp_in_set(void* info)
{
    return g_kpSetCount && bsearch(&info, g_kpSet, g_kpSetCount, sizeof(void*), kp_cmp) != NULL;
}

// Does the layer file sit directly in one of these group folders? The way the game takes a
// partition's layers: FUN_00a83097 resolves each group path and takes that group's own layer
// infos only (FUN_00b0582e with "recursive" 0).
static int grp_holds(const wchar_t (*groups)[200], int count, const wchar_t* path)
{
    int g;
    for (g = 0; g < count; g++) {
        size_t n = wcslen(groups[g]);
        const wchar_t* hit;
        for (hit = wcsistr(path, groups[g]); hit; hit = wcsistr(hit + 1, groups[g]))
            if ((hit == path || hit[-1] == L'\\') && hit[n] == L'\\' && !wcschr(hit + n + 1, L'\\')) return 1;
    }
    return 0;
}

// Buildings by their lighting. Vergen's houses are not LT_INDOOR and no folder is called
// `interior`, so neither rule above found one (a player and the owner, 0.12.0: nothing
// preloaded, and the town's fake rooms stood inside the real ones). What every one of them has
// is an interior environment of its own: a layer group `environments\...interior...` among its
// partition's groups (`environments\environment_interior\building04_dec`). 13 of Flotsam's 14
// houses have one too (`environments\interior_city`); no street and no outdoor area does, in any
// of the game's five worlds. So a partition with such a group and at most KX_HOUSE_GROUPS groups
// of its own is a building, wherever it is in the tree. Its rooms are the layers of its own
// groups that neither a parent nor a sibling that is no building lists as well, i.e. what its
// door loads: Flotsam's houses also list parts of the street (`pier\buildings\01_crane_pier`).
// These groups are matched the way the game does (grp_holds), not as path prefixes.
// The cap keeps whole areas out: the Arena's `room` (17 groups) and `arena_cutscene` (22) list
// an interior environment and are no houses. Vergen's largest is Philippa's house with 7.
#define KX_HOUSE_GROUPS 8
#define KX_GROUPS 256
static wchar_t g_kxGroup[KX_GROUPS][200];
static int     g_kxGroups, g_kxHouses;

static int kx_is_house(const void* node)
{
    const char* groups = node ? (const char*)read_ptr((const char*)node + 0x74) : NULL;
    DWORD n = node ? (DWORD)(uintptr_t)read_ptr((const char*)node + 0x78) : 0, j;
    wchar_t g[200];
    if (!groups || !n || n > KX_HOUSE_GROUPS) return 0;
    for (j = 0; j < n; j++)
        if (ko_env_interior(pt_text(groups + 0x10 * j, g, 200))) return 1;
    return 0;
}

static int kx_lists(const void* node, const wchar_t* group)
{
    const char* groups = node ? (const char*)read_ptr((const char*)node + 0x74) : NULL;
    DWORD n = node ? (DWORD)(uintptr_t)read_ptr((const char*)node + 0x78) : 0, j;
    wchar_t g[200];
    for (j = 0; groups && n < 10000 && j < n; j++)
        if (!_wcsicmp(pt_text(groups + 0x10 * j, g, 200), group)) return 1;
    return 0;
}

// One family (a partition's children): its buildings' own groups into g_kxGroup, then down.
static void kx_family(const void* parent, int depth)
{
    void** kids = parent ? (void**)read_ptr((const char*)parent + 0x64) : NULL;
    DWORD k = parent ? (DWORD)(uintptr_t)read_ptr((const char*)parent + 0x68) : 0, i, j;
    if (!kids || k > 10000 || depth > 32) return;
    for (i = 0; i < k; i++) {
        void* c = read_ptr(kids + i);
        const char* groups;
        DWORD n;
        if (!c || !kx_is_house(c)) continue;
        g_kxHouses++;
        groups = (const char*)read_ptr((char*)c + 0x74);
        n = (DWORD)(uintptr_t)read_ptr((char*)c + 0x78);
        for (j = 0; j < n && g_kxGroups < KX_GROUPS; j++) {
            wchar_t* g = g_kxGroup[g_kxGroups];
            const void* up;
            DWORD s;
            int d, taken = 0;
            if (!pt_text(groups + 0x10 * j, g, 200)[0]) continue;
            for (d = 0; d < g_kxGroups && !taken; d++) taken = !_wcsicmp(g_kxGroup[d], g);
            for (up = parent, d = 0; up && d < 32 && !taken; up = read_ptr((const char*)up + 0x60), d++)
                taken = kx_lists(up, g);
            for (s = 0; s < k && !taken; s++) {
                void* sib = read_ptr(kids + s);
                taken = sib && sib != c && !kx_is_house(sib) && kx_lists(sib, g);
            }
            if (!taken) g_kxGroups++;
        }
    }
    for (i = 0; i < k; i++) kx_family(read_ptr(kids + i), depth + 1);
}

// Every building of the world; the number found.
static int kx_groups(void* world)
{
    void* parts = world ? read_ptr((char*)world + 0x358) : NULL;
    g_kxGroups = g_kxHouses = 0;
    kx_family(parts ? read_ptr((char*)parts + 0x48) : NULL, 0);
    logf_("preload: %d building(s) with interior lighting of their own in the partition tree, %d layer groups "
          "only they list", g_kxHouses, g_kxGroups);
    return g_kxHouses;
}

// Is a door between these two partitions a house door? One side is a building (it lists an
// interior environment, whatever its size: Flotsam's inn has 11 groups) or a small place with
// at most KX_HOUSE_GROUPS groups of its own (Binduga's houses, the prologue's, Vergen's street).
// Every other door within one folder joins two whole areas: Act 2's quarry and harpy lair,
// every pair in Loc Muinne, the prologue's dungeon halves (11-82 groups a side). In the game's
// five worlds that is 70 house doors and 21 area doors. A partition the tree does not know
// counts as small, so such a door is treated as before.
static int kx_small(const void* node)
{
    const char* groups = node ? (const char*)read_ptr((const char*)node + 0x74) : NULL;
    DWORD n = node ? (DWORD)(uintptr_t)read_ptr((const char*)node + 0x78) : 0, j;
    wchar_t g[200];
    if (!node || n <= KX_HOUSE_GROUPS) return 1;
    for (j = 0; groups && n < 10000 && j < n; j++)
        if (ko_env_interior(pt_text(groups + 0x10 * j, g, 200))) return 1;
    return 0;
}

static int ko_house_door(void* world, const wchar_t* from, const wchar_t* to)
{
    return kx_small(kp_find(world, from)) || kx_small(kp_find(world, to));
}

// `exact` 0: layers under g_kpGroup's folders (the indoor siblings); 1: layers directly in
// g_kxGroup's folders (the buildings). `known` counts every layer that qualifies, loaded or not.
typedef struct { void** out; DWORD added, max, known; int exact; } kp_collect_t;

// Is this layer a quest marker inside a building (kq_marker_name)? A building is a layer under
// an interior folder (LC_INTERIOR) or directly in a group only a building lists (g_kxGroup, set
// by the last area load).
static DWORD g_kqLeft;   // markers the area load in progress left to their doors
static char  g_kqNames[360];
static size_t g_kqUsed;
static int kq_marker(const void* info)
{
    wchar_t name[64], path[260];
    if (!info || !kq_marker_name(pt_text((const char*)info + 0x78, name, 64))) return 0;
    pt_text((const char*)info + 0x68, path, 260);
    return ko_class(info) == LC_INTERIOR || grp_holds(g_kxGroup, g_kxGroups, path);
}

static void kp_walk(const void* group, int depth, kp_collect_t* t)
{
    void** infos;
    DWORD n, i, k;
    if (!group || depth > 40) return;
    infos = (void**)read_ptr((const char*)group + 0x90);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x94);
    for (i = 0; infos && n < 100000 && i < n; i++) {
        void* info = read_ptr(infos + i);
        wchar_t path[260], name[64];
        int c;
        if (!info) continue;
        pt_text((char*)info + 0x68, path, 260);
        if (!(t->exact ? grp_holds(g_kxGroup, g_kxGroups, path) : kp_in_groups(path))) continue;
        c = ko_class(info);
        if (c == LC_SWAP || c == LC_LOWPOLY || c == LC_FAKE || c == LC_ENV) continue;
        pt_text((char*)info + 0x78, name, 64);
        if (!_wcsnicmp(name, L"comm", 4) || wcsistr(path, L"\\communit")) continue;
        if (kq_marker(info)) {
            g_kqLeft++;
            if (g_kqUsed < sizeof g_kqNames - 1)
                g_kqUsed += (size_t)_snprintf_s(g_kqNames + g_kqUsed, sizeof g_kqNames - g_kqUsed, _TRUNCATE, " %ls", path);
            continue;
        }
        t->known++;
        if (g_kpSetCount < KP_SET && !kp_in_set(info)) {
            g_kpSet[g_kpSetCount++] = info;
            qsort(g_kpSet, g_kpSetCount, sizeof(void*), kp_cmp);
        }
        if (read_ptr((char*)info + 0x38) || read_ptr((char*)info + 0x5C)) continue;
        for (k = 0; k < t->added && t->out[k] != info; k++) {}   // both rules may name it
        if (k == t->added && t->added < t->max) t->out[t->added++] = info;
    }
    infos = (void**)read_ptr((const char*)group + 0x80);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x84);
    for (i = 0; infos && n < 100000 && i < n; i++) kp_walk(read_ptr(infos + i), depth + 1, t);
}

// A partition's own layer groups, the way the game loads them: FUN_00a83097 resolves each
// group path (FUN_00b059d9) and takes that group's own layer infos only (FUN_00b0582e with
// "recursive" 0), so a layer belongs to a group when its file sits directly in the group's
// folder. Used for the street of a save inside a house and for the check before a door goes
// without a fade (docs/fixes/door-transitions.md).
#define KS_GROUPS 128
static wchar_t g_ksGroup[KS_GROUPS][200];
static int     g_ksGroups;

static DWORD ks_count(const void* node)
{
    DWORD n = node ? (DWORD)(uintptr_t)read_ptr((const char*)node + 0x78) : 0;
    return n < 10000 ? n : 0;
}

static void ks_take_groups(const void* node)
{
    const char* groups = node ? (const char*)read_ptr((const char*)node + 0x74) : NULL;
    DWORD n = ks_count(node), j;
    g_ksGroups = 0;
    for (j = 0; groups && j < n && g_ksGroups < KS_GROUPS; j++) {
        pt_text(groups + 0x10 * j, g_ksGroup[g_ksGroups], 200);
        if (g_ksGroup[g_ksGroups][0]) g_ksGroups++;
    }
}

// Does the layer file sit directly in one of g_ksGroup's folders?
static int ks_in_groups(const wchar_t* path)
{
    return grp_holds(g_ksGroup, g_ksGroups, path);
}

// What a kept door would still have to load for these groups: layer infos not loaded or
// loading, without what a door swaps or skips anyway. With `out`, they are collected too.
typedef struct { void** out; DWORD added, max, missing; } ks_walk_t;

static void ks_walk(const void* group, int depth, ks_walk_t* t)
{
    void** infos;
    DWORD n, i;
    if (!group || depth > 40) return;
    infos = (void**)read_ptr((const char*)group + 0x90);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x94);
    for (i = 0; infos && n < 100000 && i < n; i++) {
        void* info = read_ptr(infos + i);
        wchar_t path[260];
        int c;
        if (!info || read_ptr((char*)info + 0x38) || read_ptr((char*)info + 0x5C)) continue;
        pt_text((char*)info + 0x68, path, 260);
        if (!ks_in_groups(path)) continue;
        c = ko_class(info);
        if (c == LC_SWAP || c == LC_LOWPOLY || c == LC_FAKE || c == LC_ENV) continue;
        t->missing++;
        if (t->out && t->added < t->max) t->out[t->added++] = info;
    }
    infos = (void**)read_ptr((const char*)group + 0x80);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x84);
    for (i = 0; infos && n < 100000 && i < n; i++) ks_walk(read_ptr(infos + i), depth + 1, t);
}

// The street of an interior partition: the sibling that clearly has the most groups of its
// own (Flotsam 51 against at most 18, Binduga 32 against 2-3, the prologue village's
// `outside` 7 against 1-2). NULL when `target` is that street itself, has children of its
// own, or no sibling stands out. Vergen's street (`city_dec_unload`, 5 groups) is smaller than
// some of its houses (Philippa's, 7), with the town itself in their parent: there the street of
// a building (kx_is_house) is its one sibling that is no building.
static void* ks_street(void* world, const wchar_t* target)
{
    void* node = kp_find(world, target);
    void* parent = node ? read_ptr((char*)node + 0x60) : NULL;
    void** kids = parent ? (void**)read_ptr((char*)parent + 0x64) : NULL;
    DWORD k = parent ? (DWORD)(uintptr_t)read_ptr((char*)parent + 0x68) : 0, i, best = 0, second = 0;
    void *street = NULL, *plain = NULL;
    int plains = 0;
    if (!node) return NULL;
    for (i = 0; kids && k < 10000 && i < k; i++) {
        void* c = read_ptr(kids + i);
        DWORD n = ks_count(c);
        if (!c) continue;
        if (c != node && !kx_is_house(c)) { plain = c; plains++; }
        if (n > best) { second = best; best = n; street = c; }
        else if (n > second) second = n;
    }
    if (kx_is_house(node) && plains == 1) return plain;
    if (read_ptr((char*)node + 0x68)) return NULL;
    if (!street || street == node || best < 5 || best < 2 * second) return NULL;
    return street;
}

// Kept-door check: how many layers of `target`'s own groups are still to load.
static DWORD ks_missing(void* world, const wchar_t* target)
{
    ks_walk_t t;
    memset(&t, 0, sizeof t);
    ks_take_groups(kp_find(world, target));
    if (g_ksGroups) ks_walk(world ? read_ptr((char*)world + 0x50) : NULL, 0, &t);
    return t.missing;
}


// ---- memory guard for kept and preloaded layers ----------------------------------
// docs/fixes/door-transitions.md, "Memory". The game is a 32-bit process (4 GB with LAA).
// With the system's Direct3D 9 the managed textures keep a copy in the process: loading the
// Flotsam inn save took 1.74 -> 2.81 GB with every interior and the street preloaded, and the
// texture streaming then ran the process out of address space (owner, 2026-11-04 15:18: a
// NULL from the renderer's allocator at 009CB57D, 3.4 GB). DXVK keeps them on the GPU (1.07 ->
// 1.33 GB for the same load). So what the DLL adds on top of the game is held to the address
// space left:
//  - an area load's preloaded layers (interiors, a house's street) go to the end of the job's
//    last bucket, after the area's own layers; the per-layer load FUN_00a5ac6c (EDI = layer
//    info, one stack argument, RET 4; its only call is 00A91709 in FUN_00a91600) goes through
//    layer_load_stub, which skips a preloaded layer once less than preload_memory_mb is free.
//    Those houses then load on their first visit, with a fade (the door check, ks_missing);
//  - a door that starts with less than door_memory_mb free is the game's own door: the game's
//    job unloads the outdoors around it, its GC runs, and the door fades.
// Without the per-layer hook (a build it is not ported to) the preload is skipped as a whole
// when less than preload_memory_mb + KM_COARSE_MB is free before the load.
#define VA_LAYER_LOAD_CALL  0x00A91709u   // call FUN_00a5ac6c in FUN_00a91600, its only caller
#define VA_LAYER_LOAD       0x00A5AC6Cu
#define KM_COARSE_MB        1024
#define KM_SET              1024          // preloaded layers watched per load
#define KM_ORIG             8192          // the job's own load list, sorted

#define KM_FAKE             16            // fake-room layers of one area load (the game has one each)

static void* g_kmSet[KM_SET];
static void* g_kmOrig[KM_ORIG];
static int   g_kmCount;
static volatile int g_kmActive;
static DWORD g_kmLoaded, g_kmSkipped, g_kmFreeMin;
// The area's fake rooms go to the very end of the load, after the preloaded layers: they are
// left out when every building loaded and loaded when one could not (kp_layer_ok). g_kpFakeKeep
// says they are in use, until the next area load: a door then swaps them the way the game does
// and keeps its fade. Up to 0.12.0 they were left out whatever the memory guard skipped, so
// with little address space every open door showed nothing behind it.
static void* g_kmFake[KM_FAKE];
static int   g_kmFakeCount;
static int   g_kpFakeKeep, g_kpFakeSeen;
static uintptr_t g_layerLoad;   // this build's FUN_00a5ac6c
static int g_layerHooked;
// The door in progress was left to the game for memory: 1 = wholly (door_memory_mb: the
// outdoors and the other interiors unload), 2 = a door between two areas (area_memory_mb: the
// area left behind unloads, the preloaded interiors stay).
static int g_koMemLow;
static void* g_koMemLowTask;    // that door's task: fast_doors leaves its timing to the game

// Free user address space of the process, in MB (cheap: no address space walk).
static DWORD ko_free_mb(void)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof ms;
    return GlobalMemoryStatusEx(&ms) ? (DWORD)(ms.ullAvailVirtual >> 20) : 0xFFFFFFFFu;
}

// 0 = skip this layer's load: a preloaded layer while too little address space is left.
static int __stdcall kp_layer_ok(void* info)
{
    DWORD freeMb;
    int i;
    if (!g_kmActive || !info) return 1;
    for (i = 0; i < g_kmFakeCount; i++)
        if (g_kmFake[i] == info) return g_kmSkipped != 0;
    if (!g_kmCount) return 1;
    if (!bsearch(&info, g_kmSet, g_kmCount, sizeof(void*), kp_cmp)) return 1;
    freeMb = ko_free_mb();
    if (freeMb < g_kmFreeMin) g_kmFreeMin = freeMb;
    // A neighbour area's layer (preload_areas) comes after the interiors and needs more room:
    // area_memory_mb, the room a door between two areas wants before it keeps both.
    if (pa_in_set(info)) {
        if (freeMb < (DWORD)(cfg_area_memory_mb > cfg_preload_memory_mb ? cfg_area_memory_mb : cfg_preload_memory_mb)) {
            g_paLoadSkipped++;
            return 0;
        }
        g_paLoadLoaded++;
        return 1;
    }
    if (freeMb < (DWORD)cfg_preload_memory_mb) {
        g_kmSkipped++;
        return 0;
    }
    g_kmLoaded++;
    return 1;
}

// Replaces `call FUN_00a5ac6c` at 00A91709. EDI (the layer info) is kept for the original; a
// skipped load returns 0 and pops its one argument, as the original does.
__declspec(naked) static void layer_load_stub(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push edi
        call kp_layer_ok
        test eax, eax
        pop edx
        pop ecx
        pop eax
        jz skip
        jmp dword ptr [g_layerLoad]
    skip:
        xor eax, eax
        ret 4
    }
}

// GOG's FUN_00a5ac6c is 00AAE95E, the same code with the info in EBX: stdcall (info, ctx),
// RET 8. Its one call, 00AE5D66 in the bucket loader 00AE5C5A, pushes ctx and then the info.
__declspec(naked) static void layer_load_stub_gog(void)
{
    __asm {
        push eax
        push ecx
        push edx
        push dword ptr [esp + 16]         ; the layer info, the first argument
        call kp_layer_ok
        test eax, eax
        pop edx
        pop ecx
        pop eax
        jz skip
        jmp dword ptr [g_layerLoad]
    skip:
        xor eax, eax
        ret 8
    }
}

// .text is decrypted by the SteamStub: retried from doors_tick until the fingerprint holds.
static int layer_install(void)
{
    uintptr_t call = ga(VA_LAYER_LOAD_CALL);
    if (g_layerHooked) return 1;
    if (!call || !code_matches(call - 14, by_build("8b 7d ec 8d 45 d4 50 89 5d d4 c6 45 d8 01 e8 5e 95 fc ff",
                                                   "8d 45 d4 50 ff 75 ec 89 5d d4 c6 45 d8 01 e8 f3 8b fc ff")))
        return 0;
    g_layerLoad = ga(VA_LAYER_LOAD);
    g_layerHooked = patch_call(call, g_layerLoad,
                               g_build == BUILD_GOG ? (void*)layer_load_stub_gog : (void*)layer_load_stub);
    logf_("preload: per-layer load %s - preloaded layers stop below %d MB of free address space",
          g_layerHooked ? "hooked" : "NOT hooked", cfg_preload_memory_mb);
    return 1;
}

// The layers this load added (out[0..n)) that the job did not list itself go into g_kmSet:
// the area's own layers always load, whatever the memory.
static void km_watch(void* const* own[], const LONG ownN[], int buckets, void* const* out, DWORD n)
{
    int no = 0, b;
    LONG i;
    DWORD k;
    g_kmCount = 0;
    g_kmLoaded = g_kmSkipped = 0;
    g_kmFreeMin = 0xFFFFFFFFu;
    for (b = 0; b < buckets; b++)
        for (i = 0; own[b] && i < ownN[b] && no < KM_ORIG; i++) g_kmOrig[no++] = own[b][i];
    qsort(g_kmOrig, no, sizeof(void*), kp_cmp);
    for (k = 0; k < n && g_kmCount < KM_SET; k++)
        if (out[k] && !bsearch(&out[k], g_kmOrig, no, sizeof(void*), kp_cmp)) g_kmSet[g_kmCount++] = out[k];
    qsort(g_kmSet, g_kmCount, sizeof(void*), kp_cmp);
}

// Occluders on a kept door. Occluders are invisible blocks the renderer uses to skip what is
// hidden behind a building. A door into a Flotsam building unloads the town's occluders
// (`marketplace\occluders`, `occluders\flotsam_outside`) and the way out loads them back; with
// no fade the camera is still inside the building's occluder block at that moment, so
// everything in front of it is culled and only water and sky show until Geralt is out (owner,
// 0.8.0, the inn's harbour door). The game hid this behind its fade. So a kept door leaves
// its occluder layers out of the job, and once Geralt is KO_OCC_DIST metres from the door (or
// after KO_OCC_MAX_MS) ko_frame queues a switch to the partition that is already current: the
// game's own job then loads exactly what is missing - the occluders. That switch (from and to
// the same partition) is never deferred itself, or it would repeat every KO_OCC_MIN_MS (it did
// in the first test). Needs a per-frame tick
// on the game thread (the viewport tick, wf_vp_tick_hook); without it nothing is deferred.
#define KO_OCC_DIST     5.0f
#define KO_OCC_MIN_MS   400
#define KO_OCC_MAX_MS   30000
#define VA_PART_REQUEST 0x00AA7E18u   // EAX = game; stdcall (String* name, mode, String* screen), RET 0xC; ECX on GOG
#define VA_EMPTY_STRING 0x0160FBA0u   // the game's empty String, its usual loading-screen argument

static volatile DWORD g_koFrameTick;   // last viewport tick (game thread)
static void cq_poll(int gameThread);   // clean_quit, from ko_frame
static void lscr_frame(void);          // loading_screen_cleanup, from ko_frame
static volatile DWORD g_koOccTick;     // GetTickCount() | 1 when the occluders were requested
static int   g_koDeferOcc;
static DWORD g_koDeferSince;
static float g_koDoorPos[3];
static int   g_koDoorPosOk;

static int ko_frames_alive(void) { return g_koFrameTick && GetTickCount() - g_koFrameTick < 1000; }

static int ko_is_occluder(const void* layer)
{
    wchar_t name[64], path[260];
    if (ko_class(layer) != LC_SWAP) return 0;
    pt_text((const char*)layer + 0x78, name, 64);
    pt_text((const char*)layer + 0x68, path, 260);
    return wcsistr(name, L"occ") != NULL || wcsistr(path, L"\\occluders\\") != NULL;
}

static void ko_defer_occluders(void)
{
    g_koDeferOcc = 1;
    g_koDeferSince = GetTickCount();
}

// Steam's request (FUN_00aa7e18) takes the game in EAX; GOG's (00AFCFA6) is thiscall with it in
// ECX. Neither reads the other register or EDX before writing it, so the game goes in both and
// the call goes through EDX.
#pragma warning(push)
#pragma warning(disable: 4100)   // the arguments are read by the asm
__declspec(naked) static unsigned char __stdcall ko_part_request(void* game, void* name, int mode, void* screen)
{
    __asm {
        mov eax, [esp + 4]
        mov ecx, eax
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        push dword ptr [esp + 16]
        mov edx, dword ptr [g_partRequest]   ; VA_PART_REQUEST on this build
        call edx
        ret 16
    }
}
#pragma warning(pop)

static int kr_player_pos(float* p)
{
    void* game = read_ptr((void*)ga(VA_GAME));
    void* player = game ? read_ptr((char*)game + OFF_GAME_PLAYER) : NULL;
    int i;
    for (i = 0; player && i < 3 && read_f32((char*)player + OFF_NODE_POS + 4 * i, &p[i]); i++) {}
    return player && i == 3;
}

// After a kept door ran (hook_PartRun, game thread): remember the layers it kept and where.
static void kr_record(void* const* unloadData, LONG unloadN, void* world)
{
    LONG i;
    int added = 0, wasFull = g_krFull;
    if (!cfg_release_kept || !g_partRequest) return;
    if (g_krWorld != world) { g_krCount = g_krFull = g_krGoneCount = 0; g_khCount = 0; g_krWorld = world; }
    for (i = 0; i < unloadN; i++) {
        void* l = unloadData[i];
        if (!l || ko_class(l) != LC_OTHER || kp_in_set(l) || pa_in_set(l) || kr_find(g_krList, g_krCount, l) >= 0) continue;
        if (g_krCount >= KR_MAX) { g_krFull = 1; continue; }
        g_krList[g_krCount++] = l;
        added++;
    }
    if (!g_krCount) return;
    // The last kept door is the one Geralt would go back through.
    g_krAnchor[0] = g_krAnchor[1] = g_krAnchor[2] = 0;
    if (!kr_player_pos(g_krAnchor)) g_krFull = 1;
    g_krSince = GetTickCount();
    g_krBlockLogged = 0;
    if (g_krFull && !wasFull)
        logf_("interiors: more than %d kept outdoor layers (or Geralt not found) - none are released here", KR_MAX);
    else if (added)
        logf_("interiors: %d kept outdoor layers recorded (%d in all) - released once Geralt is %d m from "
              "(%.1f, %.1f, %.1f)", added, g_krCount, cfg_release_kept, g_krAnchor[0], g_krAnchor[1], g_krAnchor[2]);
}

// Once per frame (ko_frame): far enough from the kept door and nothing else going on -> queue the
// switch whose job hook_PartRun turns into the release.
static void kr_frame(void)
{
    static DWORD last;
    DWORD now = GetTickCount();
    void *game, *world;
    char* rt;
    float p[3], dx, dy, dz, d;
    const char* block = NULL;
    int action, hideOk;
    if (!cfg_release_kept || !g_krCount) return;
    if (g_krPending) {
        if (now - g_krPendingTick < KR_PENDING_MS) return;
        logf_("interiors: the release switch did not run within %d ms - asking again later", KR_PENDING_MS);
        g_krPending = 0;
    }
    if (now - last < 250) return;
    last = now;
    game = read_ptr((void*)ga(VA_GAME));
    world = game ? read_ptr((char*)game + 0x40) : NULL;
    if (!world) return;
    if (world != g_krWorld) { kr_clear("another world"); return; }
    if (g_krFull || !kr_player_pos(p)) return;
    dx = p[0] - g_krAnchor[0]; dy = p[1] - g_krAnchor[1]; dz = p[2] - g_krAnchor[2];
    d = sqrtf(dx * dx + dy * dy + dz * dz);
    hideOk = g_khPred && cfg_hide_kept && (!cfg_area_memory_mb || ko_free_mb() >= (DWORD)cfg_area_memory_mb);
    if (g_khCount) {
        // Hidden: shown again on the way back, unloaded if the address space runs short.
        if (d < 0.5f * (float)cfg_release_kept) action = KR_SHOW;
        else if (!hideOk) action = KR_UNLOAD;
        else return;
    } else {
        if (d < (float)cfg_release_kept || now - g_krSince < KR_SETTLE_MS) return;
        action = hideOk ? KR_HIDE : KR_UNLOAD;
    }
    rt = (char*)read_ptr((void*)ga(VA_RENDER_THREAD));
    if (g_ptMode != -1 || read_ptr((char*)game + 0xE8)) block = "a switch is queued";
    else if (read_ptr((char*)world + 0x30C)) block = "a stream job is running";
    else if (g_koDeferOcc) block = "the occluders still wait";
    // The viewport at +0x268 says a loading screen is up. The screen object at +0x26C alone does
    // not: the game can leave one behind after its loading screen ended (lscr_frame), and that
    // blocked every release for the rest of a session (2026-11-11, 55 fps at the cliff).
    else if (rt && read_ptr(rt + OFF_RT_LOAD_MOVIE)) block = "a loading screen is up";
    else if (g_paDropPending) block = "the preloaded areas are being unloaded";
    if (block) {
        if (!g_krBlockLogged) logf_("interiors: releasing the kept outdoor layers waits - %s", block);
        g_krBlockLogged = 1;
        return;
    }
    g_krPending = 1;
    g_krPendingTick = now;
    g_krDist = d;
    g_krAction = action;
    g_krBlockLogged = 0;
    g_koOccTick = now | 1;   // the switch's spinner is hidden as for the occluders (hook_SpinnerExec)
    ko_part_request(game, (char*)world + 0x310, 4, (void*)ga(VA_EMPTY_STRING));
}

// ---- preload_areas: the small areas behind an area's own doors -----------------
// preload_interiors loads building interiors with an area; a small whole area behind a door of
// the same folder (the quarry's cave `common\secret_passage`, 16 layers) still loaded on its
// first visit behind the area fade, and was only kept from then on. The doors name it: a
// CStreamingDoor (VA_DOOR_VTABLE, class size 0x250 on both builds) holds its `inside` and
// `outside` SDoorPartition at +0x200 / +0x214, the partition name String first (property
// registration Steam 0050282A, GOG 005AFCC5); a loaded layer (info +0x38) holds its entities
// at +0x50, count +0x54.
//
// The doors are entities of the area's own layers, so they can only be read once the area is
// loaded. Two earlier versions loaded the neighbours right then, in a second switch or a second
// run of the area's job; both sessions ended with a loading-screen object left on the render
// thread (docs/fixes/interior-preload.md). So the neighbours are learned, not loaded, at that
// point (pa_learn: the pairs go into g_paKnown and into <cache>\area-doors.txt), and the next
// load of that area, in this session or a later one, puts their layers into its own job: the
// last bucket, after the interiors, in the one run the game makes anyway (pa_plan).
//
// They load hidden. A layer that is loaded and attached is walked by the renderer's visibility
// pass every frame, seen or not (the cost hide_kept removes for the kept outdoors). The planned
// layers go into g_paSet before the run, so the job's attach stage leaves them out of the world
// (kh_pred_stub), as it does for a layer group a quest has hidden. They cost memory only.
//  - A door into such an area finds nothing to load (hook_DoorStart, ks_missing): no fade. Its
//    job lists for unloading the loaded layers its partition does not hold; a hidden layer that
//    is not in that list belongs to the partition being entered and leaves the set before the
//    run (pa_keep_listed), so the same job attaches it. The other neighbours stay hidden.
//  - Memory: they load only while area_memory_mb is free (kp_layer_ok, per layer), and once
//    less than that is free during play pa_frame queues a switch to the current partition whose
//    job unloads them (hook_PartRun), as release_kept does for the kept outdoors. A door left
//    to the game for memory unloads them with everything else. After each job the set drops
//    what is no longer loaded (pa_prune).
//  - A stream switch, an area load or another world empties the set (pa_clear).
#define VA_DOOR_VTABLE    0x00EF0084u   // CStreamingDoor (slot 78 = VA_DOOR_TIMER_SLOT, 153 = VA_DOOR_START_SLOT)
#define OFF_DOOR_INSIDE   0x200
#define OFF_DOOR_OUTSIDE  0x214
#define PA_MAX_LAYERS     64            // a bigger area is left to its first visit
#define PA_AREAS          16            // partitions behind one area's doors
#define PA_KNOWN          256           // learned (area, neighbour) pairs
#define PA_NAME           128

typedef struct { wchar_t from[PA_NAME], to[PA_NAME]; } pa_pair_t;
static pa_pair_t g_paKnown[PA_KNOWN];
static int g_paKnownCount, g_paKnownRead;

typedef struct { wchar_t name[PA_AREAS][160]; int n; void* vt; } pa_doors_t;

static void pa_file(char* path, size_t cap, int makeDir)
{
    if (makeDir) {
        sprintf_s(path, cap, "%s\\witcher2_enginefix_cache", g_binDir);
        CreateDirectoryA(path, NULL);
    }
    sprintf_s(path, cap, "%s\\witcher2_enginefix_cache\\area-doors.txt", g_binDir);
}

static int pa_known_find(const wchar_t* from, const wchar_t* to)
{
    int i;
    for (i = 0; i < g_paKnownCount; i++)
        if (!_wcsicmp(g_paKnown[i].from, from) && !_wcsicmp(g_paKnown[i].to, to)) return i;
    return -1;
}

// 1 = new. Names that do not fit are left out: cut short they would name nothing.
static int pa_known_put(const wchar_t* from, const wchar_t* to)
{
    if (!from[0] || !to[0] || wcslen(from) >= PA_NAME || wcslen(to) >= PA_NAME) return 0;
    if (g_paKnownCount >= PA_KNOWN || pa_known_find(from, to) >= 0) return 0;
    wcscpy_s(g_paKnown[g_paKnownCount].from, PA_NAME, from);
    wcscpy_s(g_paKnown[g_paKnownCount].to, PA_NAME, to);
    g_paKnownCount++;
    return 1;
}

// What earlier sessions learned: one "area<TAB>neighbour" pair per line, UTF-8.
static void pa_known_read(void)
{
    char path[MAX_PATH], line[2 * 3 * PA_NAME + 8];
    FILE* f;
    if (g_paKnownRead) return;
    g_paKnownRead = 1;
    pa_file(path, sizeof path, 0);
    f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        wchar_t from[PA_NAME], to[PA_NAME];
        char* tab = strchr(line, '\t');
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!tab) continue;
        *tab = 0;
        if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, from, PA_NAME) ||
            !MultiByteToWideChar(CP_UTF8, 0, tab + 1, -1, to, PA_NAME))
            continue;
        pa_known_put(from, to);
    }
    fclose(f);
    logf_("preload: %d door(s) between areas known from earlier sessions", g_paKnownCount);
}

static void pa_known_add(const wchar_t* from, const wchar_t* to)
{
    char path[MAX_PATH], a[3 * PA_NAME], b[3 * PA_NAME];
    FILE* f;
    if (!pa_known_put(from, to)) return;
    if (!WideCharToMultiByte(CP_UTF8, 0, from, -1, a, sizeof a, NULL, NULL) ||
        !WideCharToMultiByte(CP_UTF8, 0, to, -1, b, sizeof b, NULL, NULL))
        return;
    pa_file(path, sizeof path, 1);
    f = _fsopen(path, "ab", _SH_DENYWR);
    if (!f) return;   // known for this session only
    fprintf(f, "%s\t%s\r\n", a, b);
    fclose(f);
}

static void pa_note(pa_doors_t* d, const void* str)
{
    wchar_t t[160];
    int i;
    pt_text(str, t, 160);
    if (!t[0] || d->n >= PA_AREAS) return;
    for (i = 0; i < d->n; i++) if (!_wcsicmp(d->name[i], t)) return;
    memcpy(d->name[d->n++], t, sizeof t);
}

// The doors in the area's own loaded layers; a hidden neighbour's doors lead on from there.
static void pa_doors(const void* group, int depth, pa_doors_t* d)
{
    void** infos;
    DWORD n, i, k;
    if (!group || depth > 40) return;
    infos = (void**)read_ptr((const char*)group + 0x90);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x94);
    for (i = 0; infos && n < 100000 && i < n; i++) {
        void* info = read_ptr(infos + i);
        void* layer = info ? read_ptr((char*)info + 0x38) : NULL;
        void** ents = layer ? (void**)read_ptr((char*)layer + 0x50) : NULL;
        DWORD m = layer ? (DWORD)(uintptr_t)read_ptr((char*)layer + 0x54) : 0;
        if (!layer || pa_in_set(info)) continue;
        for (k = 0; ents && m < 100000 && k < m; k++) {
            void* e = read_ptr(ents + k);
            if (e && read_ptr(e) == d->vt) {
                pa_note(d, (char*)e + OFF_DOOR_INSIDE);
                pa_note(d, (char*)e + OFF_DOOR_OUTSIDE);
            }
        }
    }
    infos = (void**)read_ptr((const char*)group + 0x80);
    n = (DWORD)(uintptr_t)read_ptr((const char*)group + 0x84);
    for (i = 0; infos && n < 100000 && i < n; i++) pa_doors(read_ptr(infos + i), depth + 1, d);
}

// Right after the area job into `cur` ran (hook_PartRun, game thread): which whole areas of the
// same folder are behind its doors. Houses and small places are the interiors' business.
static void pa_learn(void* world, const wchar_t* cur)
{
    static pa_doors_t d;
    int i, areas = 0, fresh = 0, houses = 0, other = 0;
    void* root = world ? read_ptr((char*)world + 0x50) : NULL;
    memset(&d, 0, sizeof d);
    d.vt = (void*)ga(VA_DOOR_VTABLE);
    if (!d.vt || !root || !cur[0]) return;
    pa_known_read();
    pa_doors(root, 0, &d);
    for (i = 0; i < d.n; i++) {
        void* node;
        int before = g_paKnownCount;
        if (!_wcsicmp(d.name[i], cur)) continue;
        if (!ko_same_folder(cur, d.name[i])) { other++; continue; }
        node = kp_find(world, d.name[i]);
        if (!node || kx_small(node)) { houses++; continue; }
        areas++;
        pa_known_add(cur, d.name[i]);
        fresh += g_paKnownCount - before;
    }
    if (d.n)
        logf_("preload: the doors of '%ls' lead to %d partition(s): %d whole area(s) in its folder (%d new - they load "
              "with this area from its next load on), %d houses or small places, %d in other folders",
              cur, d.n, areas, fresh, houses, other);
}

// Before the area job into `cur` runs: the layers still to load of the small areas known to be
// behind its doors, into out[0..max). The number of layers; *areas the number of areas.
static DWORD pa_plan(void* world, const wchar_t* cur, void** out, DWORD cap, int* areas)
{
    char names[400];
    size_t used = 0;
    int i, big = 0, loaded = 0, gone = 0, known = 0;
    DWORD total = 0, freeMb = ko_free_mb();
    DWORD need = (DWORD)(cfg_area_memory_mb > cfg_preload_memory_mb ? cfg_area_memory_mb : cfg_preload_memory_mb);
    void* root = world ? read_ptr((char*)world + 0x50) : NULL;
    *areas = 0;
    names[0] = 0;
    if (!root || !cur[0]) return 0;
    pa_known_read();
    for (i = 0; i < g_paKnownCount; i++) known += !_wcsicmp(g_paKnown[i].from, cur);
    if (!known) return 0;
    if (freeMb < need) {
        logf_("preload: only %lu MB of address space free (under %lu) - the %d area(s) behind the doors of '%ls' load "
              "on their first visit", freeMb, need, known, cur);
        return 0;
    }
    if (cap > PA_SET - (DWORD)g_paSetCount) cap = PA_SET - (DWORD)g_paSetCount;
    for (i = 0; i < g_paKnownCount; i++) {
        void* node;
        ks_walk_t t;
        if (_wcsicmp(g_paKnown[i].from, cur)) continue;
        node = kp_find(world, g_paKnown[i].to);
        if (!node || kx_small(node)) { gone++; continue; }   // another world's name, or a house after all
        memset(&t, 0, sizeof t);
        ks_take_groups(node);
        if (g_ksGroups) ks_walk(root, 0, &t);   // count first
        if (!t.missing) { loaded++; continue; }
        if (t.missing > PA_MAX_LAYERS || total + t.missing > cap) { big++; continue; }
        memset(&t, 0, sizeof t);
        t.out = out + total;
        t.max = cap - total;
        ks_walk(root, 0, &t);
        total += t.added;
        (*areas)++;
        if (used < sizeof names - 1)
            used += (size_t)_snprintf_s(names + used, sizeof names - used, _TRUNCATE, " '%ls' (%lu)", g_paKnown[i].to, t.added);
    }
    logf_("preload: %d area(s) known behind the doors of '%ls': %d to load now,%s %d too big, %d already loaded, %d not "
          "in this world (%lu MB free, floor %lu)", known, cur, *areas, used ? names : "", big, loaded, gone, freeMb, need);
    return total;
}

// The planned layers into the hidden set, before the run. Layers the job lists itself are the
// area's own and stay out of it.
static void pa_set_add(void* const* layers, DWORD n, void* world)
{
    DWORD i;
    if (g_paWorld != world) { g_paSetCount = 0; g_paWorld = world; }
    for (i = 0; i < n && g_paSetCount < PA_SET; i++)
        if (layers[i] && !pa_in_set(layers[i])) {
            g_paSet[g_paSetCount] = layers[i];
            g_paSetCount = g_paSetCount + 1;
            qsort(g_paSet, (size_t)g_paSetCount, sizeof(void*), kp_cmp);
        }
}

// Before a job runs: keep hidden only what this job lists for unloading (loaded, and not in the
// partition it switches to). The rest belongs to that partition now and the job's attach stage
// puts it into the world. The number that left the set.
static int pa_keep_listed(void* const* unloadData, LONG unloadN)
{
    int i, n = 0, before = (int)g_paSetCount;
    for (i = 0; i < before; i++)
        if (kr_find(unloadData, unloadN, g_paSet[i]) >= 0) g_paSet[n++] = g_paSet[i];   // order kept: still sorted
    g_paSetCount = n;
    return before - n;
}

// After a job ran: what it unloaded (or what never loaded) is hidden no longer. The number left.
static int pa_prune(void)
{
    int i, n = 0, before = (int)g_paSetCount;
    for (i = 0; i < before; i++)
        if (read_ptr((char*)g_paSet[i] + 0x38) || read_ptr((char*)g_paSet[i] + 0x5C)) g_paSet[n++] = g_paSet[i];
    g_paSetCount = n;
    if (!n) g_paDropPending = 0;
    return n;
}

// Once a second (ko_frame): with less than area_memory_mb free the preloaded areas give their
// memory back. The switch to the current partition lists them for unloading; hook_PartRun lets
// those go.
static void pa_frame(void)
{
    static DWORD last;
    DWORD now = GetTickCount(), freeMb;
    void *game, *world;
    char* rt;
    if (!g_paSetCount || !g_partRequest || !cfg_area_memory_mb) return;
    if (g_paDropPending) {
        if (now - g_paDropTick < KR_PENDING_MS) return;
        g_paDropPending = 0;
    }
    if (now - last < 1000) return;
    last = now;
    freeMb = ko_free_mb();
    if (freeMb >= (DWORD)cfg_area_memory_mb) return;
    game = read_ptr((void*)ga(VA_GAME));
    world = game ? read_ptr((char*)game + 0x40) : NULL;
    if (!world || world != g_paWorld) return;
    rt = (char*)read_ptr((void*)ga(VA_RENDER_THREAD));
    if (g_ptMode != -1 || read_ptr((char*)game + 0xE8) || read_ptr((char*)world + 0x30C) || g_koDeferOcc ||
        g_krPending || (rt && read_ptr(rt + OFF_RT_LOAD_MOVIE)))
        return;
    g_paDropPending = 1;
    g_paDropTick = now;
    g_koOccTick = now | 1;   // the switch's spinner is hidden as for the occluders (hook_SpinnerExec)
    logf_("preload: only %lu MB of address space free (under %d) - unloading the %ld preloaded area layers; those "
          "areas load on their first visit again", freeMb, cfg_area_memory_mb, (LONG)g_paSetCount);
    ko_part_request(game, (char*)world + 0x310, 4, (void*)ga(VA_EMPTY_STRING));
}

// Viewport tick, once per frame on the game thread.
static void ko_frame(void)
{
    void *game, *world, *player;
    float pos[3], dx, dy, dz;
    DWORD waited;
    int i, away = 0;
    g_koFrameTick = GetTickCount();
    cq_poll(1);
    lscr_frame();
    kr_frame();
    pa_frame();
    if (!g_koDeferOcc) return;
    waited = GetTickCount() - g_koDeferSince;
    if (waited < KO_OCC_MIN_MS) return;
    game = read_ptr((void*)ga(VA_GAME));
    world = game ? read_ptr((char*)game + 0x40) : NULL;
    player = game ? read_ptr((char*)game + OFF_GAME_PLAYER) : NULL;
    if (!world) return;
    if (player && g_koDoorPosOk) {
        for (i = 0; i < 3 && read_f32((char*)player + OFF_NODE_POS + 4 * i, &pos[i]); i++) {}
        if (i == 3) {
            dx = pos[0] - g_koDoorPos[0]; dy = pos[1] - g_koDoorPos[1]; dz = pos[2] - g_koDoorPos[2];
            away = dx * dx + dy * dy + dz * dz >= KO_OCC_DIST * KO_OCC_DIST;
        }
    }
    if (!away && waited < KO_OCC_MAX_MS) return;
    // A request already queued (another door) brings the occluders back itself.
    g_koDeferOcc = 0;
    if (!g_partRequest || read_ptr((char*)game + 0xE8) || read_ptr((char*)world + 0x30C)) return;
    logf_("doors: Geralt is %s - loading the occluders (%lu ms after the door)",
          away ? "through the door" : "still near the door, waited long enough", waited);
    g_koOccTick = GetTickCount() | 1;   // its job's spinner is hidden too (hook_SpinnerExec)
    ko_part_request(game, (char*)world + 0x310, 4, (void*)ga(VA_EMPTY_STRING));
}

// The job's lists are pointed at our own arrays for the run (its own arrays are left untouched
// and put back after it). Game thread only.
#define KO_LIST 4096
static void* g_koUnloadOut[KO_LIST];
static int   g_koPreloaded;   // preload_interiors ran on the current area
static void* g_koLoadOut[5][KO_LIST];

// GOG's run (00AE5FCA) takes the job in EDI, with no stack arguments and a plain RET (its call
// site and the async tick load EDI first); Steam's is stdcall (job), RET 4. EDI is kept.
#pragma warning(push)
#pragma warning(disable: 4100)   // the argument is read by the asm
__declspec(naked) static void __stdcall part_run_orig_gog(void* job)
{
    __asm {
        push edi
        mov edi, [esp + 8]
        call dword ptr [g_partRun]
        pop edi
        ret 4
    }
}
#pragma warning(pop)

static void __stdcall hook_PartRun(void* job)
{
    LONGLONG t0 = qpc_us();
    void** unloadData = (void**)read_ptr((char*)job + 0x0C);
    LONG unloadN = (LONG)(uintptr_t)read_ptr((char*)job + 0x10);
    void** buckets = (void**)read_ptr((char*)job + 0x1C);
    LONG nb = (LONG)(uintptr_t)read_ptr((char*)job + 0x20);
    void* savedData[5];
    LONG savedN[5];
    LONG kept = 0, unloaded = 0, skipped = 0, loaded = 0, added = 0, deferred = 0, i, b;
    int on = 0, area = 0, mem = 0, filter, coarseSkip = 0, rel = 0, relN = 0, relSkip = 0, relAct = KR_UNLOAD;
    int dropFake, fakeN = 0, fakeLast = 0, fakeKeep = -1, noInteriors = 0, memTier = g_koMemLow;
    int relLight = 0, areaDoor = 0;
    char relNames[400];
    size_t relUsed = 0;
    LONGLONG tRun;
    void* world = read_ptr((char*)job);
    void** kmBase = NULL;
    void* fakeHold[KM_FAKE];
    DWORD kmN = 0, paN = 0;
    void** paBase = NULL;
    int paRel = 0, paDropN = 0, paShown = 0, paAreas = 0;
    if (cfg_load_trace) pt_job("sync", job);
    g_ptGcSkipped = 0;
    g_ptGcUs = 0;
    if (nb < 0 || nb > 5 || unloadN < 0 || unloadN > KO_LIST || (unloadN && !unloadData) || (nb && !buckets)) nb = -1;
    if (nb >= 0) {
        // A door left to the game for memory (g_koMemLow, hook_DoorStart) runs the game's job:
        // it unloads the outdoors and the other interiors, and its GC runs.
        on = cfg_keep_outdoors && !g_koMemLow && ko_allowed(job);
        area = cfg_preload_interiors && (g_ptMode == 1 || (g_ptMode == 4 && !ko_same_folder(g_ptFrom, g_ptTo)));
        // Only its occluders wait, as on a kept door, so its picture may come back as soon as
        // the job is done (fast_door_tick): loaded with the job, they culled the town in front
        // of Geralt while he was still in the doorway.
        mem = g_koMemLow && g_ptMode == 4 && g_partRequest && ko_frames_alive() && _wcsicmp(g_ptFrom, g_ptTo);
        // release_kept: only what this job lists for unloading stays recorded; the queued switch
        // to the current partition (kr_frame) is the release, the hide or the show.
        if (g_krCount && world != g_krWorld) kr_clear("another world");
        rel = g_krPending && g_ptMode == 4 && !_wcsicmp(g_ptFrom, g_ptTo);
        if (g_krCount) {
            int before = g_krCount;
            kr_keep_listed((void* const*)unloadData, unloadN);
            if (rel) relSkip = before - g_krCount;
        }
        if (rel) {
            g_krPending = 0;
            relAct = g_krAction;
        }
        // preload_areas: a hidden layer this job does not list for unloading belongs to the
        // partition it switches to and joins the world in its attach stage. The queued switch
        // to the current partition (pa_frame) is the one that gives their memory back.
        if (g_paSetCount && world != g_paWorld) pa_clear("another world");
        paRel = g_paDropPending && g_ptMode == 4 && !_wcsicmp(g_ptFrom, g_ptTo);
        if (paRel) g_paDropPending = 0;
        if (g_paSetCount) paShown = pa_keep_listed((void* const*)unloadData, unloadN);
    }
    filter = nb >= 0 && (on || mem || rel || (cfg_preload_interiors && g_koMemLow != 1));
    if (area && !g_layerHooked && cfg_preload_memory_mb &&
        ko_free_mb() < (DWORD)(cfg_preload_memory_mb + KM_COARSE_MB))
        coarseSkip = 1;
    if (area && g_ptMode == 1) g_kpSetCount = g_kpFakeSeen = 0;
    if (nb >= 0 && !cfg_preload_interiors && (g_ptMode == 1 || (g_ptMode == 4 && !ko_same_folder(g_ptFrom, g_ptTo))))
        g_koPairCount = g_koSlowCount = 0;
    if (area) ko_count("before the switch", world);
    // A memory door unloads the other interiors: the fake rooms are needed again.
    if (nb >= 0 && g_koMemLow == 1 && g_ptMode == 4 && cfg_preload_interiors) g_kpFakeKeep = 1;
    // Fake rooms stay out while every interior is loaded (preload), or once a house has been
    // visited (keep_interiors alone). An area load decides it for itself below.
    dropFake = g_koMemLow != 1 && (cfg_preload_interiors ? g_koPreloaded && !g_kpFakeKeep : on && cfg_keep_interiors);
    if (filter) {
        // Unload: a kept door unloads only what each partition swaps; preload keeps interiors.
        // A door between two whole areas also swaps their environments (LC_ENV).
        areaDoor = on && _wcsicmp(g_ptFrom, g_ptTo) && !ko_house_door(world, g_ptFrom, g_ptTo);
        for (i = 0; i < unloadN; i++) {
            void* l = unloadData[i];
            int c = ko_class(l);
            int go = on ? (c == LC_FAKE || c == LC_SWAP || c == LC_LOWPOLY || (c == LC_ENV && areaDoor))
                        : g_koMemLow == 1 || !(cfg_preload_interiors && (c == LC_INTERIOR || kp_in_set(l)));
            if (paRel && pa_in_set(l)) { go = 1; paDropN++; }
            if (!go && kq_marker(l)) go = 1;   // a quest marker of a building that is not the target
            if (c == LC_FAKE) g_kpFakeSeen = 1;
            if (go) g_koUnloadOut[unloaded++] = l; else kept++;
        }
        // The list now holds only what the job itself lists for unloading: the switch diffs the
        // loaded layers against the partition (run of 2026-11-11: the release job listed the 112
        // kept layers), so nothing of the partition Geralt is in is touched (kr_keep_listed).
        if (rel && relAct == KR_UNLOAD && g_krCount && !g_krFull && unloaded + g_krCount <= KO_LIST) {
            g_khCount = 0;   // unloaded now; the attach stage skips unloaded layers
            for (i = 0; i < g_krCount; i++)
                if (kr_find(g_koUnloadOut, unloaded, g_krList[i]) < 0) g_koUnloadOut[unloaded++] = g_krList[i];
            relN = g_krCount;
        } else if (rel && relAct == KR_HIDE && g_krCount && !g_krFull) {
            // The job's attach stage (FUN_00b05a52) detaches them through kh_pred_stub. Light
            // layers (kr_is_light: terrain tiles, a few large meshes) stay in the world.
            int h = 0;
            relNames[0] = 0;
            for (i = 0; i < g_krCount; i++) {
                if (!kr_is_light(g_krList[i])) { g_khSet[h++] = g_krList[i]; continue; }
                relLight++;
                if (relUsed < sizeof relNames - 1) {
                    wchar_t nm[64];
                    relUsed += (size_t)_snprintf_s(relNames + relUsed, sizeof relNames - relUsed, _TRUNCATE, " %ls",
                                                   pt_text((char*)g_krList[i] + 0x78, nm, 64));
                }
            }
            qsort(g_khSet, (size_t)h, sizeof(void*), kp_cmp);
            g_khCount = h;
            relN = h;
            if (!h) g_krCount = 0;   // nothing heavy was kept: nothing to show again
        } else if (rel && relAct == KR_SHOW) {
            // ... and attaches them again once they are out of the set.
            relN = (int)g_khCount;
            g_khCount = 0;
        }
        *(void**)((char*)job + 0x0C) = g_koUnloadOut;
        *(LONG*)((char*)job + 0x10) = unloaded;
        // Load: no fake rooms back while interiors are kept, no low-poly stand-ins next to the
        // real objects; an area change also loads every building interior of the world.
        for (b = 0; b < nb; b++) {
            void* bk = read_ptr(buckets + b);
            void** d = bk ? (void**)read_ptr((char*)bk + 0x08) : NULL;
            LONG n = bk ? (LONG)(uintptr_t)read_ptr((char*)bk + 0x0C) : 0, m = 0;
            savedData[b] = d;
            savedN[b] = n;
            if (!bk || n < 0 || n > KO_LIST || (n && !d)) { savedN[b] = -1; continue; }
            for (i = 0; i < n; i++) {
                int c = ko_class(d[i]);
                if (c == LC_FAKE) g_kpFakeSeen = 1;
                if (c == LC_FAKE && area && fakeN < KM_FAKE)
                    fakeHold[fakeN++] = d[i];   // placed with the last bucket, below
                else if ((c == LC_FAKE && dropFake) || (on && c == LC_LOWPOLY))
                    skipped++;
                else if ((on || mem) && g_ptMode == 4 && g_partRequest && ko_frames_alive() &&
                         _wcsicmp(g_ptFrom, g_ptTo) && ko_is_occluder(d[i]))
                    deferred++;
                else
                    g_koLoadOut[b][m++] = d[i];
            }
            if (area && b == nb - 1 && coarseSkip) {
                logf_("preload: only %lu MB of address space free before the load - nothing preloaded "
                      "(interiors load on their first visit)", ko_free_mb());
                g_koPreloaded = 1;
                fakeKeep = 1;
            } else if (area && b == nb - 1 && m + fakeN <= KO_LIST) {
                kp_collect_t t;
                DWORD tree, houses, known;
                void* root = world ? read_ptr((char*)world + 0x50) : NULL;
                memset(&t, 0, sizeof t);
                t.out = &g_koLoadOut[b][m];
                t.max = (DWORD)(KO_LIST - m - fakeN);
                if (kp_interior_groups(world, g_ptTo) && g_kpGroups) kp_walk(root, 0, &t);
                tree = t.added;
                // Vergen's houses are neither LT_INDOOR nor in a folder called `interior`.
                t.exact = 1;
                g_kqLeft = 0;
                g_kqUsed = 0;
                g_kqNames[0] = 0;
                if (kx_groups(world) && g_kxGroups) kp_walk(root, 0, &t);
                if (g_kqLeft)
                    logf_("preload: %lu quest marker layer(s) inside buildings are left to their doors (a quest "
                          "tells from them where Geralt is):%s", g_kqLeft, g_kqNames);
                houses = t.added - tree;
                {
                    // Act 1's houses are not marked LT_INDOOR (run 15: 0 indoor siblings for
                    // Binduga and Flotsam), so the folder names add theirs.
                    ko_tree f;
                    memset(&f, 0, sizeof f);
                    f.out = t.out + t.added;
                    f.max = t.max - t.added;
                    ko_walk(root, 0, &f);
                    t.added += f.added;
                    known = t.known + f.interiors;
                    logf_("preload: %lu interior layers from the indoor partitions, %lu from the buildings with their "
                          "own lighting, %lu more by folder name (%lu known in this world)", tree, houses, f.added, known);
                }
                // Fake rooms: out when every interior is loaded or loads now; with the memory
                // guard watching, at the end of the load, where they still load if it skips a
                // building. A world with no interior the mod knows keeps its fake rooms.
                noInteriors = known == 0;
                fakeKeep = noInteriors;
                fakeLast = known && t.added && g_layerHooked && cfg_preload_memory_mb;
                // A save (or a script's LoadWorldPartition) inside a house: its street's own
                // layers were not part of the load, so the first way out loaded them on the
                // spot without a fade (Flotsam inn: 49 layers, 391 ms). Load them now, without
                // what a door swaps; the loader skips a layer listed twice (FUN_00a91600, +0x38).
                if (g_ptMode == 1 && cfg_preload_street) {
                    void* street = ks_street(world, g_ptTo);
                    if (street) {
                        ks_walk_t s;
                        wchar_t nm[96];
                        memset(&s, 0, sizeof s);
                        s.out = t.out + t.added;
                        s.max = t.max - t.added;
                        ks_take_groups(street);
                        ks_walk(world ? read_ptr((char*)world + 0x50) : NULL, 0, &s);
                        t.added += s.added;
                        logf_("preload: '%ls' is inside '%ls' - its %lu layers load now too (%d groups of its own)",
                              g_ptTo, pt_text((char*)street + 0x04, nm, 96), s.added, g_ksGroups);
                    }
                }
                // preload_areas: the small areas known to be behind this area's doors come after
                // the interiors and the street, and stay hidden until their door (pa_plan).
                if (cfg_preload_areas && g_khPred) {
                    DWORD k, w = 0;
                    LONG bb, j;
                    paBase = t.out + t.added;
                    paN = pa_plan(world, g_ptTo, paBase, t.max - t.added, &paAreas);
                    // A layer the job lists itself, an interior or a street layer added above is
                    // not a neighbour's alone: it loads as before, shown.
                    for (k = 0; k < paN; k++) {
                        int own = kp_in_set(paBase[k]) || ko_class(paBase[k]) == LC_INTERIOR;
                        for (j = 0; j < (LONG)t.added && !own; j++) own = t.out[j] == paBase[k];
                        for (bb = 0; bb <= b && !own; bb++)
                            for (j = 0; j < savedN[bb] && !own; j++) own = ((void**)savedData[bb])[j] == paBase[k];
                        if (!own) paBase[w++] = paBase[k];
                    }
                    paN = w;
                    pa_set_add(paBase, paN, world);
                    g_paLoadLoaded = g_paLoadSkipped = 0;
                    t.added += paN;
                }
                added = (LONG)t.added;
                kmBase = t.out;
                kmN = t.added;
                g_koPreloaded = 1;
                if (cfg_load_trace) pt_layers("preload", t.out, t.added, PT_MAX_NAMES - 400);
                m += added;
            }
            if (area && b == nb - 1) {
                LONG k;
                if (fakeKeep != 1 && !fakeLast) skipped += fakeN;
                else for (k = 0; k < fakeN && m < KO_LIST; k++) g_koLoadOut[b][m++] = fakeHold[k];
            }
            loaded += m;
            *(void***)((char*)bk + 0x08) = g_koLoadOut[b];
            *(LONG*)((char*)bk + 0x0C) = m;
        }
    }
    // A release frees what it unloaded at once: the GC runs.
    g_koSkipGc = on && cfg_skip_door_gc && !(relN && relAct == KR_UNLOAD) && !paDropN;
    // Preloaded layers come last in the job and load only while the address space allows.
    if (kmN && g_layerHooked && cfg_preload_memory_mb) {
        km_watch((void* const**)savedData, savedN, nb, kmBase, kmN);
        g_kmFakeCount = fakeLast ? fakeN : 0;
        if (g_kmFakeCount) memcpy(g_kmFake, fakeHold, (size_t)fakeN * sizeof(void*));
        g_kmActive = 1;
    }
    tRun = qpc_us();
    if (g_build == BUILD_GOG) part_run_orig_gog(job);
    else ((PartRun_t)g_partRun)(job);
    tRun = qpc_us() - tRun;
    // preload_areas: what this job unloaded is hidden no longer; an area job's doors are read
    // for the next load of this area.
    if (paN) {
        DWORD k, got = 0;
        for (k = 0; k < paN; k++) got += read_ptr((char*)paBase[k] + 0x38) != NULL;
        logf_("preload: %lu of %lu layers of %d area(s) behind the doors loaded, hidden until their door "
              "(%lu held back: less than %d MB free)", got, paN, paAreas, paN - got,
              cfg_area_memory_mb > cfg_preload_memory_mb ? cfg_area_memory_mb : cfg_preload_memory_mb);
    }
    if (g_paSetCount || paDropN) {
        int left = pa_prune();
        if (paShown)
            logf_("preload: %d preloaded layer(s) of '%ls' joined the world at this switch (%d stay hidden)",
                  paShown, g_ptTo, left);
        if (paRel)
            logf_("preload: %d preloaded area layers unloaded (job %lu ms, GC %lu ms, %lu MB free now)", paDropN,
                  (DWORD)(tRun / 1000), (DWORD)(g_ptGcUs / 1000), ko_free_mb());
    }
    if (cfg_preload_areas && area && nb >= 0 && (LONG)(uintptr_t)read_ptr((char*)job + 0x04) == 5)
        pa_learn(world, g_ptTo);
    g_koSkipGc = 0;
    if (g_kmActive) {
        g_kmActive = 0;
        if (g_kmFakeCount && !g_kmSkipped) { loaded -= fakeN; skipped += fakeN; }
        g_kmFakeCount = 0;
        logf_("preload: %lu of %d preloaded layers loaded, %lu left for their first visit (free address space "
              "at the lowest %lu MB, floor %d MB)", g_kmLoaded, g_kmCount - (int)paN, g_kmSkipped,
              g_kmFreeMin == 0xFFFFFFFFu ? ko_free_mb() : g_kmFreeMin, cfg_preload_memory_mb);
        if (g_kmSkipped) fakeKeep = 1;
    }
    // The outcome of an area load for the fake rooms, until the next one.
    if (area && fakeKeep >= 0) {
        if (fakeN || (g_kpFakeSeen && fakeKeep != g_kpFakeKeep))
            logf_("preload: the fake rooms behind doors and windows %s", !fakeKeep
                  ? "are left out - every interior is loaded"
                  : noInteriors ? "stay - no interior found to preload in this world"
                  : "stay in use - not every interior is loaded (doors keep their fade)");
        g_kpFakeKeep = fakeKeep;
    }
    g_koMemLow = 0;
    // Its occluders wait: the memory door's picture may come back early (fast_door_tick).
    if (mem) g_koMemLowTask = NULL;
    if (filter) {
        *(void**)((char*)job + 0x0C) = unloadData;
        *(LONG*)((char*)job + 0x10) = unloadN;
        for (b = 0; b < nb; b++) {
            void* bk = read_ptr(buckets + b);
            if (!bk || savedN[b] < 0) continue;
            *(void**)((char*)bk + 0x08) = savedData[b];
            *(LONG*)((char*)bk + 0x0C) = savedN[b];
        }
    }
    if (on || area || mem || cfg_load_trace)
        logf_("partition: sync job ran %lu ms (GC %s %lu ms)%s", (DWORD)((qpc_us() - t0) / 1000),
              g_ptGcSkipped ? "skipped," : "", (DWORD)(g_ptGcUs / 1000),
              on ? " - keep_outdoors: outdoor layers left loaded" : "");
    if (filter)
        logf_("partition: %s kept %ld of %ld layers, unloaded %ld; loaded %ld (%ld interiors added), skipped %ld (fake rooms, low-poly)%s",
              on ? "keep_outdoors" : rel ? "release" : memTier == 2 ? "area door" : mem ? "memory door" : "preload", kept, unloadN, unloaded,
              loaded, added, skipped,
              deferred ? ", occluders wait until Geralt is through the door" : "");
    if (deferred) ko_defer_occluders();
    if (area) ko_count("after the switch", world);
    if (rel && relAct == KR_HIDE) {
        logf_("interiors: hid %d kept outdoor layers %.0f m from the door (job %lu ms, %lu MB free%s); shown again "
              "within %d m of it or at the next switch", relN, g_krDist, (DWORD)(tRun / 1000), ko_free_mb(),
              relSkip ? ", some recorded layers belong to this partition and stay" : "", cfg_release_kept / 2);
        if (relLight)
            logf_("interiors: %d light layers (at most %d plain entities each: terrain, single meshes) stay in the "
                  "world:%s", relLight, KR_LIGHT_ENTS, relNames);
    } else if (rel && relAct == KR_SHOW) {
        logf_("interiors: showed %d hidden layers again, Geralt %.0f m from the door (job %lu ms)", relN, g_krDist,
              (DWORD)(tRun / 1000));
    } else if (rel) {
        if (relN) {
            for (i = 0; i < relN && g_krGoneCount < KR_MAX; i++)
                if (kr_find(g_krGone, g_krGoneCount, g_krList[i]) < 0) g_krGone[g_krGoneCount++] = g_krList[i];
            logf_("interiors: released %d kept outdoor layers %.0f m from the door (job %lu ms, GC %lu ms); "
                  "they load again at the door%s", relN, g_krDist, (DWORD)(tRun / 1000), (DWORD)(g_ptGcUs / 1000),
                  relSkip ? " - some recorded layers belong to this partition and stay" : "");
            // The doors learned as loading nothing would now load them: their fade comes back.
            g_koPairCount = 0;
        } else {
            logf_("interiors: the release switch found nothing to release (%d recorded layers belong to this "
                  "partition and stay)", relSkip);
        }
        g_krCount = 0;
    } else if (areaDoor && cfg_release_kept) {
        // Only a door between two whole areas: a house keeps its street in the world, which its
        // windows show and which is 2-50 layers; an area left behind is a cave's whole outdoors.
        kr_record((void* const*)unloadData, unloadN, world);
    }
    // A kept door that loaded nothing will not load anything next time either: no fade then.
    // When it only dropped fake rooms (no sound/occluder swap) the way back loads nothing too.
    if (!rel && on && loaded == 0 && (cfg_keep_interiors || cfg_preload_interiors)) {
        int swaps = 0;
        for (i = 0; i < unloaded; i++) if (ko_class(g_koUnloadOut[i]) != LC_FAKE) swaps = 1;
        ko_pair_add(g_ptFrom, g_ptTo);
        if (!swaps) ko_pair_add(g_ptTo, g_ptFrom);
    }
    // With preload a house door should load nothing but what each place swaps (sound,
    // occluders, the indoor environment) and its townspeople. Anything else is an interior the
    // preload missed: logged always, so a player's log shows which.
    if (!rel && on && cfg_preload_interiors) {
        char line[600];
        size_t used = 0;
        int missed = 0;
        for (b = 0; b < nb; b++) {
            LONG n = savedN[b];
            if (n <= 0) continue;
            for (i = 0; i < n; i++) {
                void* l = ((void**)savedData[b])[i];
                int c = ko_class(l);
                wchar_t name[64], path[260];
                if (c == LC_SWAP || c == LC_LOWPOLY || c == LC_FAKE || c == LC_ENV) continue;
                if (kr_find(g_krGone, g_krGoneCount, l) >= 0) continue;   // released, loads back here
                if (kq_marker(l)) continue;                               // a quest marker: the door's
                pt_text((char*)l + 0x78, name, 64);
                pt_text((char*)l + 0x68, path, 260);
                if (!_wcsnicmp(name, L"comm", 4) || wcsistr(path, L"\\communit")) continue;
                missed++;
                if (used < sizeof line - 1)
                    used += (size_t)_snprintf_s(line + used, sizeof line - used, _TRUNCATE, " %ls", path);
            }
        }
        if (missed)
            logf_("doors: '%ls' -> '%ls' loaded %d layer(s) not preloaded (%lu ms):%s", g_ptFrom, g_ptTo, missed,
                  (DWORD)(tRun / 1000), used ? line : "");
    }
    if (rel || paRel) return;   // not a door: its time says nothing about one
    if (on && tRun / 1000 >= KO_SLOW_MS) {
        ko_pair_put(g_koSlow, &g_koSlowCount, g_ptFrom, g_ptTo);
        logf_("doors: '%ls' -> '%ls' took %lu ms - it keeps its fade", g_ptFrom, g_ptTo, (DWORD)(tRun / 1000));
    } else if (on && ko_pair_in(g_koSlow, g_koSlowCount, g_ptFrom, g_ptTo)) {
        // A first visit can be slow (what it loaded then stays loaded): fast now, no fade again.
        ko_pair_del(g_koSlow, &g_koSlowCount, g_ptFrom, g_ptTo);
        logf_("doors: '%ls' -> '%ls' took %lu ms now - no fade again", g_ptFrom, g_ptTo, (DWORD)(tRun / 1000));
    }
}

// GOG's call site is `mov edi,esi; call run`: the job goes on the stack for hook_PartRun, which
// keeps EDI (callee-saved), as the original does.
__declspec(naked) static void part_run_hook_gog(void)
{
    __asm {
        push edi
        call hook_PartRun
        ret
    }
}

// The first bytes of the run and of the request tell the builds' conventions apart (Steam: run
// stdcall, request with EAX = game; GOG: run with EDI = job, request thiscall).
static const char* pt_run_bytes(void)
{
    return by_build("55 8b ec 83 ec 18 53 8b 5d 08 56 57", "55 8b ec 83 ec 18 53 33 db 83 7f 04");
}

static const char* pt_request_bytes(void)
{
    return by_build("55 8b ec 53 57 8b f8 8b 47 40 33 db", "55 8b ec 53 56 57 8b f9 8b 57 40 33");
}

// The GC's first bytes; its rel32 differs per build.
static const char* pt_gc_bytes(void)
{
    return by_build("55 8b ec e8 df ba 94 ff", "55 8b ec e8 96 82 8f ff");
}

// .text is decrypted by the SteamStub: call once the fingerprints are there.
static int pt_install(void)
{
    static int done;
    int a, b, c;
    uintptr_t runCall = ga(VA_PART_RUN_CALL);
    if (done) return 1;
    if (!code_matches(ga(VA_PART_SWITCH), "55 8b ec 83 ec 50 53 56 57 ff 75 08") ||
        !code_matches(ga(VA_PART_RUN), pt_run_bytes()))
        return 0;
    g_partSwitch = ga(VA_PART_SWITCH);
    g_partRun = ga(VA_PART_RUN);
    // Without it a kept door does not hold its occluders back (ko_frame could not load them).
    g_partRequest = code_matches(ga(VA_PART_REQUEST), pt_request_bytes()) ? ga(VA_PART_REQUEST) : 0;
    if (!g_partRequest) logf_("partition: request function not recognised - kept doors load their occluders at once");
    g_gc = ga(VA_GC);
    a = patch_call(ga(VA_PART_SWITCH_A), g_partSwitch, (void*)hook_PartSwitch);
    b = patch_call(ga(VA_PART_SWITCH_B), g_partSwitch, (void*)hook_PartSwitch);
    if (g_build == BUILD_GOG)
        c = code_matches(runCall - 2, "8b fe e8") && patch_call(runCall, g_partRun, (void*)part_run_hook_gog);
    else
        c = patch_call(runCall, g_partRun, (void*)hook_PartRun);
    if (!code_matches(g_gc, pt_gc_bytes()) || !patch_call(ga(VA_PART_GC_CALL), g_gc, (void*)hook_PartGc))
        logf_("partition: GC call not recognised - skip_door_gc off");
    // hide_kept: the layer visibility predicate's one call (both builds; GOG not run in game).
    if (((cfg_release_kept && cfg_hide_kept) || cfg_preload_areas) && g_partRequest) {
        uintptr_t pred = ga(VA_LAYER_VISIBLE), sync = ga(VA_LAYER_SYNC), call = ga(VA_LAYER_VISIBLE_CALL);
        if (pred && sync && call && code_matches(pred, "80 78 58 00 75 03 32 c0 c3") &&
            code_matches(sync, "53 56 33 db 57 8b f8 38 1d")) {
            g_khPred = pred;
            g_khSync = sync;
            if (!patch_call(call, pred, (void*)kh_pred_stub)) g_khPred = g_khSync = 0;
        }
        logf_("interiors: %s", g_khPred
              ? "layer visibility hooked - loaded layers can be kept out of the world (hide_kept, preload_areas)"
              : "layer visibility not recognised on this build - kept outdoor layers unload instead (hide_kept off), "
                "no areas are preloaded (preload_areas off)");
    }
    logf_("partition: switch calls %s/%s, sync run %s (load_trace=%d keep_outdoors=%d)", a ? "hooked" : "NOT hooked",
          b ? "hooked" : "NOT hooked", c ? "hooked" : "NOT hooked", cfg_load_trace, cfg_keep_outdoors);
    done = 1;
    return 1;
}

// ---- fast doors ---------------------------------------------------------
// [loading] fast_doors=1. A house door is a type 1 streaming-door task (vtable 00EF0410): its
// constructor FUN_00502a7f fades to black, Tick 00502B18 queues the interior switch
// (FUN_00aa7e18 appends a request to the game object's array at +0xE4, count +0xE8) and sets
// state 1, and only the door animation's `fade_out` event (handler 00502B5F, slot 2) fades the
// picture back in and sets state 2. The switch itself takes ~0.05-0.3 s, the event comes a fixed
// ~1.2 s later (docs/fixes/load-times.md). Once the request has been consumed and the world
// reports no pending streaming (world+0x30C == 0, the test the interior task 00502DC0 uses) for
// FD_READY_TICKS door ticks, the hook calls that handler with a synthetic `fade_out` event, so
// the game itself fades in and finishes the task; the real event later finds state 2 and does
// nothing. [loading] door_fade repoints the two fades' `movss xmm0,[00E30444]` (the shared 0.5;
// GOG loads it with `fld`) at a float of ours. Everything runs on the game thread inside the
// door's own OnTimer.
#define VA_DOOR_TELEPORT_VT 0x00EF0410u   // type 1 task: +0x14 state (1 = switch queued)
#define VA_DOOR_FADE_EVENT  0x00502B5Fu   // slot 2: thiscall (event, unused), RET 8
#define VA_CNAME_FROM_STR   0x00B28D76u   // __stdcall (DWORD* out, const wchar_t*), RET 8; __thiscall on GOG
#define VA_STR_FADE_OUT     0x00EF0050u   // L"fade_out", the string the handler names its event by
#define VA_DOOR_FADE_OUT    0x00502ACDu   // movss xmm0,[00E30444] in FUN_00502a7f (GOG: fld)
#define VA_DOOR_FADE_IN     0x00502BD9u   // movss xmm0,[00E30444] in 00502B5F (GOG: fld)
#define VA_DOOR_FADE_LEN    0x00E30444u   // the float 0.5 both fades read (the disp32 above)
#define FD_READY_TICKS      2

typedef void (__fastcall *DoorEvent_t)(void* task, void* edx, const void* event, void* unused);
typedef void (__stdcall *CNameFromStr_t)(DWORD* out, const wchar_t* name);
typedef void (__fastcall *CNameFromStrGog_t)(DWORD* out, void* edx, const wchar_t* name);

static float g_doorFade = 0.5f;

// Steam's 00B28D76 is stdcall (out, name), RET 8; GOG's 00B5E7AC is thiscall with `out` in ECX,
// RET 4. Its first bytes tell them apart; anything else leaves the id 0 (the game's own timing).
static void cname_from_str(DWORD* out, const wchar_t* name)
{
    uintptr_t fn = ga(VA_CNAME_FROM_STR);
    if (!code_matches(fn, by_build("55 8b ec 6a 00 ff 75 0c e8", "55 8b ec 56 6a 00 ff 75 08 8b f1 e8"))) return;
    if (g_build == BUILD_GOG) ((CNameFromStrGog_t)fn)(out, NULL, name);
    else ((CNameFromStr_t)fn)(out, name);
}

static void fast_door_tick(void* task)
{
    static void* s_task;
    static DWORD s_since;
    static int   s_ready, s_done;
    static DWORD s_fadeOut;            // CName id of "fade_out", as the handler computes it
    DWORD event[4] = { 0, 0, 0, 0 };
    void *game, *world;
    DWORD reqs, pending, waited;

    if (!task || read_ptr(task) != (void*)ga(VA_DOOR_TELEPORT_VT)) return;
    // A door left to the game for memory whose occluders could not wait (hook_PartRun clears
    // g_koMemLowTask when they do): the picture comes back on the game's own `fade_out` event,
    // once Geralt is out of the building's block. Brought back as soon as such a job was done,
    // the town showed see-through in front of him (GOG, native D3D9, the inn's harbour door).
    if (task == g_koMemLowTask) return;
    if (task != s_task) { s_task = task; s_since = 0; s_ready = 0; s_done = 0; }
    if (s_done || *(const LONG*)((const char*)task + 0x14) != 1) return;
    if (!s_since) s_since = GetTickCount();
    game = read_ptr((void*)ga(VA_GAME));
    world = game ? read_ptr((char*)game + 0x40) : NULL;
    reqs = game ? (DWORD)(uintptr_t)read_ptr((char*)game + 0xE8) : 1;
    pending = world ? (DWORD)(uintptr_t)read_ptr((char*)world + 0x30C) : 1;
    if (reqs == 0 && pending == 0) s_ready++; else s_ready = 0;
    if (s_ready < FD_READY_TICKS) return;
    s_done = 1;
    waited = GetTickCount() - s_since;
    // The handler's own copy (026ADE10) is only set on the first event it sees; the name pool
    // gives the same id for the same string, so look it up here (game thread).
    if (!s_fadeOut) cname_from_str(&s_fadeOut, (const wchar_t*)ga(VA_STR_FADE_OUT));
    if (!s_fadeOut) {
        logf_("doors: interior ready after %lu ms, event name not found - the game's own timing", waited);
        return;
    }
    event[1] = s_fadeOut;
    ((DoorEvent_t)ga(VA_DOOR_FADE_EVENT))(task, NULL, event, NULL);
    logf_("doors: interior ready after %lu ms - picture back now (state %ld)", waited,
          *(const LONG*)((const char*)task + 0x14));
}

// Streaming door OnTimer: fast doors after the game's tick, then the load trace notes the task
// before and after it. Runs on the game thread; the trace's poll thread reads the g_ltDoor*
// fields and clears g_ltDoorTask/g_ltDoorEnd when done.
static void __fastcall hook_DoorTimer(void* door, void* edx, void* name, float dt)
{
    void* before = *(void* volatile*)((char*)door + OFF_DOOR_TASK);
    void* after;
    g_origDoorTimer(door, edx, name, dt);
    after = *(void* volatile*)((char*)door + OFF_DOOR_TASK);
    if (cfg_fast_doors && after) fast_door_tick(after);
    if (!before || !cfg_load_trace) return;
    if (!g_ltDoorTask && !g_ltDoorEnd) {
        g_ltDoorVt = read_ptr(before);
        g_ltDoorTicks = 0;
        g_ltDoorFaded = 0;
        g_ltDoorStart = GetTickCount();
        g_ltDoorTask = before;
        pt_where("door task starts", door);
    } else if (before != g_ltDoorTask || g_ltDoorEnd) {
        g_ltDoorOverlap++;
        return;
    }
    g_ltDoorTicks++;
    if (g_ltDoorVt == (void*)ga(VA_DOOR_INTERIOR_VT) && after == before &&
        *(const LONG*)((const char*)before + 0x14) == 1)
        g_ltDoorFaded = 1;
    if (after != before) {
        g_ltDoorEnd = GetTickCount();
        pt_where("door task ends", door);
    }
}

// The door slot is in .rdata (not encrypted by the SteamStub): callable from DllMain's threads.
static void door_timer_install(const char* tag)
{
    DWORD old = 0, tmp = 0;
    uintptr_t slot = ga(VA_DOOR_TIMER_SLOT), timer = ga(VA_DOOR_TIMER);
    void* have = slot ? read_ptr((void*)slot) : NULL;
    if (have == (void*)hook_DoorTimer) return;
    if (!slot || !timer || have != (void*)timer) {
        logf_("%s: streaming door slot %08X holds %08X, expected %08X - doors untouched",
              tag, (unsigned)slot, (unsigned)(uintptr_t)have, (unsigned)timer);
        return;
    }
    if (!VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old)) return;
    g_origDoorTimer = (DoorTimer_t)timer;
    if (InterlockedCompareExchangePointer((void* volatile*)slot, (void*)hook_DoorTimer, have) == have)
        logf_("%s: streaming door timer hooked", tag);
    VirtualProtect((void*)slot, 4, old, &tmp);
}

// indoor_outdoors: applying a partition sends its location type to the renderer (FUN_00aa95fa
// -> render command 00E7C3B0 -> renderer+0xA8 = partition+0x28; LT_DEFAULT 0, LT_INDOOR 1,
// LT_OUTDOOR 2, LT_CAVE 3). Each frame's setup (FUN_009c011e) then turns three of the
// frame's draw flags off (+0x2775, +0x2776, +0x278F: terrain, grass and the like) when
// renderer+0xAC is set and the type is LT_INDOOR. With the outdoors kept loaded that makes
// the ground vanish behind Geralt as he walks in (La Valette, run 13), and outside the windows.
// The `cmp dword [eax+0xA8], 1` at 009C022C gets an immediate no type has (0x7F), so indoor
// partitions draw them too. Only this check reads the value this way; light, sound and
// weather come from elsewhere.
#define VA_INDOOR_CMP       0x009C0223u   // cmp byte [eax+0xAC],0 / jz / cmp dword [eax+0xA8],1
#define VA_INDOOR_IMM       0x009C0232u

static int indoor_patch(void)
{
    DWORD old = 0, tmp = 0;
    unsigned char* at = (unsigned char*)ga(VA_INDOOR_IMM);
    if (!at || !range_readable(at, 1)) return 0;
    if (*at == 0x7F) return 1;
    if (!code_matches(ga(VA_INDOOR_CMP), "80 b8 ac 00 00 00 00 74 0d 83 b8 a8 00 00 00 01 75 04")) return 0;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *at = 0x7F;
    VirtualProtect(at, 1, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    return 1;
}

// The fade length load at `at`: Steam `movss xmm0,[VA_DOOR_FADE_LEN]` (F3 0F 10 05 disp32), GOG
// `fld dword [VA_DOOR_FADE_LEN]` (D9 05 disp32; it passes the length on the stack). Returns the
// opcode's length, where the disp32 starts.
static unsigned door_fade_op(const unsigned char** op)
{
    static const unsigned char movss[4] = { 0xF3, 0x0F, 0x10, 0x05 }, fld[2] = { 0xD9, 0x05 };
    if (g_build == BUILD_GOG) { *op = fld; return 2; }
    *op = movss;
    return 4;
}

// With ASLR the loader has relocated the disp32 like any absolute address, so it is compared
// with ga().
static int door_fade_site(uintptr_t at)
{
    const unsigned char* op;
    unsigned n = door_fade_op(&op);
    uintptr_t len = ga(VA_DOOR_FADE_LEN);
    return at && len && range_readable((void*)at, n + 4) && !memcmp((void*)at, op, n) &&
           *(const DWORD*)(at + n) == (DWORD)len;
}

// Repoint one fade length load at g_doorFade. The shared 0.5 itself is never written.
static int door_fade_patch(uintptr_t at)
{
    const unsigned char* op;
    unsigned n = door_fade_op(&op);
    LONG mine = (LONG)(uintptr_t)&g_doorFade;
    DWORD old = 0, tmp = 0;
    if (!at || !range_readable((void*)at, n + 4)) return 0;
    if (*(LONG*)(at + n) == mine) return 1;
    if (!door_fade_site(at)) return 0;
    if (!VirtualProtect((void*)at, n + 4, PAGE_EXECUTE_READWRITE, &old)) return 0;
    InterlockedCompareExchange((volatile LONG*)(at + n), mine, (LONG)ga(VA_DOOR_FADE_LEN));
    VirtualProtect((void*)at, n + 4, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n + 4);
    return *(LONG*)(at + n) == mine;
}

// Door fade per door: the door's start method (vtable slot 00EF02E8 -> FUN_0095dcc4, thiscall
// (SDoorPartition*), RET 4; the partition's name String first) creates the task whose
// constructor reads the fade-out length, so g_doorFade is set here, on the game thread, before
// the task exists; the fade-in later reads the same float. A door whose target partition is in
// another folder than the current one changes area (`village-city-forest\Binduga` ->
// `landing_site\outside\Flotsam`: 195 layers out, 348 in, ~0.8-1.2 s) and gets area_fade;
// houses and the inn stay in their folder and get door_fade.
#define VA_DOOR_START_SLOT  0x00EF02E8u
#define VA_DOOR_START       0x0095DCC4u

typedef int (__fastcall *DoorStart_t)(void* door, void* edx, void* part);
static uintptr_t g_doorStart;   // this build's FUN_0095dcc4, set before its slot is swapped

// instant_doors: a house door both ways of which keep_interiors has already made load nothing
// gets no fade at all. Its fade length is set to the KO_NO_FADE marker; the two render
// commands' execute methods (slot 3 of the fade-out command 00E7C374 -> 009DC316, length at
// +0x0C, and of the fade-in command 00E7C388 -> 009DC2C3, length at +0x08; thiscall, no
// arguments) skip a command carrying it, so the screen never goes black. The door task waits
// for "fade over" (FUN_00aa72de, renderer slot 0x7C): with no fade-out issued the last fade
// is still complete (FUN_009d68e7 keeps its progress at 1.0), so the task goes straight on.
#define VA_FADE_OUT_EXEC_SLOT 0x00E7C380u
#define VA_FADE_OUT_EXEC      0x009DC316u
#define VA_FADE_IN_EXEC_SLOT  0x00E7C394u
#define VA_FADE_IN_EXEC       0x009DC2C3u
#define KO_NO_FADE            0.00123f

typedef void (__fastcall *FadeExec_t)(void* cmd, void* edx);
static int g_fadeExecHooked;
static uintptr_t g_fadeOutExec, g_fadeInExec;   // this build's, set before the slots are swapped

static void __fastcall hook_FadeOutExec(void* cmd, void* edx)
{
    float len = 0;
    if (read_f32((char*)cmd + 0x0C, &len) && len == KO_NO_FADE) return;
    ((FadeExec_t)g_fadeOutExec)(cmd, edx);
}

static void __fastcall hook_FadeInExec(void* cmd, void* edx)
{
    float len = 0;
    if (read_f32((char*)cmd + 0x08, &len) && len == KO_NO_FADE) return;
    ((FadeExec_t)g_fadeInExec)(cmd, edx);
}

static int slot_swap(uintptr_t slot, uintptr_t want, void* hook)
{
    DWORD old = 0, tmp = 0;
    void* have = slot ? read_ptr((void*)slot) : NULL;
    int ok;
    if (have == hook) return 1;
    if (!slot || !want || have != (void*)want || !VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old)) return 0;
    ok = InterlockedCompareExchangePointer((void* volatile*)slot, hook, have) == have;
    VirtualProtect((void*)slot, 4, old, &tmp);
    return ok;
}

static void fade_exec_install(void)
{
    int a, b;
    g_fadeOutExec = ga(VA_FADE_OUT_EXEC);
    g_fadeInExec = ga(VA_FADE_IN_EXEC);
    a = slot_swap(ga(VA_FADE_OUT_EXEC_SLOT), g_fadeOutExec, (void*)hook_FadeOutExec);
    b = slot_swap(ga(VA_FADE_IN_EXEC_SLOT), g_fadeInExec, (void*)hook_FadeInExec);
    g_fadeExecHooked = a && b;
    logf_("doors: fade commands %s/%s - instant_doors %s", a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked",
          g_fadeExecHooked ? "on" : "off");
}

// ---- door lighting and the loading spinner ----------------------------------
// door_light_blend (docs/fixes/door-transitions.md). An interior's lighting is an area
// environment (CAreaEnvironmentComponent; Flotsam's are in environments\interior_city) in a
// layer the door's partition loads or unloads. The environment manager (world+0x2B8) blends
// area environments over their activation/deactivation times, but CDPR set those to 0 for
// most interiors, and every door task ends in FUN_0095db81, which re-tests the areas and then
// calls Stabilize (FUN_00a74fbf, EAX = manager, at 0095DC5E) to finish every blend at once.
// Behind the game's fade nobody saw it; on an instant door the street turned into the
// interior look (no sun, far plane 26-386 m, depth of field) and, leaving, the room into the
// outdoor look, in one frame. For KO_LIGHT_MS after an instant door starts:
//  - every area activation gets at least door_light_blend seconds both ways: FUN_00a75014
//    (stdcall, 9 args, RET 0x24; arg 3 isGlobal, arg 7 -> {priority, activation,
//    deactivation}) has one caller, 00A367CB in FUN_00a36748, whose call goes to a stub;
//  - the manager's active non-global entries (array +0x1BB0, count +0x1BB4, stride 0x1BCC;
//    deactivation +0x08, isGlobal +0x1BC8) get at least that fade-out time when the door
//    starts, so the interior's environment fades out when the door unloads its layer (the
//    entry keeps its own copy of the parameters);
//  - the door's Stabilize call is skipped; the manager's tick (FUN_00a78346) advances the
//    blends and drops finished entries every frame anyway.
// Doors with a fade and area changes keep the game's behaviour.
//
// door_spinner=0: the door task and the partition job show the loading spinner (render
// command vtable 00E7C07C, show byte +0x08, execute slot 3 = 009DC3F1 -> FUN_009d1b60,
// `00_loading.swf`). An instant door has nothing to wait for, so for KO_SPINNER_MS after one
// starts a "show" command is dropped on the render thread; "hide" always runs.
#define VA_ENV_ADD_CALL       0x00A367CBu   // call FUN_00a75014 in FUN_00a36748, its only caller
#define VA_ENV_ADD            0x00A75014u
#define VA_ENV_STAB_CALL      0x0095DC5Eu   // call FUN_00a74fbf in FUN_0095db81 (door re-evaluation)
#define VA_ENV_STAB           0x00A74FBFu
#define VA_SPINNER_EXEC_SLOT  0x00E7C088u   // loading spinner command vtable 00E7C07C, slot 3
#define VA_SPINNER_EXEC       0x009DC3F1u
#define OFF_WORLD_ENVMGR      0x2B8
#define KO_LIGHT_MS           4000
#define KO_SPINNER_MS         2000
#define KO_SPINNER_LOG_MS     10000         // a "show" this long after an instant door is logged
#define KO_MISSING_MAX        16            // more layers than this to load: no instant door
#define KO_MISSING_AREA       4             // the same for a door between two whole areas

typedef void (__fastcall *SpinnerExec_t)(void* cmd, void* edx);
static volatile DWORD g_koInstantTick;   // GetTickCount() | 1 when the last door went without a fade
static uintptr_t g_envAdd, g_envStab, g_spinnerExec;   // this build's originals
static int g_lightHooked, g_spinnerHooked;
static volatile LONG g_spinnerDropped;

static int ko_instant_recent(DWORD ms)
{
    DWORD t = g_koInstantTick;
    return t && GetTickCount() - t < ms;
}

static int ko_light_on(void)
{
    return g_lightHooked && cfg_door_light_blend > 0.0f && ko_instant_recent(KO_LIGHT_MS);
}

static char* ko_env_manager(void)
{
    void* game = read_ptr((void*)ga(VA_GAME));
    void* world = game ? read_ptr((char*)game + 0x40) : NULL;
    return world ? (char*)read_ptr((char*)world + OFF_WORLD_ENVMGR) : NULL;
}

// The manager's entries; NULL when the array does not look right.
static char* ko_env_entries(char* mgr, DWORD* count)
{
    char* e = mgr ? (char*)read_ptr(mgr + 0x1BB0) : NULL;
    DWORD n = mgr ? (DWORD)(uintptr_t)read_ptr(mgr + 0x1BB4) : 0;
    *count = 0;
    if (!e || n > 64 || (n && !range_readable(e, (size_t)n * 0x1BCC))) return NULL;
    *count = n;
    return e;
}

// At an instant door: active non-global environments fade out over at least door_light_blend.
static int ko_light_deactivation(void)
{
    DWORD n, i;
    char* e = ko_env_entries(ko_env_manager(), &n);
    int raised = 0;
    float d;
    for (i = 0; e && i < n; i++) {
        char* en = e + i * 0x1BCC;
        if (en[0x1BC8]) continue;
        if (read_f32(en + 0x08, &d) && d < cfg_door_light_blend && write_f32(en + 0x08, cfg_door_light_blend))
            raised++;
    }
    return raised;
}

// Called from env_add_stub before FUN_00a75014 runs; `times` is the caller's
// {priority, activation, deactivation} on its stack.
static void __stdcall env_add_adjust(DWORD isGlobal, float* times)
{
    float v;
    if (!ko_light_on() || (isGlobal & 0xFF) || !times) return;
    if (read_f32(times + 1, &v) && v < cfg_door_light_blend) write_f32(times + 1, cfg_door_light_blend);
    if (read_f32(times + 2, &v) && v < cfg_door_light_blend) write_f32(times + 2, cfg_door_light_blend);
}

// Replaces `call FUN_00a75014` at 00A367CB: after 3 saved registers the return address is at
// [esp+12] and argument N at [esp+12+4N].
__declspec(naked) static void env_add_stub(void)
{
    __asm {
        push eax
        push ecx
        push edx
        mov eax, [esp + 12 + 4 * 7]
        push eax
        mov eax, [esp + 4 + 12 + 4 * 3]
        push eax
        call env_add_adjust
        pop edx
        pop ecx
        pop eax
        jmp dword ptr [g_envAdd]
    }
}

// The door's Stabilize: skipped (1) while an instant door's lighting blends; logs the blends.
static int __cdecl env_stab_skip(void)
{
    char line[400];
    size_t used = 0;
    DWORD n, i, shown = 0;
    char* e;
    if (!ko_light_on()) return 0;
    e = ko_env_entries(ko_env_manager(), &n);
    for (i = 0; e && i < n && shown < 6; i++) {
        char* en = e + i * 0x1BCC;
        wchar_t nm[48];
        float f = 0, r = 0;
        if (en[0x1BC8]) continue;
        read_f32(en + 0x1BB0, &f);
        read_f32(en + 0x1BB4, &r);
        pt_text(en + 0x1BA0, nm, 48);
        if (used < sizeof line - 1)
            used += (size_t)_snprintf_s(line + used, sizeof line - used, _TRUNCATE, " '%ls' %ld at %.2f (%+.2f/s)",
                                        nm, (LONG)(uintptr_t)read_ptr(en), f, r);
        shown++;
    }
    logf_("doors: lighting blends over %.2f s instead of switching at once -%s", cfg_door_light_blend,
          used ? line : " no area environment active");
    return 1;
}

// Replaces `call FUN_00a74fbf` at 0095DC5E. EAX (the manager) is kept for the original. On GOG
// it replaces the door finish's tail jump to Stabilize 00AC8CDF (thiscall, ECX = the manager,
// plain RET) at 005AF54B: ECX is kept the same way, and the skip's `ret` returns to the door's
// caller as Stabilize would; none of the five callers reads EAX.
__declspec(naked) static void env_stab_stub(void)
{
    __asm {
        push eax
        push ecx
        push edx
        call env_stab_skip
        test eax, eax
        pop edx
        pop ecx
        pop eax
        jnz skip
        jmp dword ptr [g_envStab]
    skip:
        ret
    }
}

static void __fastcall hook_SpinnerExec(void* cmd, void* edx)
{
    unsigned char show = 1;
    __try { show = *((const unsigned char*)cmd + 8); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // The occluders' own switch (ko_frame, seconds after the door) is the DLL's, not the game's:
    // its job shows the spinner as well, which popped up after a door without a fade.
    DWORD occ = g_koOccTick;
    if (show && !cfg_door_spinner &&
        (ko_instant_recent(KO_SPINNER_MS) || (occ && GetTickCount() - occ < KO_SPINNER_MS))) {
        InterlockedIncrement(&g_spinnerDropped);
        return;
    }
    // Passive: a spinner that still shows near a door without a fade (seen once on GOG with
    // 0.12.1, on the way out of a house). Which command it is shows in when it comes: between
    // the two windows above, while the occluders wait, or after them. Render thread.
    if (!cfg_door_spinner) {
        static DWORD door, shown;
        static int lines;
        DWORD inst = g_koInstantTick, now = GetTickCount();
        if (inst != door) { door = inst; shown = 0; lines = 0; }
        if (inst && now - inst < KO_SPINNER_LOG_MS && lines < 8) {
            if (show) {
                char when[64] = "no occluder switch for this door";
                if (g_koDeferOcc) strcpy_s(when, sizeof when, "its occluders still wait");
                else if (occ && (LONG)(occ - inst) >= 0)
                    _snprintf_s(when, sizeof when, _TRUNCATE, "%lu ms after its occluder switch", now - occ);
                shown = now | 1;
                lines++;
                logf_("doors: loading spinner shown %lu ms after a door without a fade (%s) - hidden only "
                      "for %d ms after each", now - inst, when, KO_SPINNER_MS);
            } else if (shown) {
                lines++;
                logf_("doors: loading spinner taken away again %lu ms later", now - shown);
                shown = 0;
            }
        }
    }
    ((SpinnerExec_t)g_spinnerExec)(cmd, edx);
}

// .text is decrypted by the SteamStub: retried from doors_tick until the fingerprints hold.
static int light_install(void)
{
    uintptr_t addCall = ga(VA_ENV_ADD_CALL), stabCall = ga(VA_ENV_STAB_CALL);
    int a, b;
    if (g_lightHooked) return 1;
    // Both from -9: the bytes before hold VA_GAME, relocated on GOG. 80/88 is EAX/ECX = manager.
    if (!addCall || !stabCall ||
        !code_matches(addCall - 9, by_build("51 8d 8e c0 01 00 00 51 50 e8 44 e8 03 00",
                                            "51 8d 8e c0 01 00 00 51 50 e8 8f ff 03 00")) ||
        !code_matches(stabCall - 9, by_build("8b 40 40 8b 80 b8 02 00 00 e8 5c 73 11 00",
                                             "8b 40 40 8b 88 b8 02 00 00 e9 8f 97 51 00")))
        return 0;
    g_envAdd = ga(VA_ENV_ADD);
    g_envStab = ga(VA_ENV_STAB);
    a = patch_call(addCall, g_envAdd, (void*)env_add_stub);
    b = a && patch_rel32(stabCall, g_build == BUILD_GOG ? 0xE9 : 0xE8, g_envStab, (void*)env_stab_stub);
    g_lightHooked = a && b;
    logf_("doors: environment activation %s, door Stabilize %s - door_light_blend %s (%.2f s)",
          a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked", g_lightHooked ? "on" : "off", cfg_door_light_blend);
    return 1;
}

static void spinner_install(void)
{
    g_spinnerExec = ga(VA_SPINNER_EXEC);
    g_spinnerHooked = slot_swap(ga(VA_SPINNER_EXEC_SLOT), g_spinnerExec, (void*)hook_SpinnerExec);
    logf_("doors: loading spinner command %s - no spinner on instant doors%s",
          g_spinnerHooked ? "hooked" : "NOT hooked", g_spinnerHooked ? "" : " not possible");
}

static int __fastcall hook_DoorStart(void* door, void* edx, void* part)
{
    g_koDoorPosOk = read_f32((char*)door + OFF_NODE_POS, &g_koDoorPos[0]) &&
                    read_f32((char*)door + OFF_NODE_POS + 4, &g_koDoorPos[1]) &&
                    read_f32((char*)door + OFF_NODE_POS + 8, &g_koDoorPos[2]);
    wchar_t from[160], to[160];
    void* game = read_ptr((void*)ga(VA_GAME));
    void* world = game ? read_ptr((char*)game + 0x40) : NULL;
    void* streaming = world ? read_ptr((char*)world + 0x30C) : NULL;
    int nf, nt, area;
    pt_text(streaming ? (char*)streaming + 0x2C : world ? (char*)world + 0x310 : NULL, from, 160);
    pt_text(part, to, 160);
    nf = part_folder(from);
    nt = part_folder(to);
    area = from[0] && to[0] && (nf != nt || _wcsnicmp(from, to, nf));
    g_doorFade = area ? cfg_area_fade : cfg_door_fade;
    g_koInstantTick = 0;
    // Little address space left: this door is the game's own (hook_PartRun lets its job unload
    // the outdoors and the other interiors), behind the area fade (docs/fixes/door-transitions.md).
    // Sooner than that, a door between two whole areas stops keeping the one left behind: an
    // area is 16-218 layers that only come back into use if the player returns, and in Loc
    // Muinne every area visited would stay. House doors keep their street down to door_memory_mb.
    g_koMemLow = 0;
    if (!area && cfg_keep_outdoors && (cfg_door_memory_mb || cfg_area_memory_mb)) {
        DWORD freeMb = ko_free_mb();
        if (cfg_door_memory_mb && freeMb < (DWORD)cfg_door_memory_mb) {
            g_koMemLow = 1;
            g_doorFade = cfg_area_fade;
            logf_("doors: '%ls' -> '%ls' with only %lu MB of address space free - the game's door "
                  "(the outdoors unloads), %.2f s fades", from, to, freeMb, g_doorFade);
        } else if (cfg_area_memory_mb && freeMb < (DWORD)cfg_area_memory_mb && _wcsicmp(from, to) &&
                   !ko_house_door(world, from, to)) {
            g_koMemLow = 2;
            g_doorFade = cfg_area_fade;
            logf_("doors: '%ls' -> '%ls' joins two areas with %lu MB of address space free (under %d) - "
                  "the area left behind unloads, interiors stay, %.2f s fades", from, to, freeMb,
                  cfg_area_memory_mb, g_doorFade);
        }
    }
    if (area) logf_("doors: '%ls' -> '%ls' changes area - %.2f s fades", from, to, g_doorFade);
    else if (g_koMemLow) {}
    else if (cfg_instant_doors && g_fadeExecHooked && cfg_keep_outdoors && !ko_pair_in(g_koSlow, g_koSlowCount, from, to) &&
             ((cfg_preload_interiors && g_koPreloaded && !(g_kpFakeKeep && g_kpFakeSeen)) ||
              (cfg_keep_interiors && ko_pair_known(from, to)))) {
        // The door decides before its job exists: count what the target's own groups still
        // have to load (a save inside a house left its street out; 49 layers took 391 ms).
        // A whole area's layers are heavy: the cave near Vergen loaded 13 of them in 89-99 ms on
        // its first visit, a freeze with nothing to hide it once three of its 17 had become the
        // door's own (stand-ins, environment) and the count fell under 16. Such a door keeps the
        // short fade unless next to nothing is left to load (kept, or preload_areas).
        DWORD missing = ks_missing(world, to);
        DWORD most = ko_house_door(world, from, to) ? KO_MISSING_MAX : KO_MISSING_AREA;
        if (missing > most) {
            g_doorFade = cfg_area_fade;
            logf_("doors: '%ls' -> '%ls' still has %lu layers to load - %.2f s fades", from, to, missing, g_doorFade);
        } else {
            int raised;
            g_doorFade = KO_NO_FADE;
            g_koInstantTick = GetTickCount() | 1;
            raised = ko_light_on() ? ko_light_deactivation() : 0;
            logf_("doors: '%ls' -> '%ls' is loaded - no fade (%lu to load%s%s)", from, to, missing,
                  raised ? ", lighting fades out slowly" : "", g_spinnerDropped ? ", spinner hidden" : "");
        }
    }
    {
        int r = ((DoorStart_t)g_doorStart)(door, edx, part);
        g_koMemLowTask = g_koMemLow ? read_ptr((char*)door + OFF_DOOR_TASK) : NULL;
        return r;
    }
}

static void door_start_install(void)
{
    DWORD old = 0, tmp = 0;
    uintptr_t slot = ga(VA_DOOR_START_SLOT), start = ga(VA_DOOR_START);
    void* have = slot ? read_ptr((void*)slot) : NULL;
    if (have == (void*)hook_DoorStart) return;
    if (!slot || !start || have != (void*)start) {
        logf_("doors: door start slot %08X holds %08X, expected %08X - one fade for every door",
              (unsigned)slot, (unsigned)(uintptr_t)have, (unsigned)start);
        return;
    }
    if (!VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old)) return;
    g_doorStart = start;
    if (InterlockedCompareExchangePointer((void* volatile*)slot, (void*)hook_DoorStart, have) == have)
        logf_("doors: door start hooked (door_fade %.2f s, area_fade %.2f s)", cfg_door_fade, cfg_area_fade);
    VirtualProtect((void*)slot, 4, old, &tmp);
}

// Worker tick: the door slots at once, the fade sites once the SteamStub has decrypted .text.
static void doors_tick(void)
{
    static int fades_done, tries, partTries, indoorTries, lightTries, spinnerTried, layerTries;
    if (cfg_preload_interiors && cfg_preload_memory_mb && ga(VA_LAYER_LOAD_CALL) && layerTries >= 0 && layerTries < 1000) {
        if (layer_install()) layerTries = -1;
        else if (++layerTries == 1000)
            logf_("preload: per-layer load not recognised - the preload is skipped below %d MB free instead",
                  cfg_preload_memory_mb + KM_COARSE_MB);
    }
    if (cfg_indoor_outdoors && cfg_keep_outdoors && indoorTries >= 0 && indoorTries < 1000) {
        if (indoor_patch()) { logf_("doors: indoor partitions keep drawing terrain and grass (indoor_outdoors=1)"); indoorTries = -1; }
        else if (++indoorTries == 1000) logf_("doors: indoor draw check not recognised - indoor_outdoors off");
    }
    if ((cfg_keep_outdoors || cfg_preload_interiors) && partTries < 1000 && !pt_install() && ++partTries == 1000)
        logf_("partition: switch not recognised - keep_outdoors off");
    // Both only matter on instant doors (docs/fixes/door-transitions.md).
    if (cfg_instant_doors && cfg_keep_outdoors && (cfg_keep_interiors || cfg_preload_interiors)) {
        if (cfg_door_light_blend > 0.0f && lightTries >= 0 && lightTries < 1000) {
            if (light_install()) lightTries = -1;
            else if (++lightTries == 1000) logf_("doors: environment calls not recognised - door_light_blend off");
        }
        if (!cfg_door_spinner && !spinnerTried) {
            spinnerTried = 1;
            spinner_install();
        }
    }
    if (!cfg_fast_doors && cfg_door_fade == 0.5f && cfg_area_fade == 0.5f) return;
    door_timer_install("doors");
    if (fades_done || tries > 500) return;
    tries++;
    if (cfg_door_fade == 0.5f && cfg_area_fade == 0.5f) { fades_done = 1; return; }
    g_doorFade = cfg_door_fade;
    if (cfg_area_fade != cfg_door_fade || (cfg_instant_doors && (cfg_keep_interiors || cfg_preload_interiors))) door_start_install();
    if (cfg_instant_doors && (cfg_keep_interiors || cfg_preload_interiors) && !g_fadeExecHooked && tries == 1) fade_exec_install();
    if (door_fade_site(ga(VA_DOOR_FADE_OUT)) || door_fade_site(ga(VA_DOOR_FADE_IN))) {
        int a = door_fade_patch(ga(VA_DOOR_FADE_OUT)), b = door_fade_patch(ga(VA_DOOR_FADE_IN));
        fades_done = 1;
        logf_("doors: fade-out %s, fade-in %s, %.2f s, area changes %.2f s (game 0.5 s); fast_doors=%d",
              a ? "patched" : "NOT patched", b ? "patched" : "NOT patched", cfg_door_fade, cfg_area_fade, cfg_fast_doors);
    } else if (tries == 500) {
        logf_("doors: fade sites not recognised - fades left at 0.5 s");
    }
}

// CPU time of every thread of the process, with its start address (to find it in Ghidra).
static int lt_snapshot_cpu(lt_cpu* out, int max)
{
    static NtQueryInformationThread_t qit;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    BOOL ok;
    int n = 0;
    if (!qit) qit = (NtQueryInformationThread_t)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    if (snap == INVALID_HANDLE_VALUE) return 0;
    te.dwSize = sizeof te;
    for (ok = Thread32First(snap, &te); ok && n < max; ok = Thread32Next(snap, &te)) {
        HANDLE th;
        FILETIME c, e, k, u;
        if (te.th32OwnerProcessID != pid) continue;
        th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        out[n].tid = te.th32ThreadID;
        out[n].cpu = 0;
        out[n].start = 0;
        if (GetThreadTimes(th, &c, &e, &k, &u))
            out[n].cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                         (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
        if (qit) qit(th, 9 /* ThreadQuerySetWin32StartAddress */, &out[n].start, sizeof out[n].start, NULL);
        CloseHandle(th);
        n++;
    }
    CloseHandle(snap);
    return n;
}

static double lt_ms(LONGLONG us) { return us / 1000.0; }

// b - a, field by field; presentMaxUs is a maximum, not a counter, and is taken from b.
static void lt_diff(const lt_thread* a, const lt_thread* b, lt_thread* d)
{
    int k;
    memset(d, 0, sizeof *d);
    for (k = 0; k < LT_KINDS; k++) {
        d->reads[k] = b->reads[k] - a->reads[k];
        d->readBytes[k] = b->readBytes[k] - a->readBytes[k];
        d->readUs[k] = b->readUs[k] - a->readUs[k];
    }
    d->readsAsync = b->readsAsync - a->readsAsync;
    d->opens = b->opens - a->opens;           d->openUs = b->openUs - a->openUs;
    d->unpacks = b->unpacks - a->unpacks;     d->unpackIn = b->unpackIn - a->unpackIn;
    d->unpackOut = b->unpackOut - a->unpackOut; d->unpackUs = b->unpackUs - a->unpackUs;
    d->sleeps = b->sleeps - a->sleeps;        d->sleep0 = b->sleep0 - a->sleep0;
    d->sleep1 = b->sleep1 - a->sleep1;        d->sleepUs = b->sleepUs - a->sleepUs;
    d->waits = b->waits - a->waits;           d->waitsZero = b->waitsZero - a->waitsZero;
    d->waitUs = b->waitUs - a->waitUs;
    d->presents = b->presents - a->presents;  d->presentUs = b->presentUs - a->presentUs;
    d->presentMaxUs = b->presentMaxUs;
}

static void lt_sum(const lt_thread* in, lt_thread* s)
{
    int i, k;
    memset(s, 0, sizeof *s);
    for (i = 0; i < LT_THREADS; i++) {
        const lt_thread* t = &in[i];
        if (!t->tid) continue;
        for (k = 0; k < LT_KINDS; k++) {
            s->reads[k] += t->reads[k]; s->readBytes[k] += t->readBytes[k]; s->readUs[k] += t->readUs[k];
        }
        s->readsAsync += t->readsAsync;
        s->opens += t->opens;     s->openUs += t->openUs;
        s->unpacks += t->unpacks; s->unpackIn += t->unpackIn; s->unpackOut += t->unpackOut; s->unpackUs += t->unpackUs;
        s->sleeps += t->sleeps;   s->sleep0 += t->sleep0; s->sleep1 += t->sleep1; s->sleepUs += t->sleepUs;
        s->waits += t->waits;     s->waitsZero += t->waitsZero; s->waitUs += t->waitUs;
        s->presents += t->presents; s->presentUs += t->presentUs;
    }
}

static ULONGLONG lt_process_cpu(void)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

// The counters of one interval as " | ..." fields; returns 0 when nothing was counted.
static int lt_format(char* buf, size_t cap, const lt_thread* d)
{
    size_t n = 0;
    int k, any = 0;
    buf[0] = 0;
#define LT_ADD(...) (n += (size_t)max(0, _snprintf_s(buf + n, cap - n, _TRUNCATE, __VA_ARGS__)))
    for (k = 0; k < LT_KINDS; k++) {
        if (!d->reads[k]) continue;
        any = 1;
        LT_ADD(" | %s read %ldx %.1f MB %.0f ms", k_ltKind[k], d->reads[k],
               d->readBytes[k] / 1048576.0, lt_ms(d->readUs[k]));
    }
    if (d->readsAsync) LT_ADD(" (%ld overlapped)", d->readsAsync);
    if (d->opens) { any = 1; LT_ADD(" | open %ldx %.0f ms", d->opens, lt_ms(d->openUs)); }
    if (d->unpacks) {
        any = 1;
        LT_ADD(" | unpack %ldx %.1f->%.1f MB %.0f ms", d->unpacks, d->unpackIn / 1048576.0,
               d->unpackOut / 1048576.0, lt_ms(d->unpackUs));
    }
    if (d->sleeps) {
        any = 1;
        LT_ADD(" | sleep %ldx (0:%ld 1:%ld) %.0f ms", d->sleeps, d->sleep0, d->sleep1, lt_ms(d->sleepUs));
    }
    if (d->waits) { any = 1; LT_ADD(" | wait %ldx (%ld polls) %.0f ms", d->waits, d->waitsZero, lt_ms(d->waitUs)); }
    if (d->presents) {
        any = 1;
        if (d->presentMaxUs) LT_ADD(" | present %ldx %.0f ms (max %.1f)", d->presents, lt_ms(d->presentUs), lt_ms(d->presentMaxUs));
        else LT_ADD(" | present %ldx %.0f ms", d->presents, lt_ms(d->presentUs));
    }
#undef LT_ADD
    return any;
}

// The device's stored D3DPRESENT_PARAMETERS (window_patch.h) and the VSync setting (+0x11).
static void lt_present_info(const char* when)
{
    const unsigned char* pp = (const unsigned char*)ga(WF_PP);
    uintptr_t rva = ga_rva(RVA_SETTINGS);
    void* settings = rva ? read_ptr((void*)(g_base + rva)) : NULL;
    unsigned vsync = settings ? read_u8((uintptr_t)settings + 0x11) : 0xFF;
    if (!pp || !range_readable(pp, 0x38)) return;
    logf_("load-trace: %s: back buffer %lux%lu windowed=%lu refresh=%lu PresentationInterval=%08lX "
          "(0 default and 1 one wait for vsync, 80000000 immediate); [Rendering] VSync=%u",
          when, *(const DWORD*)pp, *(const DWORD*)(pp + 4), *(const DWORD*)(pp + 0x20),
          *(const DWORD*)(pp + 0x30), *(const DWORD*)(pp + 0x34), vsync);
}

static void lt_window_end(int id, const char* what, DWORD wall, const lt_cpu* c0, int n0, ULONGLONG pcpu0)
{
    static lt_cpu c1[LT_CPU_MAX];
    static lt_thread now[LT_THREADS];
    lt_thread total0, total1, d;
    char line[640];
    int n1, i, j, m;
    ULONGLONG pcpu = lt_process_cpu() - pcpu0;
    ULONG tmin = 0, tmax = 0, tcur = 0;
    NtQueryTimerResolution_t qtr = (NtQueryTimerResolution_t)(void*)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");

    memcpy(now, g_lt, sizeof now);
    n1 = lt_snapshot_cpu(c1, LT_CPU_MAX);
    if (qtr) qtr(&tmin, &tmax, &tcur);
    logf_("load-trace: window %d end: %s %lu ms; process CPU %.0f ms = %.2f cores; timer resolution %.3f ms",
          id, what, wall, pcpu / 10000.0, wall ? pcpu / 10000.0 / wall : 0.0, tcur / 10000.0);
    for (i = 0; i < LT_THREADS; i++) {
        double cpu = -1;
        uintptr_t start = 0;
        if (!now[i].tid) continue;
        lt_diff(&g_ltStart[i], &now[i], &d);
        for (j = 0; j < n1; j++) {
            if (c1[j].tid != (DWORD)now[i].tid) continue;
            start = c1[j].start;
            cpu = c1[j].cpu / 10000.0;                // started inside the window
            for (m = 0; m < n0; m++)
                if (c0[m].tid == c1[j].tid) cpu = (c1[j].cpu - c0[m].cpu) / 10000.0;
        }
        if (!lt_format(line, sizeof line, &d)) continue;
        logf_("load-trace:   tid=%ld%s start=%08X cpu=%.0f ms (%.0f%%)%s", now[i].tid,
              (DWORD)now[i].tid == g_ltMainTid ? " (main)" : "", (unsigned)start, cpu,
              (cpu >= 0 && wall) ? cpu * 100.0 / wall : 0.0, line);
    }
    // Threads with CPU time but no traced call (audio, the d3d9 wrapper, Steam, ...).
    for (j = 0; j < n1; j++) {
        double cpu = c1[j].cpu / 10000.0;
        int counted = 0;
        for (i = 0; i < LT_THREADS; i++) if ((DWORD)now[i].tid == c1[j].tid) counted = 1;
        for (m = 0; m < n0; m++) if (c0[m].tid == c1[j].tid) cpu = (c1[j].cpu - c0[m].cpu) / 10000.0;
        if (!counted && cpu >= 50.0)
            logf_("load-trace:   tid=%lu%s start=%08X cpu=%.0f ms (%.0f%%) | no traced call", c1[j].tid,
                  c1[j].tid == g_ltMainTid ? " (main)" : "", (unsigned)c1[j].start, cpu,
                  wall ? cpu * 100.0 / wall : 0.0);
    }
    lt_sum(g_ltStart, &total0);
    lt_sum(now, &total1);
    lt_diff(&total0, &total1, &d);
    d.presentMaxUs = 0;
    lt_format(line, sizeof line, &d);
    logf_("load-trace: window %d totals%s", id, line);
    lt_present_info("window end");
}

// Started from DllMain while load_trace=1: the filetrace_thread pattern for the import slots,
// then the code site once the SteamStub has decrypted .text and Present once the device exists.
static DWORD WINAPI loadtrace_thread(LPVOID param)
{
    static lt_cpu c0[LT_CPU_MAX];
    static lt_thread tickPrev, cur, d;
    DWORD t0 = GetTickCount(), winStart = 0, screenStart = 0, screenMs = 0, lastTick = 0;
    HMODULE k32 = NULL, pinned;
    void* expect = NULL;
    int n = 0, unpackOn = 0, presentOn = 0, partOn = 0, inWin = 0, windows = 0, n0 = 0;   // inWin: 1 screen, 2 door
    void* lastScreen = NULL;
    unsigned char lastShown = 0;
    ULONGLONG pcpu0 = 0;
    char line[640], what[96];
    (void)param;

    for (;;) {
        if (!k32) k32 = GetModuleHandleA("kernel32.dll");
        if (k32 && !expect) expect = (void*)GetProcAddress(k32, "ReadFile");
        if (expect && imp_value(IMP_READFILE) == expect) break;
        if (!g_running || GetTickCount() - t0 > 120000) {
            logf_("load-trace: ReadFile slot never held the kernel32 export - off");
            return 0;
        }
        Sleep(1);
    }
    // Import slots must not outlive this DLL if a caller releases its handle.
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)(uintptr_t)hook_ReadFile, &pinned)) {
        logf_("load-trace: could not pin module - off");
        return 0;
    }
    n += patch_iat("load-trace", IMP_READFILE, (void*)hook_ReadFile, (void**)&g_origReadFile);
    n += patch_iat("load-trace", IMP_CREATEFILEW, (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
    n += patch_iat("load-trace", IMP_SLEEP, (void*)hook_Sleep, (void**)&g_origSleep);
    n += patch_iat("load-trace", IMP_WAITSINGLE, (void*)hook_WaitSingle, (void**)&g_origWaitSingle);
    n += patch_iat("load-trace", IMP_WAITSINGLEEX, (void*)hook_WaitSingleEx, (void**)&g_origWaitSingleEx);
    logf_("load-trace: hooked after %lu ms (%d/5 import slots), main thread %lu", GetTickCount() - t0, n, g_ltMainTid);
    door_timer_install("load-trace");
    lt_sum(g_lt, &tickPrev);

    while (g_running) {
        void *rt, *movie = NULL, *screen = NULL;
        unsigned char shown = 0;
        DWORD now = GetTickCount();

        // .text is encrypted until the SteamStub reaches the OEP: wait for the fingerprint.
        if (!unpackOn && code_matches(ga(VA_UNPACK), "55 8b ec 8b 4d 08 0f b6 01 c1 e8 05")) {
            g_unpack = ga(VA_UNPACK);
            unpackOn = patch_call(ga(VA_UNPACK_CALL), g_unpack, (void*)hook_Unpack) ? 1 : -1;
            logf_("load-trace: block unpack call %s after %lu ms", unpackOn > 0 ? "hooked" : "NOT hooked", now - t0);
        } else if (!unpackOn && now - t0 > 120000) {
            unpackOn = -1;
            logf_("load-trace: block unpack function never appeared - not timed");
        }
        if (!partOn && pt_install()) partOn = 1;
        else if (!partOn && now - t0 > 120000) {
            partOn = -1;
            logf_("load-trace: partition switch not recognised - partitions not traced");
        }
        if (!presentOn) {
            void* dev = read_ptr((void*)ga(WF_D3D_DEVICE));
            if (dev && range_readable(dev, sizeof(void*))) {
                presentOn = patch_vtable(dev, 17, (void*)hook_Present, (void**)&g_origPresent) ? 1 : -1;
                logf_("load-trace: device %08X Present %s after %lu ms", (unsigned)(uintptr_t)dev,
                      presentOn > 0 ? "hooked" : "NOT hooked", now - t0);
                lt_present_info("device found");
            }
        }

        rt = read_ptr((void*)ga(VA_RENDER_THREAD));
        if (rt) {
            movie = read_ptr((char*)rt + OFF_RT_LOAD_MOVIE);
            screen = read_ptr((char*)rt + OFF_RT_LOAD_SCREEN);
            if (screen) shown = read_u8((uintptr_t)screen + 0x10);
        }
        if (!inWin && windows < LT_MAX_WINDOWS && (movie || (g_ltDoorTask && !g_ltDoorEnd))) {
            inWin = movie ? 1 : 2;
            windows++;
            winStart = screenStart = lastTick = now;
            if (inWin == 2 && now - g_ltDoorStart < 1000) winStart = g_ltDoorStart;
            screenMs = 0;
            memcpy(g_ltStart, g_lt, sizeof g_ltStart);
            lt_sum(g_ltStart, &tickPrev);
            n0 = lt_snapshot_cpu(c0, LT_CPU_MAX);
            pcpu0 = lt_process_cpu();
            if (inWin == 1)
                logf_("load-trace: window %d start (loading screen): render thread %08X movie %08X screen %08X shown=%u (%d threads)",
                      windows, (unsigned)(uintptr_t)rt, (unsigned)(uintptr_t)movie, (unsigned)(uintptr_t)screen,
                      shown, n0);
            else
                logf_("load-trace: window %d start (streaming door): task %08X vtable %08X%s, ticking for %lu ms (%d threads)",
                      windows, (unsigned)(uintptr_t)g_ltDoorTask, (unsigned)(uintptr_t)g_ltDoorVt,
                      g_ltDoorVt == (void*)ga(VA_DOOR_INTERIOR_VT) ? " (interior)" : "",
                      now - g_ltDoorStart, n0);
        } else if ((inWin == 1 && !movie) || (inWin == 2 && g_ltDoorEnd)) {
            if (inWin == 1) {
                if (lastScreen) screenMs += now - screenStart;
                _snprintf_s(what, sizeof what, _TRUNCATE, "loading screen (screen object %lu ms)", screenMs);
                lt_window_end(windows, what, now - winStart, c0, n0, pcpu0);
            } else {
                _snprintf_s(what, sizeof what, _TRUNCATE, "streaming door (%ld ticks%s, %ld overlapping)",
                            g_ltDoorTicks, g_ltDoorFaded ? ", faded to black" : "", g_ltDoorOverlap);
                lt_window_end(windows, what, g_ltDoorEnd - winStart, c0, n0, pcpu0);
                g_ltDoorOverlap = 0;
                g_ltDoorTask = NULL;
                g_ltDoorEnd = 0;
            }
            inWin = 0;
            lt_sum(g_lt, &tickPrev);
            lastTick = now;
        } else if (inWin == 1 && (screen != lastScreen || shown != lastShown)) {
            logf_("load-trace: window %d +%lu ms: screen object %08X shown=%u", windows, now - winStart,
                  (unsigned)(uintptr_t)screen, shown);
            if (screen && !lastScreen) screenStart = now;
            if (!screen && lastScreen) screenMs += now - screenStart;
        }
        if (inWin != 2 && g_ltDoorEnd) {       // a door that ended inside a loading screen
            g_ltDoorTask = NULL;
            g_ltDoorEnd = 0;
        }
        if (now - lastTick >= LT_TICK_MS) {
            int k;
            LONGLONG bytes = 0;
            lt_sum(g_lt, &cur);
            lt_diff(&tickPrev, &cur, &d);
            d.presentMaxUs = 0;
            tickPrev = cur;
            for (k = 0; k < LT_KINDS; k++) bytes += d.readBytes[k];
            if (inWin) {
                if (!lt_format(line, sizeof line, &d)) strcpy_s(line, sizeof line, " | idle");
                logf_("load-trace: window %d +%lu ms%s", windows, now - winStart, line);
            } else if ((bytes >= LT_BURST_BYTES || d.unpackOut >= LT_BURST_BYTES) &&
                       lt_format(line, sizeof line, &d)) {
                logf_("load-trace: streaming burst %lu ms%s", now - lastTick, line);
            }
            lastTick = now;
        }
        lastScreen = screen;
        lastShown = shown;
        Sleep(5);
    }
    return 0;
}

static void save_tick(void)
{
    static int iat_done, steam_done, steam_tries;
    static unsigned char last_pending = 0xFE, last_queued = 0xFE;
    static DWORD mem_tick;
    if (!cfg_save_trace && !cfg_save_thumbs && !cfg_savelist_trace && !cfg_list_leak && !cfg_fast_list) return;
    if (!iat_done) {
        HMODULE pinned;
        iat_done = 1;
        // Import slots must not outlive this DLL if a caller releases its handle.
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                (LPCSTR)(uintptr_t)hook_CreateFileW, &pinned)) {
            logf_("saves: could not pin module - thumbnail fix and trace disabled");
            cfg_save_trace = cfg_save_thumbs = cfg_savelist_trace = cfg_list_leak = cfg_fast_list = 0;
            return;
        }
        if (cfg_save_trace) {
            patch_iat("save-trace", IMP_CREATEFILEW, (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
            patch_iat("save-trace", IMP_DELETEFILEW, (void*)hook_DeleteFileW, (void**)&g_origDeleteFileW);
            patch_iat("save-trace", IMP_COPYFILEW, (void*)hook_CopyFileW, (void**)&g_origCopyFileW);
            patch_iat("save-trace", IMP_D3DX_SAVEFILE, (void*)hook_SaveFile, (void**)&g_origSaveFile);
        }
        if (!patch_iat("save-trace", IMP_D3DX_SAVEMEM, (void*)hook_SaveMem, (void**)&g_origSaveMem) &&
            cfg_save_thumbs) {
            logf_("thumbnail: capture import not found - fix disabled");
            cfg_save_thumbs = 0;
        }
        // GOG: the .sav and the game's own preview are seen as CreateFileW opens for writing.
        if (g_build == BUILD_GOG && cfg_save_thumbs) {
            if (patch_iat("thumbnail", IMP_CREATEFILEW, (void*)hook_CreateFileW, (void**)&g_origCreateFileW))
                logf_("saves: local save folder (%s) - thumbnail fix on", build_name());
            else {
                logf_("thumbnail: CreateFileW not hooked - fix disabled");
                cfg_save_thumbs = 0;
            }
        }
        if (g_build != BUILD_GOG && (cfg_save_thumbs || cfg_savelist_trace || cfg_list_leak)) hook_image_registry();
        if (cfg_savelist_trace || cfg_list_leak || cfg_fast_list) { savelist_install(); mem_tick = GetTickCount(); }
    }
    if (g_imp[IMP_STEAM_RS] && !steam_done && steam_tries < 600) {
        HMODULE api = GetModuleHandleA("steam_api.dll");
        SteamRS_t accessor = api ? (SteamRS_t)GetProcAddress(api, "SteamRemoteStorage") : NULL;
        steam_tries++;
        if (accessor && imp_value(IMP_STEAM_RS) == (void*)accessor) {
            void* rs = accessor();   // NULL until SteamAPI_Init has run
            if (rs) g_remoteStorage = rs;
            if (rs && patch_vtable(rs, 0, (void*)hook_FileWrite, (void**)&g_origFileWrite)) {
                steam_done = 1;
                if (cfg_savelist_trace && !patch_vtable(rs, 1, (void*)hook_FileRead, (void**)&g_origFileRead))
                    logf_("savelist: FileRead not hooked");
                logf_("saves: ISteamRemoteStorage %08X FileWrite hooked; thumbnail fix=%d",
                      (unsigned)(uintptr_t)rs, cfg_save_thumbs);
                if (cfg_save_trace) log_engine_storage();
            }
        } else if (steam_tries == 1) {
            logf_("saves: steam_api accessor missing or IAT mismatch - retrying");
        }
        if (!steam_done && steam_tries == 600)
            logf_("saves: ISteamRemoteStorage never became available - thumbnail fix inactive");
    }
    thumb_tick();
    if (cfg_savelist_trace && GetTickCount() - mem_tick >= 60000) {
        mem_tick = GetTickCount();
        log_mem("periodic");
        if (g_freedTotal) logf_("savelist: leaked preview streams freed so far: %ld", g_freedTotal);
    }
    if (!cfg_save_trace) return;
    unsigned char pending = read_u8(ga(VA_CAPTURE_PENDING)), queued = read_u8(ga(VA_CAPTURE_QUEUED));
    if (pending != last_pending || queued != last_queued) {
        logf_("save-trace: capture flags pending=%u queued=%u", pending, queued);
        last_pending = pending; last_queued = queued;
    }
}

// ---- ray cast guard ----------------------------------------------------
// [stability] raycast_guard=1. Crash of 2026-11-02 18:02:55 (leaving the Flotsam inn through a
// streaming door, docs/fixes/crash-raycast-stale-body.md): a worker thread's broadphase ray cast
// (FUN_00625400, `call edx` after `mov edx,[edx+4]` at 006258DB) handed a stale collidable to
// the ray collector's per-object callback FUN_0084c750 (vtable 00D41A34, slot 1 = 00D41A38; thiscall (handle, index),
// RET 8, returns the hit fraction in ST0). It computes the owner as handle + (signed char)
// handle[5]; the freed handle gave an odd owner (5947BE03) whose transform pointer pointed into
// nvgpucomp32.dll, and `movaps xmm1,[esi+30h]` (0084C7AF) faulted. The callback's own "no hit"
// exits return the collector's current fraction, *(float*)(*(this+0xC)+0x10); the guard returns
// that when the callback raises an access violation, so the ray misses an object that is gone.
// Valid calls are untouched (no extra checks on the normal path).
#define VA_RAY_SLOT  0x00D41A38u
#define VA_RAY_HIT   0x0084C750u

typedef float (__fastcall *RayHit_t)(void* self, void* edx, const void* handle, int index);
static volatile LONG g_rayCaught;
static RayHit_t g_rayHit;   // this build's FUN_0084c750, set before the slot is swapped

static int ray_filter(DWORD code)
{
    return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

static float __fastcall hook_RayHit(void* self, void* edx, const void* handle, int index)
{
    __try {
        return g_rayHit(self, edx, handle, index);
    }
    __except (ray_filter(GetExceptionCode())) {
        LONG n = InterlockedIncrement(&g_rayCaught);
        float miss = 1.0f;
        void* out = read_ptr((char*)self + 0xC);
        if (out) read_f32((char*)out + 0x10, &miss);
        if (n <= 5 || (n & (n - 1)) == 0)
            logf_("stability: ray cast hit a freed physics object (handle %08X, thread %lu) - skipped, %ld so far",
                  (unsigned)(uintptr_t)handle, GetCurrentThreadId(), n);
        return miss;
    }
}

static void ray_guard_tick(void)
{
    static int done;
    DWORD old = 0, tmp = 0;
    uintptr_t slot = ga(VA_RAY_SLOT), hit = ga(VA_RAY_HIT);
    void* have;
    if (done || !cfg_ray_guard) return;
    done = 1;
    have = slot ? read_ptr((void*)slot) : NULL;
    if (!slot || !hit || have != (void*)hit) {
        logf_("stability: ray collector slot %08X holds %08X, expected %08X - guard off",
              (unsigned)slot, (unsigned)(uintptr_t)have, (unsigned)hit);
        return;
    }
    if (!VirtualProtect((void*)slot, 4, PAGE_READWRITE, &old)) return;
    g_rayHit = (RayHit_t)hit;
    InterlockedCompareExchangePointer((void* volatile*)slot, (void*)hook_RayHit, have);
    VirtualProtect((void*)slot, 4, old, &tmp);
    logf_("stability: ray cast guard installed (collector slot %08X)", (unsigned)slot);
}

// ---- quit guard ---------------------------------------------------------
// [stability] quit_guard=1. Quitting with an area loaded (the pause menu's Quit calls
// QuitApplication, 009A25AE -> 00AA5F3A, which only sets the engine's exit flag) tears the world
// down while entities still get their interaction-deactivated script event (FUN_00ad84cb ->
// FUN_005167ea). A handler that calls FactsAdd there crashed the game: the native (FUN_009986bd,
// registered as "FactsAdd" in FUN_00998ae9) reads the default of its time argument from
// *(*([01404950]+0x90)+8), and +0x90 is NULL by then (00998724, read of 0x8; three dumps on
// 2026-11-04). The read becomes a call to facts_time_stub, which returns the same value while
// the pointers are there and 0 when one is NULL. Only EAX and the flags change, as before; the
// code after it reads no flags. See docs/fixes/quit-crash.md.
#define VA_FACTS_TIME 0x0099871Eu   // mov eax,[eax+90h]; mov eax,[eax+8] (9 bytes), EAX = game

static volatile LONG g_factsNoWorld;   // times FactsAdd ran without a world

__declspec(naked) static void facts_time_stub(void)
{
    __asm {
        test eax, eax
        jz no_world
        mov eax, dword ptr [eax + 0x90]
        test eax, eax
        jz no_world
        mov eax, dword ptr [eax + 8]
        ret
    no_world:
        lock inc dword ptr [g_factsNoWorld]
        xor eax, eax
        ret
    }
}

// The 12 bytes at VA_FACTS_TIME: the 9 replaced ones and the store of the result. GOG keeps the
// time in [ebp-4], Steam in [ebp-8].
static const char* quit_site_bytes(void)
{
    return by_build("8b 80 90 00 00 00 8b 40 08 89 45 f8", "8b 80 90 00 00 00 8b 40 08 89 45 fc");
}

static void quit_guard_tick(void)
{
    const char* const orig = quit_site_bytes();
    static int done, tries;
    unsigned char code[9];
    pb_range range;
    HANDLE peers[256];
    unsigned count = 0;
    DWORD old = 0, tmp = 0;
    uintptr_t at = ga(VA_FACTS_TIME);
    void* site = (void*)at;
    int64_t delta = (int64_t)(uintptr_t)facts_time_stub - (int64_t)(at + 5);

    if (done || !cfg_quit_guard) return;
    if (!code_matches(at, orig)) {
        if (++tries >= 50) {
            done = 1;
            logf_("stability: FactsAdd not recognised at %08X - quit guard off", (unsigned)at);
        }
        return;
    }
    done = 1;
    if (delta < INT32_MIN || delta > INT32_MAX) return;   // the call must be reachable
    code[0] = 0xE8;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    memset(code + 5, 0x90, 4);
    range.start = (const unsigned char*)site;
    range.len = (unsigned)sizeof code;
    if (!pb_stop_peers(peers, &count, &range, 1)) {
        logf_("stability: could not stop the game's threads to patch FactsAdd - quit guard off");
        return;
    }
    if (!code_matches(at, orig) ||
        !VirtualProtect(site, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        pb_resume_peers(peers, count);
        logf_("stability: FactsAdd changed while patching - quit guard off");
        return;
    }
    memcpy(site, code, sizeof code);
    VirtualProtect(site, sizeof code, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof code);
    pb_resume_peers(peers, count);
    logf_("stability: quit guard installed (FactsAdd time default at %08X)", (unsigned)at);
}

// The first catch is logged from the next worker tick, not only in the total at the DLL detach,
// which GOG did not always reach (docs/fixes/gog-build.md, "Open").
static void quit_guard_report(void)
{
    static int seen;
    if (seen || !g_factsNoWorld) return;
    seen = 1;
    logf_("stability: quit guard: FactsAdd ran without a world - time 0 used");
}

// ---- clean quit ---------------------------------------------------------
// [stability] clean_quit=1. QuitApplication's native (Steam 009A25AE, GOG 00504E78) only sets the
// engine's exit flag, byte +0x20 of the engine object (Steam 00AA5F3A reads [0160C6DC] itself;
// GOG 00AFA032 takes it in ECX). With an area loaded (the pause menu's Quit, pause_quit) the
// engine then shuts down with the world still in, and the world goes before its entities and
// cutscenes are done: FactsAdd without a world (quit_guard), and in the prologue's interrogation
// a cutscene actor whose component at +0x130 is already gone (00A517CC reads +0x38 of NULL, from
// the cutscene stop 00A47FDC; three dumps 2026-11-06). The game's own "Exit current game" calls
// theGame.ExitGame() (native 00AA8E6C -> 00AA8945, EAX = game; GOG 00AFCE86 -> 00AFC92C, ECX =
// game; plain RET): it sets game+0x4E and raises RequestGameEnd, which ends the session in order
// and opens the main menu. With an area loaded, the native's call to the exit flag goes to
// quit_stub: it runs that ExitGame and remembers the world. Once the world has been gone for
// CQ_SETTLE_MS the exit flag is set from the next frame on the game thread (the worker when
// there is no viewport tick), as the main menu's own Quit does with no area loaded; after
// CQ_GIVEUP_MS it is set in any case. With no area loaded the flag is set at once, as before.
// See docs/fixes/quit-crash.md.
#define VA_QUIT_CALL    0x009A25B7u   // call 00AA5F3A in the QuitApplication native 009A25AE
#define VA_QUIT_FLAG    0x00AA5F3Au   // [engine]+0x20 = 1 (GOG 00AFA032: ECX = engine)
#define VA_EXIT_GAME    0x00AA8945u   // ExitGame's body: EAX = game (GOG: ECX), plain RET
#define VA_ENGINE       0x0160C6DCu   // the engine object; +0x20 its exit flag
#define CQ_SETTLE_MS    1500
#define CQ_GIVEUP_MS    30000

static uintptr_t g_cqFlag, g_cqExit;
static void* volatile g_cqWorld;       // the world Quit was pressed in; NULL = nothing pending
static volatile DWORD g_cqTick, g_cqGoneTick;
static int g_cqHooked;
static int ko_frames_alive(void);

// ExitGame's body with the game in both EAX (Steam) and ECX (GOG); it keeps EBX, ESI and EDI.
#pragma warning(suppress: 4100)   // `game` is read from the stack
__declspec(naked) static void __stdcall cq_call_exit(void* game)
{
    __asm {
        mov eax, [esp + 4]
        mov ecx, eax
        call dword ptr [g_cqExit]
        ret 4
    }
}

// ---- a loading-screen object the game left behind ------------------------------
// [stability] loading_screen_cleanup=1. docs/fixes/quit-crash.md, "A loading-screen movie
// still alive at the GUI shutdown". Static analysis, Steam 3.5.0.26 (GOG in k_gog):
//  - The game asks for a loading screen through its vtable +0x108 (00AA8B61) and ends it through
//    +0x10C (009B49CF -> 00AA8984). Both count at game+0x68; every "show" posts the render command
//    00E7C054 (viewport, name), and only the "end" that takes the count to 0 posts it with
//    viewport 0.
//  - Its execute slot (00E7C060 -> 009DC408 -> 009D1C46) sets render thread +0x268 = viewport.
//    With a viewport it destroys the screen object at +0x26C and builds a new one from the
//    screen's name (FUN_009c3271: a Scaleform movie, class vtable 00EC6A20); with viewport 0 it
//    only asks that movie to `FadeOut`. The object is freed later, by the frame's overlay pass
//    (FUN_009d1a0e) or the loading loop (FUN_009d1d61), once its "shown" byte (+0x10) is 0, and
//    only the movie itself clears that byte: its fscommand handler (00EC6A8C) calls slot 4.
//  - So a movie that never answers `FadeOut` stays for good: +0x268 is 0, +0x26C still holds
//    the object, and the render thread keeps advancing it. Seen in two sessions on 2026-11-11
//    (`quit: ... LOADING SCREEN UP (movie 00000000, screen 0C210768)`), minutes after the load.
//    What kept those movies from answering is not known. At Quit the GUI shutdown then destroys
//    the movie's heap while its memory context is alive, and can crash (the dump of 23:31).
// The remedy is the game's own: the video loading screen's command (vtable 00E7C068, built by
// FUN_00aa1d2f, executed by FUN_009d1b9f) with no viewport and no movie sets +0x268 = 0, destroys
// the object at +0x26C and clears the pointer - what every new loading screen does to the one
// before it. It is posted from the game thread like any render command (FUN_00aa2685 gets the
// queue memory, the constructor must follow at once, vtable +4 commits), and only when the game
// itself has no loading screen up: count 0, +0x268 0, the same object for LSCR_STALE_MS.
// The execute slot is also wrapped to log every show and end with the object before and after.
#define VA_RCMD_ALLOC       0x00AA2685u   // cdecl (size) -> render command memory; its ctor must follow at once
#define VA_RCMD_LOADMOVIE   0x00AA1D2Fu   // video loading-screen command ctor (vtable 00E7C068): memory in EAX (GOG: ECX), stdcall (viewport, movie), RET 8
#define VA_LSCR_EXEC_SLOT   0x00E7C060u   // loading-screen command vtable 00E7C054, slot 3
#define VA_LSCR_EXEC        0x009DC408u   // ECX = command: +0x08 viewport, +0x0C the screen's name
#define OFF_GAME_LSCR_COUNT 0x68          // loading screens asked for and not ended yet (both builds)
#define LSCR_STALE_MS       4000
#define LSCR_QUIT_MS        3000

typedef void (__fastcall *LscrExec_t)(void* cmd, void* edx);
static uintptr_t g_lscrAlloc, g_lscrCtor, g_lscrExec;
static int g_lscrReady;

// Game thread. 1 = posted. Steam's constructor takes the memory in EAX, GOG's in ECX; neither
// reads the other register first.
__declspec(naked) static int __stdcall lscr_post_clear(void)
{
    __asm {
        push 0x10
        call dword ptr [g_lscrAlloc]
        pop ecx
        test eax, eax
        jz none
        push 0                          ; no movie
        push 0                          ; no viewport
        mov ecx, eax
        call dword ptr [g_lscrCtor]     ; RET 8, EAX = the command
        mov edx, [eax]
        mov ecx, eax
        call dword ptr [edx + 4]        ; commit
        mov eax, 1
        ret
    none:
        xor eax, eax
        ret
    }
}

static int lscr_clear(void)
{
    int ok = 0;
    __try { ok = lscr_post_clear(); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

static int lscr_state(void** movie, void** screen, int* shown)
{
    char* rt = (char*)read_ptr((void*)ga(VA_RENDER_THREAD));
    *movie = rt ? read_ptr(rt + OFF_RT_LOAD_MOVIE) : NULL;
    *screen = rt ? read_ptr(rt + OFF_RT_LOAD_SCREEN) : NULL;
    *shown = -1;
    __try { if (*screen) *shown = *((const unsigned char*)*screen + 0x10); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return rt != NULL;
}

// Render thread: every loading-screen command, with the screen object before and after it.
static void __fastcall hook_LscrExec(void* cmd, void* edx)
{
    void *vp = NULL, *movie, *before, *after;
    wchar_t name[80];
    int i = 0, shown;
    __try {
        const wchar_t* s = (const wchar_t*)((const char*)cmd + 0x0C);
        vp = *(void**)((char*)cmd + 0x08);
        for (; i < 79 && s[i]; i++) name[i] = s[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) { i = 0; }
    name[i] = 0;
    lscr_state(&movie, &before, &shown);
    ((LscrExec_t)g_lscrExec)(cmd, edx);
    lscr_state(&movie, &after, &shown);
    logf_("loading: screen %s on the render thread ('%ls'), screen object %p -> %p", vp ? "shown" : "ended", name,
          before, after);
}

// Once per frame on the game thread (ko_frame).
static void lscr_frame(void)
{
    static DWORD last, since, posted;
    static void* seen;
    DWORD now = GetTickCount();
    char* game;
    void *movie, *screen;
    int shown, ok;
    LONG count = 0;
    if (!cfg_lscr_cleanup || !g_lscrReady || now - last < 500) return;
    last = now;
    game = (char*)read_ptr((void*)ga(VA_GAME));
    if (!game || !lscr_state(&movie, &screen, &shown)) return;
    if (!screen) {
        if (posted) logf_("loading: the left-over loading-screen object is gone (%lu ms after asking)", now - posted);
        posted = 0;
        seen = NULL;
        return;
    }
    __try { count = *(volatile LONG*)(game + OFF_GAME_LSCR_COUNT); } __except (EXCEPTION_EXECUTE_HANDLER) { count = 1; }
    // A loading screen that is up or asked for, a switch in progress, or Quit (lscr_quit_wait).
    if (movie || count > 0 || screen != seen || g_ptMode != -1 || g_cqWorld) {
        // A new loading screen can get the freed object's address (seen 2026-11-12): it is a
        // new object all the same, and may be asked about again.
        if (movie || count > 0 || screen != seen) posted = 0;
        seen = screen;
        since = now;
        return;
    }
    if (posted || now - since < LSCR_STALE_MS) return;
    posted = now | 1;
    ok = lscr_clear();
    logf_("loading: a loading-screen object is still alive %lu s after its loading screen ended (object %p, shown %d) - %s",
          (now - since) / 1000, screen, shown,
          ok ? "asked the render thread to free it" : "could not ask the render thread to free it");
}

// At Quit, on the game thread, while the world is still there (cq_request): a loading screen
// that ended and left its object behind is freed now. On Steam no frame is drawn between the
// end of the session and the exit flag, so there is no later moment on the game thread: a Quit
// 7 s after a load, before lscr_frame's time was up, went into the GUI shutdown with the object
// alive and crashed there (2026-11-12 15:21, the 0077E986 crash of docs/fixes/quit-crash.md).
static void lscr_at_quit(void)
{
    char* game = (char*)read_ptr((void*)ga(VA_GAME));
    void *movie, *screen;
    int shown;
    LONG count = 0;
    if (!cfg_lscr_cleanup || !g_lscrReady || !game || !lscr_state(&movie, &screen, &shown) || movie || !screen) return;
    __try { count = *(volatile LONG*)(game + OFF_GAME_LSCR_COUNT); } __except (EXCEPTION_EXECUTE_HANDLER) { count = 1; }
    if (count > 0) return;
    logf_("quit: the last loading screen left its object behind (%p, shown %d) - %s", screen, shown,
          lscr_clear() ? "asked the render thread to free it" : "could not ask the render thread to free it");
}

// At Quit, when the exit flag is due (cq_poll; the game thread, or the worker when no frame is
// drawn): 1 = wait a little longer. A loading screen that is still up, or an object still there,
// gets LSCR_QUIT_MS to go; on the game thread an object left behind is freed first. The GUI
// shutdown should find no loading-screen movie.
static int lscr_quit_wait(DWORD now, int gameThread)
{
    static DWORD since;
    static int posted;
    void *movie, *screen;
    int shown;
    if (!cfg_lscr_cleanup || !g_lscrReady || !lscr_state(&movie, &screen, &shown)) return 0;
    if (!movie && !screen) {
        if (since) logf_("quit: the loading screen is gone %lu ms later", now - since);
        since = 0;
        return 0;
    }
    if (!since) {
        since = now | 1;
        posted = 0;
        logf_("quit: a loading screen is still %s (viewport %p, object %p, shown %d) - waiting for it before the game closes",
              movie ? "up" : "alive", movie, screen, shown);
        return 1;
    }
    if (now - since >= LSCR_QUIT_MS) {
        logf_("quit: it is still there after %d ms - closing anyway", LSCR_QUIT_MS);
        since = 0;
        return 0;
    }
    if (gameThread && !movie && !posted && now - since >= 500) {
        posted = 1;
        logf_("quit: its loading screen ended but the object stays - %s",
              lscr_clear() ? "asked the render thread to free it" : "could not ask the render thread to free it");
    }
    return 1;
}

// .text is decrypted by the SteamStub: retried from the worker until the fingerprints hold.
static void lscr_tick(void)
{
    static int done, tries;
    uintptr_t alloc = ga(VA_RCMD_ALLOC), ctor = ga(VA_RCMD_LOADMOVIE), exec = ga(VA_LSCR_EXEC);
    int hooked;
    if (done || !cfg_lscr_cleanup) return;
    if (!alloc || !ctor || !exec ||
        !code_matches(alloc, "55 8b ec 51 51 8b 0d") ||
        !code_matches(ctor, by_build("55 8b ec e8 b1 fd ff ff 8b 4d 08 89 48 08 8b 4d",
                                     "55 8b ec 8b c1 e8 7a fd ff ff 8b 4d 08 89 48 08")) ||
        !code_matches(exec, by_build("8b c1 8d 48 0c 8b 40 08 e9 31 58 ff ff",
                                     "8b c1 8d 48 0c 8b 40 08 e9 24 55 ff ff"))) {
        if (++tries == 1000) {
            done = 1;
            logf_("stability: loading-screen commands not recognised - loading_screen_cleanup off");
        }
        return;
    }
    done = 1;
    g_lscrAlloc = alloc;
    g_lscrCtor = ctor;
    g_lscrExec = exec;
    hooked = slot_swap(ga(VA_LSCR_EXEC_SLOT), exec, (void*)hook_LscrExec);
    g_lscrReady = 1;
    logf_("stability: loading screen cleanup ready (a screen object left behind is freed after %d s, and before Quit); "
          "its command is %s", LSCR_STALE_MS / 1000, hooked ? "logged" : "NOT hooked - no `loading: screen` lines");
}

// Passive, for the log line when the exit flag is set: is a loading screen up (render thread
// +0x268 movie, +0x26C screen object)? A loading-screen movie still alive at the GUI shutdown
// crashes in Scaleform's heap teardown (2026-11-11 dump, docs/fixes/quit-crash.md).
static const char* cq_loading_note(void)
{
    static char buf[96];
    char* rt = (char*)read_ptr((void*)ga(VA_RENDER_THREAD));
    void* movie = rt ? read_ptr(rt + OFF_RT_LOAD_MOVIE) : NULL;
    void* screen = rt ? read_ptr(rt + OFF_RT_LOAD_SCREEN) : NULL;
    if (!rt) return "render thread not found";
    if (!movie && !screen) return "no loading screen up";
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "LOADING SCREEN UP (movie %p, screen %p)", movie, screen);
    return buf;
}

// On the game thread, from quit_stub: 1 = the session is ending (ExitGame), 0 = quit at once.
static int __cdecl cq_request(void)
{
    char* game = (char*)read_ptr((void*)ga(VA_GAME));
    void* world = game ? read_ptr(game + 0x40) : NULL;
    unsigned char ending = 0;
    lscr_at_quit();
    if (!world) logf_("quit: no area loaded - quitting at once, %s", cq_loading_note());
    if (!world || !cfg_clean_quit || !g_cqExit) return 0;
    if (g_cqWorld) return 1;   // Quit again while the session ends
    __try {
        ending = (unsigned char)game[0x4E];
        if (!ending) cq_call_exit(game);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        logf_("quit: ExitGame faulted - quitting as the game does");
        return 0;
    }
    g_cqGoneTick = 0;
    g_cqTick = GetTickCount();
    g_cqWorld = world;
    logf_("quit: an area is loaded - ending the game session first%s", ending ? " (it was already ending)" : "");
    return 1;
}

// Replaces the native's `call VA_QUIT_FLAG`. GOG passes the engine in ECX, kept for the original.
__declspec(naked) static void quit_stub(void)
{
    __asm {
        push ecx
        push edx
        call cq_request
        pop edx
        pop ecx
        test eax, eax
        jnz handled
        jmp dword ptr [g_cqFlag]
    handled:
        ret
    }
}

static void cq_set_exit_flag(void)
{
    unsigned char* eng = (unsigned char*)read_ptr((void*)ga(VA_ENGINE));
    __try {
        if (eng && !eng[0x20]) eng[0x20] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// From ko_frame (gameThread 1) and the worker (0, only without the viewport tick).
static void cq_poll(int gameThread)
{
    void* w = g_cqWorld;
    char* game;
    void* cur;
    DWORD now;
    if (!w || (!gameThread && ko_frames_alive())) return;
    game = (char*)read_ptr((void*)ga(VA_GAME));
    cur = game ? read_ptr(game + 0x40) : NULL;
    now = GetTickCount();
    if (cur != w) {
        if (!g_cqGoneTick) {
            g_cqGoneTick = now | 1;
            logf_("quit: the game session ended %lu ms after Quit", now - g_cqTick);
            return;
        }
        if (now - g_cqGoneTick < CQ_SETTLE_MS) return;
        if (lscr_quit_wait(now, gameThread)) return;
        if (InterlockedCompareExchangePointer((void* volatile*)&g_cqWorld, NULL, w) != w) return;
        logf_("quit: quitting from the main menu, %lu ms after Quit, %s", now - g_cqTick, cq_loading_note());
        cq_set_exit_flag();
    } else if (now - g_cqTick >= CQ_GIVEUP_MS) {
        if (InterlockedCompareExchangePointer((void* volatile*)&g_cqWorld, NULL, w) != w) return;
        logf_("quit: the game session did not end within %d s - quitting as the game does, %s",
              CQ_GIVEUP_MS / 1000, cq_loading_note());
        cq_set_exit_flag();
    }
}

// The native's bytes before the call (GOG also loads the engine into ECX, relocated: checked
// against ga()), the flag setter and ExitGame's prologue.
static void clean_quit_tick(void)
{
    static int done, tries;
    uintptr_t at = ga(VA_QUIT_CALL);
    int gog = g_build == BUILD_GOG, ok;
    cq_poll(0);
    if (done || !cfg_clean_quit) return;
    ok = at &&
         code_matches(at - (gog ? 15 : 9), "55 8b ec 8b 45 08 ff 40 18") &&
         (!gog || (code_matches(at - 6, "8b 0d") && read_ptr((void*)(at - 4)) == (void*)ga(VA_ENGINE))) &&
         code_matches(ga(VA_QUIT_FLAG) + (gog ? 0 : 5),
                       by_build("80 78 20 00 75 04 c6 40 20 01 c3", "80 79 20 00 75 04 c6 41 20 01 c3")) &&
         (gog || read_ptr((void*)(ga(VA_QUIT_FLAG) + 1)) == (void*)ga(VA_ENGINE)) &&
         code_matches(ga(VA_EXIT_GAME), by_build("55 8b ec 83 ec 14 53 56 57 33 db 8b f0",
                                                 "55 8b ec 83 ec 10 56 57 33 ff 8b f1"));
    if (!ok) {
        if (++tries >= 100) {
            done = 1;
            logf_("quit: QuitApplication or ExitGame not recognised - clean_quit off");
        }
        return;
    }
    done = 1;
    g_cqFlag = ga(VA_QUIT_FLAG);
    g_cqExit = ga(VA_EXIT_GAME);
    g_cqHooked = patch_call(at, g_cqFlag, (void*)quit_stub);
    logf_("quit: QuitApplication %s - with an area loaded it ends the game session first (clean_quit)",
          g_cqHooked ? "hooked" : "NOT hooked");
}

// ---- pathfinding guard --------------------------------------------------
// [stability] path_guard=1. FUN_006550c0 (GOG 00725FC0, byte-identical: PathEngine is a static
// library) is PathEngine's edge walk around a mesh vertex: cdecl (line, startEdge, out before,
// out after), plain RET; line = 4 ints (x1, y1, x2, y2), an edge = {mesh, index}. It walks the
// edges until the cross product of an edge and the line's direction changes sign. A line whose
// two ends are the same point has no direction: every product is 0 and the first loop
// (00655214..) never ends. Seen 2026-11-06 in a Task Manager dump of a frozen game: a job
// thread in that loop with the line {65C9,5381,65C9,5381}, called from the obstacle crossing
// walk FUN_008bbf70 in a CPathfindingQueryJob, and 53 jobs behind it; the main thread spun in
// the next door's flush (docs/fixes/door-transitions.md). Nine callers use the walk. The
// function's entry goes to path_stub: for a zero-length line it returns at once with both
// outputs set to the start edge, which is what the function returns when its first loop stops
// on the start edge (the second loop then stops at once on a product of 0); any other line
// runs the original through a trampoline with its 8 prologue bytes.
#define VA_PATH_EDGE_WALK 0x006550C0u
#define PATH_PROLOGUE     "55 8b ec 83 ec 28 53 56 8b 75 0c 8b 06 8b 50 14 8b 4e 04 8d 0c 89 8b 4c 8a 04"

static void* g_pathTramp;              // the original: 8 prologue bytes, then jmp site+8
static volatile LONG g_pathFlat;       // zero-length lines answered by the guard

__declspec(naked) static void path_stub(void)
{
    __asm {
        mov eax, [esp + 4]           // line
        mov ecx, [eax]
        cmp ecx, [eax + 8]
        jne original
        mov ecx, [eax + 4]
        cmp ecx, [eax + 12]
        jne original
        mov eax, [esp + 8]           // start edge {mesh, index}
        mov ecx, [esp + 12]          // out before
        mov edx, [eax]
        mov [ecx], edx
        mov edx, [eax + 4]
        mov [ecx + 4], edx
        mov ecx, [esp + 16]          // out after
        mov edx, [eax]
        mov [ecx], edx
        mov edx, [eax + 4]
        mov [ecx + 4], edx
        lock inc dword ptr [g_pathFlat]
        ret
    original:
        jmp dword ptr [g_pathTramp]
    }
}

static void path_guard_tick(void)
{
    static int done, tries;
    static LONG reported;
    unsigned char code[8];
    pb_range range;
    HANDLE peers[256];
    unsigned count = 0;
    DWORD old = 0, tmp = 0;
    uintptr_t at = ga(VA_PATH_EDGE_WALK);
    int64_t delta;
    LONG flat = g_pathFlat;

    if (flat != reported && (!reported || flat - reported >= 10)) {
        logf_("stability: path guard: %ld pathfinding line(s) of zero length answered at once "
              "(the game would have looped for good)", (long)flat);
        reported = flat;
    }
    if (done || !cfg_path_guard) return;
    if (!at || !code_matches(at, PATH_PROLOGUE)) {
        if (++tries >= 100) {
            done = 1;
            logf_("stability: PathEngine edge walk not recognised at %08X - path guard off", (unsigned)at);
        }
        return;
    }
    done = 1;
    delta = (int64_t)(uintptr_t)path_stub - (int64_t)(at + 5);
    if (delta < INT32_MIN || delta > INT32_MAX) return;
    g_pathTramp = wf_make_trampoline((void*)at, (const unsigned char*)at, 8);
    if (!g_pathTramp) { logf_("stability: no memory for the path guard's trampoline - off"); return; }
    code[0] = 0xE9;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    memset(code + 5, 0x90, 3);
    range.start = (const unsigned char*)at;
    range.len = (unsigned)sizeof code;
    if (!pb_stop_peers(peers, &count, &range, 1)) {
        logf_("stability: could not stop the game's threads to patch the PathEngine edge walk - path guard off");
        return;
    }
    if (!code_matches(at, PATH_PROLOGUE) ||
        !VirtualProtect((void*)at, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        pb_resume_peers(peers, count);
        logf_("stability: PathEngine edge walk changed while patching - path guard off");
        return;
    }
    memcpy((void*)at, code, sizeof code);
    VirtualProtect((void*)at, sizeof code, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, sizeof code);
    pb_resume_peers(peers, count);
    logf_("stability: path guard installed (PathEngine edge walk at %08X)", (unsigned)at);
}

// ---- layer storage leak -------------------------------------------------
// [stability] layer_storage_leak=1. Each layer keeps the state of its entities when they detach,
// in a "layerStorage" (save block names): an array of 0x1C-byte records, idTag at +0, the state
// at +0x14 (a CGameDataStorage, 0x14 bytes: vtable, then a TDynArray of the serialized state,
// ~1-4 KB) and spawn data at +0x18. The record destructor (00AFE0A2, EAX = record, plain RET)
// frees +0x18 and never +0x14. It runs when the array is cleared (004461B1, from the save load
// 00AFE9F2 and the storage teardown 00AFE392) and when one spawned entity's record goes
// (00AFE12C). So every save load leaves all stored states of the world behind. Seen 2026-11-05
// after five hours: 132 774 storages, 122 699 of them pointed at by nothing (~370 MB), many of
// them byte-identical copies of the same save. Both calls of the destructor go through
// ls_record_hook, which deletes the storage with its own destructor (vtable slot 0, flags 1)
// first. Nothing else frees +0x14: the restore 00AFE3B1 and the save writer 00AFE7C6 only read
// it. GOG has the same code (00B37334, calls 0044F348 and 00B3746F). See
// docs/fixes/layer-storage-leak.md.
#define VA_LS_RECORD_FREE  0x00AFE0A2u   // record destructor, EAX = record, plain RET
#define VA_LS_CLEAR_CALL   0x004461C8u   // E8 in the array clear 004461B1 (after mov eax,edi)
#define VA_LS_REMOVE_CALL  0x00AFE1C3u   // E8 in the single removal 00AFE12C (after mov eax,edi)
#define VA_GDS_VTBL        0x00E7E264u   // CGameDataStorage vtable (RTTI)
#define VA_GDS_DTOR        0x00421438u   // its scalar deleting destructor, slot 0, thiscall, RET 4

static uintptr_t g_lsRecordFree, g_gdsVtbl, g_gdsDtor;   // this build's, set before the patch
static volatile LONG g_lsFreed, g_lsFaults;
static volatile LONG64 g_lsBytes;   // capacity of the freed state buffers

static void __stdcall ls_free_state(unsigned char* rec)
{
    __try {
        void** st = rec ? *(void***)(rec + 0x14) : NULL;
        if (st && st[0] == (void*)g_gdsVtbl) {
            LONG cap = *(LONG*)((unsigned char*)st + 0x0C);
            *(void**)(rec + 0x14) = NULL;
            ((void (__fastcall*)(void*, void*, int))g_gdsDtor)(st, NULL, 1);
            InterlockedIncrement(&g_lsFreed);
            if (cap > 0) InterlockedExchangeAdd64(&g_lsBytes, cap);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { InterlockedIncrement(&g_lsFaults); }
}

// Same convention as the record destructor: record in EAX, no stack arguments. The callers
// keep EBX, ESI and EDI across the call; the C function keeps them too.
__declspec(naked) static void ls_record_hook(void)
{
    __asm {
        push eax                       ; kept for the original
        push eax                       ; argument
        call ls_free_state             ; stdcall, pops the argument
        pop eax
        jmp dword ptr [g_lsRecordFree] ; its RET returns to the caller
    }
}

static const char* ls_record_bytes(void)
{
    // push edi/esi; mov edi/esi,[eax+18h]; test; jz: the spawn data check at the start
    return by_build("57 8b 78 18 85 ff 74 24", "56 8b 70 18 85 f6 74 22");
}

static void storage_leak_tick(void)
{
    static int done, tries;
    static LONG reported;
    static DWORD report_tick;
    if (!cfg_storage_leak) return;
    if (done) {
        LONG n = g_lsFreed;
        if (n != reported && GetTickCount() - report_tick >= 30000) {
            report_tick = GetTickCount();
            logf_("stability: layer storage: %ld saved entity state(s) freed so far (%lld MB) that the game would have kept%s",
                  n, (long long)(g_lsBytes >> 20), g_lsFaults ? " - some reads failed" : "");
            reported = n;
        }
        return;
    }
    {
        uintptr_t rec = ga(VA_LS_RECORD_FREE), clr = ga(VA_LS_CLEAR_CALL), rem = ga(VA_LS_REMOVE_CALL);
        uintptr_t vt = ga(VA_GDS_VTBL), dtor = ga(VA_GDS_DTOR);
        const char* site = by_build("8b c7 e8", "8b c3 e8");
        const char* site2 = by_build("8b c7 e8", "8b c2 e8");
        int a, b;
        if (!code_matches(rec, ls_record_bytes()) || !code_matches(clr - 2, site) || !code_matches(rem - 2, site2) ||
            !range_readable((void*)vt, sizeof(void*)) || read_ptr((void*)vt) != (void*)dtor ||
            !code_matches(dtor, by_build("55 8b ec 56 8b f1 8d 46 04 e8", "55 8b ec 56 8b f1 8d 4e 04 e8"))) {
            if (++tries >= 100) {
                done = 1;
                cfg_storage_leak = 0;
                logf_("stability: layer storage record destructor not recognised at %08X - layer_storage_leak off",
                      (unsigned)rec);
            }
            return;
        }
        done = 1;
        g_lsRecordFree = rec; g_gdsVtbl = vt; g_gdsDtor = dtor;
        a = patch_call(clr, rec, (void*)ls_record_hook);
        b = patch_call(rem, rec, (void*)ls_record_hook);
        logf_("stability: layer storage leak fix: array clear %s, single removal %s",
              a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked");
        if (!a && !b) cfg_storage_leak = 0;
    }
}

// ---- text file terminator -----------------------------------------------
// [stability] text_guard=1. The engine's whole-file text reader (00AC6AAE, GOG 00B1B4AB; three
// callers, CSV tables among them) asks the file for its size, allocates size + 1 (the pool
// allocator 0040315F, GOG 00403268, cdecl, zeroes exactly what was asked), reads the file and
// writes one 0 byte after it. A UTF-16 file (FF FE) is then copied as a wide string from
// offset 2, and a wide string ends at a 0x0000 on an even offset: the byte after the one 0 is
// whatever the pool block held before. Seen 2026-11-07 (Steam, after a save load in Flotsam):
// globals\knowledge_list.csv's text ran on to the end of its page and the copy's wcslen
// (004020F9) read 0x6AB00000. The call to the allocator goes to text_alloc_hook, which asks
// for 2 bytes more and zeroes them, so the text ends in three 0 bytes whatever its length.
// The read itself still writes size bytes. The buffer is never freed by the reader, as before.
#define VA_TEXT_ALLOC       0x0040315Fu   // cdecl (size), returns the block in EAX
#define VA_TEXT_ALLOC_CALL  0x00AC6ACAu   // E8 rel32 after lea eax,[edi+1] / push eax

typedef void* (__cdecl *TextAlloc_t)(unsigned size);
static uintptr_t g_textAlloc;
static volatile LONG g_textReads;

static void* __cdecl text_alloc_hook(unsigned n)
{
    unsigned char* p;
    if (n == 0 || n > 0x7FFFFFF0u) return ((TextAlloc_t)g_textAlloc)(n);
    p = (unsigned char*)((TextAlloc_t)g_textAlloc)(n + 2);
    if (p) {
        p[n] = 0;
        p[n + 1] = 0;
        InterlockedIncrement(&g_textReads);
    }
    return p;
}

static void text_guard_tick(void)
{
    static int done, tries;
    uintptr_t call, alloc;
    if (done || !cfg_text_guard) return;
    call = ga(VA_TEXT_ALLOC_CALL);
    alloc = ga(VA_TEXT_ALLOC);
    // lea eax,[size+1] / push eax before the call; after it pop ecx, the read through the
    // file's vtable and `mov byte [buf+size],0` (Steam: size EDI, buffer ESI; GOG the other way).
    if (!call || !alloc ||
        !code_matches(call - 4, by_build("8d 47 01 50 e8", "8d 46 01 50 e8")) ||
        !code_matches(call + 5, by_build("59 8b f0 8b 03 57 56 8b cb ff 50 04 c6 04 3e 00",
                                         "59 8b f8 8b 03 56 57 8b cb ff 50 04 c6 04 37 00"))) {
        if (++tries >= 100) {
            done = 1;
            cfg_text_guard = 0;
            logf_("stability: text file reader not recognised at %08X - text_guard off", (unsigned)call);
        }
        return;
    }
    done = 1;
    g_textAlloc = alloc;
    if (patch_call(call, alloc, (void*)text_alloc_hook)) {
        logf_("stability: text guard installed (text files end in three 0 bytes, allocation at %08X)",
              (unsigned)call);
    } else {
        cfg_text_guard = 0;
        logf_("stability: text file reader call could not be patched - text_guard off");
    }
}

// ---- memory trace -------------------------------------------------------
// [diagnostics] mem_trace=N: every N minutes (first one after 2), a census of the process's own
// memory on a low-priority thread, to find what grows over a long session. Passive: it only
// reads. Per census:
// * private, free and largest free address space;
// * the engine's 64 KB pages (FUN_00ac84cd: one VirtualAlloc each) by their first bytes: saved
//   entity state (BLCK), Havok packfiles, small-block pages (12 12 12 12), other;
// * every aligned dword in committed private read/write memory that is an RTTI vtable of
//   witcher2.exe, counted per vtable. Freed memory that still holds a vtable counts too, so a
//   count is "objects plus remnants"; what matters is a class that keeps growing.
// The log gets one line per census with the classes that grew most since the first one; the full
// counts go to witcher2_enginefix_memtrace.tsv (previous session: .prev.tsv), read by
// tools/memtrace-report.py. Found the layer storage leak this way (docs/fixes/layer-storage-leak.md).
#define MT_SLOTS     (1u << 20)        // open-addressing table of vtable -> count, 8 MB
#define MT_MIN_TSV   20                // classes with fewer objects stay out of the file
#define MT_TOP       6

typedef struct { uint32_t key, count; } mt_slot;
typedef struct { uint32_t vt, count; } mt_pair;

static mt_slot*  g_mtTable;
static mt_pair*  g_mtBase;             // first census, sorted by vtable
static unsigned  g_mtBaseN;
static uintptr_t g_mtLo, g_mtHi;       // witcher2.exe image
static uint32_t* g_mtVt;               // one bit per dword of the image: an RTTI vtable starts there
static unsigned  g_mtVtN;              // how many
static char      g_mtPath[MAX_PATH];

// Only vtables are keys (some ten thousand), so the table never fills. Counting every value
// that fell inside the image did: in Flotsam at 2 GB private, texture and sound data alone gave
// millions of distinct "addresses", and with the table full each add walked all 1M slots (the
// first census never ended, 2026-11-06). The probe is capped as well.
static void mt_add(uint32_t v)
{
    uint32_t h = (v * 2654435761u) >> 12, i;
    for (i = 0; i < 4096; i++) {
        mt_slot* s = &g_mtTable[(h + i) & (MT_SLOTS - 1)];
        if (s->key == v) { s->count++; return; }
        if (!s->key) { s->key = v; s->count = 1; return; }
    }
}

static int mt_is_vt(uint32_t v)
{
    uint32_t k = (v - (uint32_t)g_mtLo) >> 2;
    return !(v & 3) && v >= g_mtLo && v < g_mtHi && (g_mtVt[k >> 5] >> (k & 31) & 1);
}

// The RTTI class name of a vtable of witcher2.exe, or NULL: complete object locator at vt-4
// (signature 0), type descriptor at +12, its name at +8 (".?AV...").
static const char* mt_rtti(uint32_t vt)
{
    const char* name = NULL;
    __try {
        uint32_t col = *(const uint32_t*)(uintptr_t)(vt - 4), td;
        if (col < g_mtLo || col + 16 > g_mtHi || *(const uint32_t*)(uintptr_t)col != 0) return NULL;
        td = *(const uint32_t*)(uintptr_t)(col + 12);
        if (td < g_mtLo || td + 16 > g_mtHi) return NULL;
        name = (const char*)(uintptr_t)(td + 8);
        if (name[0] != '.' || name[1] != '?' || name[2] != 'A' || (name[3] != 'V' && name[3] != 'U')) return NULL;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
    return name;
}

// The vtable map, once, before the first census: every dword of the image whose RTTI locator
// checks out. ~10M reads of the exe's own pages, well under a second.
static int mt_build_vt(void)
{
    uint32_t words = (uint32_t)((g_mtHi - g_mtLo) >> 2), k;
    if (g_mtVt) return 1;
    g_mtVt = (uint32_t*)VirtualAlloc(NULL, ((words + 31) >> 5) * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_mtVt) return 0;
    for (k = 1; k < words; k++)
        if (mt_rtti((uint32_t)g_mtLo + 4 * k)) { g_mtVt[k >> 5] |= 1u << (k & 31); g_mtVtN++; }
    return 1;
}

static int mt_pair_cmp(const void* a, const void* b)
{
    uint32_t x = ((const mt_pair*)a)->vt, y = ((const mt_pair*)b)->vt;
    return x < y ? -1 : x > y;
}

static uint32_t mt_base_count(uint32_t vt)
{
    mt_pair k, *p;
    if (!g_mtBase) return 0;
    k.vt = vt;
    p = (mt_pair*)bsearch(&k, g_mtBase, g_mtBaseN, sizeof k, mt_pair_cmp);
    return p ? p->count : 0;
}

// Scan one committed region in place; a page that goes away under the scan ends the region.
static void mt_scan(const uint32_t* p, size_t words)
{
    size_t i = 0;
    __try {
        for (; i < words; i++) {
            uint32_t v = p[i];
            if (mt_is_vt(v)) mt_add(v);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void mt_census(int n, DWORD t0)
{
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t at = 0x10000;
    unsigned long long priv = 0, freeb = 0, largest = 0;
    unsigned pages[4] = { 0, 0, 0, 0 };   // state, havok, small, other
    unsigned i, used = 0, classes = 0;
    DWORD start = GetTickCount(), sec = (start - t0) / 1000;
    FILE* f = NULL;
    mt_pair top[MT_TOP];
    mt_pair* now;
    char line[512];

    memset(g_mtTable, 0, MT_SLOTS * sizeof(mt_slot));
    while (at < 0xFFFF0000u && VirtualQuery((void*)at, &mbi, sizeof mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        if (mbi.State == MEM_FREE) {
            freeb += mbi.RegionSize;
            if (mbi.RegionSize > largest) largest = mbi.RegionSize;
        } else if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE) {
            DWORD pr = mbi.Protect;
            priv += mbi.RegionSize;
            if ((pr & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
                !(pr & (PAGE_GUARD | PAGE_NOACCESS)) &&
                !(base >= (uintptr_t)g_mtTable && base < (uintptr_t)g_mtTable + MT_SLOTS * sizeof(mt_slot)) &&
                mbi.AllocationBase != (void*)g_mtVt) {
                if (mbi.RegionSize == 0x10000 && mbi.AllocationBase == mbi.BaseAddress) {
                    uint32_t h = 0;
                    __try { h = *(const uint32_t*)base; } __except (EXCEPTION_EXECUTE_HANDLER) { h = 0; }
                    pages[h == 0x4B434C42u ? 0 : h == 0x57E0E057u ? 1 : h == 0x12121212u ? 2 : 3]++;
                }
                mt_scan((const uint32_t*)base, mbi.RegionSize / 4);
            }
        }
        if (base + mbi.RegionSize <= at) break;
        at = base + mbi.RegionSize;
    }
    for (i = 0; i < MT_SLOTS; i++) if (g_mtTable[i].key) used++;
    now = (mt_pair*)malloc((used ? used : 1) * sizeof(mt_pair));
    if (!now) return;
    // keep the vtables only
    for (i = 0; i < MT_SLOTS; i++) {
        mt_slot* s = &g_mtTable[i];
        if (s->key && s->count >= 2 && mt_rtti(s->key)) { now[classes].vt = s->key; now[classes].count = s->count; classes++; }
    }
    qsort(now, classes, sizeof *now, mt_pair_cmp);

    if (fopen_s(&f, g_mtPath, n == 1 ? "w" : "a") == 0 && f) {
        if (n == 1) fprintf(f, "census\tseconds\tkind\tkey\tvalue\n");
        fprintf(f, "%d\t%lu\tmem\tprivate_mb\t%llu\n", n, sec, priv >> 20);
        fprintf(f, "%d\t%lu\tmem\tfree_mb\t%llu\n", n, sec, freeb >> 20);
        fprintf(f, "%d\t%lu\tmem\tlargest_free_mb\t%llu\n", n, sec, largest >> 20);
        fprintf(f, "%d\t%lu\tpage\tstate\t%u\n%d\t%lu\tpage\thavok\t%u\n%d\t%lu\tpage\tsmall\t%u\n%d\t%lu\tpage\tother\t%u\n",
                n, sec, pages[0], n, sec, pages[1], n, sec, pages[2], n, sec, pages[3]);
        for (i = 0; i < classes; i++)
            if (now[i].count >= MT_MIN_TSV)
                fprintf(f, "%d\t%lu\tclass\t%s@%08X\t%u\n", n, sec, mt_rtti(now[i].vt), (unsigned)now[i].vt, now[i].count);
        fclose(f);
    }

    memset(top, 0, sizeof top);
    if (g_mtBase) {
        for (i = 0; i < classes; i++) {
            uint32_t was = mt_base_count(now[i].vt), grew = now[i].count > was ? now[i].count - was : 0;
            int k;
            if (!grew) continue;
            for (k = MT_TOP; k > 0 && grew > top[k - 1].count; k--) if (k < MT_TOP) top[k] = top[k - 1];
            if (k < MT_TOP) { top[k].vt = now[i].vt; top[k].count = grew; }
        }
        line[0] = 0;
        for (i = 0; i < MT_TOP && top[i].count; i++) {
            const char* nm = mt_rtti(top[i].vt);
            const char* end;
            size_t len = strlen(line);
            if (!nm) continue;
            nm += 4;   // ".?AV"
            end = strstr(nm, "@@");
            _snprintf_s(line + len, sizeof line - len, _TRUNCATE, "%s%.*s +%u", i ? ", " : "",
                        (int)(end ? end - nm : (ptrdiff_t)strlen(nm)), nm, top[i].count);
        }
        free(now);
    } else {
        g_mtBase = now;
        g_mtBaseN = classes;
        strcpy_s(line, sizeof line, "first census, the baseline");
    }
    logf_("memtrace: #%d at %lu min: private %llu MB, free %llu MB (largest %llu MB); 64 KB pages: state %u, havok %u, small %u, other %u; %u classes; %s (%lu ms)",
          n, sec / 60, priv >> 20, freeb >> 20, largest >> 20, pages[0], pages[1], pages[2], pages[3], classes,
          line[0] ? line : "nothing grew since the first", GetTickCount() - start);
}

static DWORD WINAPI memtrace_thread(LPVOID param)
{
    const IMAGE_DOS_HEADER* dos;
    const IMAGE_NT_HEADERS32* nt;
    char prev[MAX_PATH];
    DWORD t0 = GetTickCount(), wait = 120000;
    int n = 0;
    (void)param;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    __try {
        dos = (const IMAGE_DOS_HEADER*)g_base;
        nt = (const IMAGE_NT_HEADERS32*)(g_base + dos->e_lfanew);
        g_mtLo = g_base;
        g_mtHi = g_base + nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { g_mtLo = g_mtHi = 0; }
    g_mtTable = (mt_slot*)VirtualAlloc(NULL, MT_SLOTS * sizeof(mt_slot), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_base || g_mtHi <= g_mtLo || !g_mtTable) {
        logf_("memtrace: no exe image or no table - off");
        return 0;
    }
    sprintf_s(g_mtPath, MAX_PATH, "%s\\witcher2_enginefix_memtrace.tsv", g_binDir);
    sprintf_s(prev, MAX_PATH, "%s\\witcher2_enginefix_memtrace.prev.tsv", g_binDir);
    MoveFileExA(g_mtPath, prev, MOVEFILE_REPLACE_EXISTING);
    logf_("memtrace: a census every %d min, the first in 2 min; counts in %s", cfg_mem_trace, g_mtPath);
    while (g_running) {
        DWORD slept = 0;
        while (g_running && slept < wait) { Sleep(1000); slept += 1000; }
        if (!g_running) break;
        if (!g_mtVt) {
            DWORD s = GetTickCount();
            if (!mt_build_vt()) { logf_("memtrace: no memory for the vtable map - off"); break; }
            logf_("memtrace: %u vtables in witcher2.exe (%lu ms)", g_mtVtN, GetTickCount() - s);
        }
        mt_census(++n, t0);
        wait = (DWORD)cfg_mem_trace * 60000;
    }
    return 0;
}

// ---- crash capture ------------------------------------------------------
// The game's own top-level filter (installed by the static initializer at 00930D99,
// handler 00930D4A) is `xor eax,eax; inc eax; ret 4`: EXCEPTION_EXECUTE_HANDLER, so a
// crash ends the process silently - no WER report, no event log entry, nothing on disk.
// It runs after DllMain, so the filter set there is replaced; crash_tick() takes the
// top slot back every worker tick and chains to whatever it displaced, which keeps the
// game's behaviour (and any overlay's filter) after the log line and the dump are out.
// Only unhandled exceptions reach a top-level filter: exceptions the game catches
// itself, and CRT aborts (__report_gsfailure, __call_reportfault clear the filter
// first), are not seen here. See docs/fixes/crash-capture.md.

#define CRASH_KEEP_DUMPS 5

typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;
static MiniDumpWriteDump_t g_miniDumpWriteDump;
static volatile LONG g_inCrash;

// logf_ without waiting: the crashing thread may be the one holding the log lock.
static void crash_logf(const char* fmt, ...)
{
    if (!g_log || !cfg_log) return;
    if (!TryEnterCriticalSection(&g_logLock)) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

// "witcher2.exe+0x530D4A" for an address inside a loaded image, "?" otherwise.
static void crash_where(DWORD addr, char* out, size_t cap)
{
    HMODULE mod = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &mod) && mod &&
        GetModuleFileNameA(mod, path, MAX_PATH)) {
        const char* name = strrchr(path, '\\');
        sprintf_s(out, cap, "%s+0x%X", name ? name + 1 : path, addr - (DWORD)(uintptr_t)mod);
    } else {
        strcpy_s(out, cap, "?");
    }
}

static int crash_is_code(DWORD addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (addr < 0x10000 || !VirtualQuery((LPCVOID)(uintptr_t)addr, &mbi, sizeof mbi)) return 0;
    return mbi.State == MEM_COMMIT && mbi.Type == MEM_IMAGE &&
           (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY));
}

static void crash_write_dump(EXCEPTION_POINTERS* ep)
{
    char path[MAX_PATH];
    SYSTEMTIME st;
    HANDLE h;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    MINIDUMP_TYPE type;

    if (!g_miniDumpWriteDump) { crash_logf("crash: no dump (dbghelp MiniDumpWriteDump not found)"); return; }
    GetLocalTime(&st);
    sprintf_s(path, MAX_PATH, "%s\\witcher2_enginefix_crash_%04d-%02d-%02d_%02d-%02d-%02d.dmp",
              g_binDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { crash_logf("crash: cannot create %s (%lu)", path, GetLastError()); return; }
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    type = cfg_crash_dump >= 2
        ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithHandleData |
                          MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules)
        : (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData |
                          MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
    if (g_miniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, type, &mei, NULL, NULL)) {
        LARGE_INTEGER size; size.QuadPart = 0;
        GetFileSizeEx(h, &size);
        crash_logf("crash: dump written: %s (%.1f MB)", path, size.QuadPart / 1048576.0);
    } else {
        crash_logf("crash: MiniDumpWriteDump failed (0x%08lX)", GetLastError());
    }
    CloseHandle(h);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep)
{
    // A second crashing thread, or a filter that chains back to us: let the next one run.
    if (InterlockedExchange(&g_inCrash, 1)) return EXCEPTION_CONTINUE_SEARCH;

    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        EXCEPTION_RECORD* er = ep->ExceptionRecord;
        CONTEXT* c = ep->ContextRecord;
        char where[MAX_PATH + 16];
        PROCESS_MEMORY_COUNTERS_EX pmc;

        crash_where((DWORD)(uintptr_t)er->ExceptionAddress, where, sizeof where);
        crash_logf("crash: unhandled exception 0x%08lX at 0x%08X (%s), thread %lu",
                   er->ExceptionCode, (unsigned)(uintptr_t)er->ExceptionAddress, where,
                   GetCurrentThreadId());
        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            ULONG_PTR op = er->ExceptionInformation[0];
            crash_logf("crash: access violation %s 0x%08X",
                       op == 0 ? "reading" : op == 1 ? "writing" : op == 8 ? "executing" : "at",
                       (unsigned)er->ExceptionInformation[1]);
        }
        crash_logf("crash: eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX",
                   c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi);
        crash_logf("crash: eip=%08lX esp=%08lX ebp=%08lX", c->Eip, c->Esp, c->Ebp);

        // Raw stack scan: code addresses found on the stack, newest first. Most are
        // return addresses; some are stale. The dump has the real unwind.
        {
            DWORD sp = c->Esp & ~3u, top = (DWORD)__readfsdword(4);   // TEB StackBase
            int found = 0;
            if (sp && top > sp) {
                if (top - sp > 0x10000) top = sp + 0x10000;
                for (; sp < top && found < 24; sp += 4) {
                    DWORD v = *(DWORD*)(uintptr_t)sp;
                    if (!crash_is_code(v)) continue;
                    crash_where(v, where, sizeof where);
                    crash_logf("crash:   [esp+%04X] %08lX %s", sp - (c->Esp & ~3u), v, where);
                    found++;
                }
            }
        }

        pmc.cb = sizeof pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc))
            crash_logf("crash: memory private=%lu MB working set=%lu MB",
                       (unsigned long)(pmc.PrivateUsage >> 20), (unsigned long)(pmc.WorkingSetSize >> 20));
        crash_write_dump(ep);
    } else {
        crash_logf("crash: unhandled exception without exception pointers");
    }

    if (g_prevFilter) {
        crash_logf("crash: passing on to the previous filter at 0x%08X",
                   (unsigned)(uintptr_t)g_prevFilter);
        return g_prevFilter(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Takes the top-level filter slot back. Called from DllMain and every worker tick.
static void crash_tick(void)
{
    static int logged;
    LPTOP_LEVEL_EXCEPTION_FILTER prev;
    if (!cfg_crash_dump || g_inCrash) return;
    prev = SetUnhandledExceptionFilter(crash_filter);
    if (prev != crash_filter) {
        char where[MAX_PATH + 16];
        g_prevFilter = prev;
        if (logged++ < 5) {   // a module that keeps re-installing its own would flood the log
            crash_where((DWORD)(uintptr_t)prev, where, sizeof where);
            logf_("crash: filter armed, chaining to 0x%08X (%s)", (unsigned)(uintptr_t)prev,
                  prev ? where : "none");
        }
    }
}

// Keeps the newest CRASH_KEEP_DUMPS dumps; the names sort by time.
static void crash_prune_dumps(void)
{
    char pattern[MAX_PATH], names[64][64], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int n = 0, i, j;

    sprintf_s(pattern, MAX_PATH, "%s\\witcher2_enginefix_crash_*.dmp", g_binDir);
    f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 64 && strlen(fd.cFileName) < sizeof names[0]) strcpy_s(names[n++], sizeof names[0], fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[64];
            strcpy_s(t, sizeof t, names[j]); strcpy_s(names[j], sizeof names[j], names[j - 1]);
            strcpy_s(names[j - 1], sizeof names[j - 1], t);
        }
    for (i = 0; i < n - CRASH_KEEP_DUMPS; i++) {
        sprintf_s(path, MAX_PATH, "%s\\%s", g_binDir, names[i]);
        if (DeleteFileA(path)) logf_("crash: removed old dump %s", names[i]);
    }
}

static void crash_init(void)
{
    HMODULE dbg;
    if (!cfg_crash_dump) return;
    // The game imports dbghelp itself, so this normally finds the loaded copy.
    dbg = GetModuleHandleA("dbghelp.dll");
    if (!dbg) dbg = LoadLibraryA("dbghelp.dll");
    if (dbg) g_miniDumpWriteDump = (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump");
    if (!g_miniDumpWriteDump) logf_("crash: MiniDumpWriteDump not found - crashes are logged without a dump");
    crash_prune_dumps();
    crash_tick();
}

// ---- how the game ends --------------------------------------------------
// [diagnostics] exit_trace=1 (default): the log also says who ends the process: the exe's ExitProcess and
// TerminateProcess import slots, and the same two in the platform DLL (Galaxy.dll on GOG,
// steam_api.dll on Steam), are logged with their caller just before the real function runs.
// After ExitProcess Windows ends every other thread and then runs the DLL detach, which writes
// `unloaded`.
// The CRT's exit (__crtExitProcess, GOG 00743F4A, Steam 00672371) first looks for the .NET
// runtime: with mscoree.dll in the process it calls its CorExitProcess (GOG 00743F1F, Steam
// 00672346: GetModuleHandleW, then GetProcAddress), and only if that returns ExitProcess.
// On GOG the runtime is loaded (its filter, mscoreei.dll, shows in the log; Steam's process has
// none), and the process then ends without the DLL detach: the log stopped at the CRT's
// filter change, with no `unloaded` (docs/fixes/gog-build.md, "Open"). So the exe's
// GetProcAddress hands out a wrapper for CorExitProcess that logs the end first. Logging only.
typedef void (WINAPI *ExitProcess_t)(UINT);
typedef BOOL (WINAPI *TerminateProcess_t)(HANDLE, UINT);
typedef FARPROC (WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef void (WINAPI *CorExitProcess_t)(int);
static ExitProcess_t      g_realExitProcess;
static TerminateProcess_t g_realTerminateProcess;
static GetProcAddress_t   g_realGetProcAddress;
static CorExitProcess_t   g_realCorExitProcess;

static void exit_note(const char* what, UINT code, void* caller)
{
    char where[MAX_PATH + 16], quit[96] = "";
    DWORD tid = GetCurrentThreadId();
    crash_where((DWORD)(uintptr_t)caller, where, sizeof where);
    if (g_factsNoWorld)
        sprintf_s(quit, sizeof quit, "; quit guard: FactsAdd ran without a world %ld time(s)",
                  (long)g_factsNoWorld);
    exit_logf("exit: %s(%u) from %s on thread %lu%s%s", what, code, where, tid,
              tid == g_mainTid ? " (the main thread)" : "", quit);
}

static void WINAPI hook_ExitProcess(UINT code)
{
    exit_note("ExitProcess", code, _ReturnAddress());
    g_realExitProcess(code);
}

static BOOL WINAPI hook_TerminateProcess(HANDLE process, UINT code)
{
    if (process == GetCurrentProcess() || GetProcessId(process) == GetCurrentProcessId())
        exit_note("TerminateProcess", code, _ReturnAddress());
    return g_realTerminateProcess(process, code);
}

static void WINAPI hook_CorExitProcess(int code)
{
    exit_note("CorExitProcess", (UINT)code, _ReturnAddress());
    exit_logf("exit: the CRT hands the exit to the .NET runtime (on GOG it ends the process without the DLL detach)");
    g_realCorExitProcess(code);
    exit_logf("exit: CorExitProcess returned - the CRT goes on to ExitProcess");
}

static int __stdcall gpa_is_corexit(LPCSTR name)
{
    int is = 0;
    __try { is = ((uintptr_t)name >> 16) && !strcmp(name, "CorExitProcess"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { is = 0; }
    return is;
}

static FARPROC WINAPI gpa_corexit(HMODULE mod, LPCSTR name)
{
    FARPROC real = g_realGetProcAddress(mod, name);
    if (!real) return NULL;
    g_realCorExitProcess = (CorExitProcess_t)(void*)real;
    return (FARPROC)(void*)hook_CorExitProcess;
}

// GetProcAddress resolves for its caller (KERNELBASE!GetProcAddressForCaller takes the return
// address), so every other lookup jumps on with the game's own return address on the stack.
__declspec(naked) static void hook_GetProcAddress(void)
{
    __asm {
        push dword ptr [esp + 8]             ; name
        call gpa_is_corexit
        test eax, eax
        jnz corexit
        jmp dword ptr [g_realGetProcAddress]
    corexit:
        jmp gpa_corexit
    }
}

typedef struct { uintptr_t exitSlot, termSlot; } exit_slots;

static void exit_slots_cb(void* ctx, const char* dll, const char* fn, uintptr_t slot)
{
    exit_slots* s = (exit_slots*)ctx;
    if (_stricmp(dll, "kernel32.dll")) return;
    if (!strcmp(fn, "ExitProcess")) s->exitSlot = slot;
    else if (!strcmp(fn, "TerminateProcess")) s->termSlot = slot;
}

// From DllMain: the loader has filled every static import by then.
static void exit_trace_install(void)
{
    static const char* const platform[] = { "Galaxy.dll", "steam_api.dll" };
    int i;
    if (!cfg_exit_trace) return;
    patch_iat("exit", IMP_EXITPROCESS, (void*)hook_ExitProcess, (void**)&g_realExitProcess);
    patch_iat("exit", IMP_TERMINATEPROCESS, (void*)hook_TerminateProcess, (void**)&g_realTerminateProcess);
    patch_iat("exit", IMP_GETPROCADDRESS, (void*)hook_GetProcAddress, (void**)&g_realGetProcAddress);
    for (i = 0; i < 2; i++) {
        HMODULE mod = GetModuleHandleA(platform[i]);
        exit_slots s = { 0, 0 };
        char tag[40];
        if (!mod) continue;
        imports_walk((const unsigned char*)mod, exit_slots_cb, &s);
        sprintf_s(tag, sizeof tag, "exit (%s)", platform[i]);
        if (s.exitSlot)
            patch_import_slot(tag, "kernel32.dll", "ExitProcess", s.exitSlot, (void*)hook_ExitProcess,
                              (void**)&g_realExitProcess);
        if (s.termSlot)
            patch_import_slot(tag, "kernel32.dll", "TerminateProcess", s.termSlot,
                              (void*)hook_TerminateProcess, (void**)&g_realTerminateProcess);
    }
}

// ---- window mode: full screen, borderless, windowed -----------------------------------------
// The game creates its top-level window in FUN_009d44e1: as a captioned window (style 0x00C80000,
// centered, client area = the configured resolution) when windowed, or as a topmost popup when
// full screen. Borderless replaces only the windowed style, so Direct3D stays windowed and an
// alt-tab never recreates the device.
//
// Full screen is the Windowed flag of the device's stored present parameters. The game has no
// call that changes it, but its own lost-device path (viewport tick FUN_009E146E: release, then
// Reset with the stored parameters) is exactly the sequence a switch needs, so the viewport tick
// is hooked (window_patch.h) and drives that path with a new Windowed flag, on the game's thread.
//
// The wanted state comes from the game's config (the Options > Graphics "Display mode" row writes
// [Rendering] Fullscreen and EngineFixBorderless through the game's own WriteConfigParamFloat,
// stored in User.ini), else from [window] borderless in witcher2_enginefix.ini.

static LONG g_wfApplied = -1;            // 0/1 = the window's current style, -2 = fullscreen
static volatile LONG g_wfFrom;           // 1 = options menu, 0 = startup/other
static volatile LONG g_wfFullscreenHint; // 1/-1: the menu wrote Fullscreen=1/0 (no live switch)
static int g_wfWarnedFullscreen;
// Live full screen switch (wf_vp_tick_hook). g_fsWant is set by the menu and taken back by the
// hook once the device runs in that mode; the worker leaves the window alone while it is set.
static int g_fsHooked;                   // the viewport tick is hooked: Full screen applies live
static uintptr_t g_wfVpTick;             // this build's viewport tick, set before its slot is swapped
static volatile LONG g_fsWant = -1;      // 1 full screen, 0 windowed, -1 nothing pending
static volatile LONG g_fsResult;         // 1/2 switched to full screen/windowed, -1 Reset refused
static volatile LONG g_fsTicks;          // viewport ticks seen, so a stalled switch is noticed
static volatile LONG g_fsStage;          // 0 idle, 1 released (the hook asked for state 1)
static BOOL g_fsPrevWindowed;
static HWND g_wfWnd;
// Draw distance row (Options > Graphics). The menu writes [Rendering] EngineFixDrawDistance; the
// worker applies that level without the OSD. The other way round, the live level is written into
// the game's config whenever it changes elsewhere (start, Ctrl+F10), so the row opens on it. That
// write calls the engine's config writer, so it happens on the game's main thread, inside the
// viewport tick, never from the DLL's threads.
static volatile LONG g_ddMenu = -1;      // level the menu chose, -1 nothing pending
static volatile LONG g_ddOffThread;      // the viewport tick ran elsewhere: no sync (logged)
// The swapchain's size: [Viewport] Width/Height from User.ini, else the client area the window
// was born with. A restyle always restores it, because a framed window as large as the screen is
// clipped by Windows (WM_GETMINMAXINFO), so its client area shrinks and cannot be trusted later.
static int g_wfCx, g_wfCy;

static void wf_state(int on)
{
    InterlockedExchange(&wf_borderless, on ? 1 : 0);
    InterlockedExchange(&wf_style, on ? (LONG)WF_STYLE_POPUP : (LONG)WF_STYLE_FRAMED);
}

static void wf_request(int on, LONG from)
{
    wf_state(on);
    InterlockedExchange(&g_wfFrom, from);
}

// The options menu's write lands here on the game's main thread. Only the state changes; the
// viewport tick (full screen) and the worker thread (window style) apply it, so no window, device
// or log work happens inside a script call.
static void wf_on_config_write(const wchar_t* section, const wchar_t* key, float value)
{
    (void)section;
    if (!cfg_wf_enabled) return;
    if (_wcsicmp(key, L"EngineFixDrawDistance") == 0) {
        if (cfg_preset_mode)
            InterlockedExchange(&g_ddMenu, value < 0.5f ? 0 : value < 1.5f ? 1 : value < 2.5f ? 2 : 3);
    } else if (_wcsicmp(key, L"EngineFixShadowQuality") == 0) {
        if (cfg_cascade) InterlockedExchange(&g_shadowMenu, value < 0.5f ? 0 : value < 1.5f ? 1 : 2);
    } else if (_wcsicmp(key, L"EngineFixBorderless") == 0) {
        wf_request(value > 0.0f, 1);
    } else if (_wcsicmp(key, L"Fullscreen") == 0) {
        if (g_fsHooked) InterlockedExchange(&g_fsWant, value > 0.0f ? 1 : 0);
        else InterlockedExchange(&g_wfFullscreenHint, value > 0.0f ? 1 : -1);
    }
}

// Documents\Witcher 2\Config\User.ini
static int wf_user_ini_path(wchar_t* path)
{
    wchar_t docs[MAX_PATH];

    if (SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL, SHGFP_TYPE_CURRENT, docs) != S_OK) return 0;
    return _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s\\Witcher 2\\Config\\User.ini", docs) >= 0;
}

// [Rendering] EngineFixBorderless in User.ini. *found = 0 when absent.
static int wf_user_ini_borderless(int* found)
{
    wchar_t path[MAX_PATH];
    int v;

    *found = 0;
    if (!wf_user_ini_path(path)) return 0;
    v = (int)GetPrivateProfileIntW(L"Rendering", L"EngineFixBorderless", -1, path);
    if (v < 0) return 0;
    *found = 1;
    return v != 0;
}

// [Rendering] EngineFixDrawDistance in User.ini: the Draw distance row's last level (0-3), -1
// when absent. The engine stores floats ("2.000000"); the integer part is the level.
static int wf_user_ini_draw_distance(void)
{
    wchar_t path[MAX_PATH];
    int v;

    if (!wf_user_ini_path(path)) return -1;
    v = (int)GetPrivateProfileIntW(L"Rendering", L"EngineFixDrawDistance", -1, path);
    return v >= 0 && v < PRESET_COUNT ? v : -1;
}

// [Rendering] EngineFixShadowQuality in User.ini: the Shadow quality row's last level, -1 when absent.
static int wf_user_ini_shadow_quality(void)
{
    wchar_t path[MAX_PATH];
    int v;

    if (!wf_user_ini_path(path)) return -1;
    v = (int)GetPrivateProfileIntW(L"Rendering", L"EngineFixShadowQuality", -1, path);
    return v >= 0 && v < SHADOW_LEVELS ? v : -1;
}

// [Viewport] Width/Height in User.ini: the resolution the launcher's Configurator sets, and the
// size the game creates its window and swapchain with. 0 when absent.
static void wf_user_ini_resolution(void)
{
    wchar_t path[MAX_PATH];
    int cx, cy;

    if (!wf_user_ini_path(path)) return;
    cx = (int)GetPrivateProfileIntW(L"Viewport", L"Width", 0, path);
    cy = (int)GetPrivateProfileIntW(L"Viewport", L"Height", 0, path);
    if (cx >= 640 && cy >= 480 && cx <= 16384 && cy <= 16384) { g_wfCx = cx; g_wfCy = cy; }
}

// The game has no manifest, so it is DPI-unaware: on a scaled desktop Windows virtualizes the
// window (a 3840x2160 client becomes 5760x3240 physical and is clipped). System awareness is
// enough because the window always lands on the primary monitor, and it avoids WM_DPICHANGED
// handling. Must run before the game creates its window.
static void wf_set_dpi_aware(void)
{
    typedef BOOL (WINAPI *wf_dpi_ctx_t)(HANDLE);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    wf_dpi_ctx_t ctx = user32
        ? (wf_dpi_ctx_t)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;

    // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE is (HANDLE)-2; SetProcessDPIAware is the Windows 7 path.
    if (ctx && ctx((HANDLE)-2)) { logf_("window: dpi awareness = system aware"); return; }
    if (SetProcessDPIAware()) { logf_("window: dpi awareness = system aware (fallback)"); return; }
    logf_("window: dpi awareness not set (%lu); borderless may be DPI-virtualized",
          (unsigned long)GetLastError());
}

// "mov edi,0x00C80000" -> "call wf_style_stub". Only EDI changes, as the replaced instruction did.
static int wf_install_style(void)
{
    static const char* const orig = "bf 00 00 c8 00";
    unsigned char code[5];
    pb_range range;
    HANDLE peers[256];
    unsigned count = 0;
    DWORD old = 0, tmp = 0;
    uintptr_t at = ga(WF_STYLE_SITE);
    void* site = (void*)at;
    int64_t delta = (int64_t)(uintptr_t)wf_style_stub - (int64_t)(at + 5);

    if (delta < INT32_MIN || delta > INT32_MAX) return 0;   // the call must be reachable
    if (!code_matches(at, orig)) return 0;
    code[0] = 0xE8;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    range.start = (const unsigned char*)site;
    range.len = (unsigned)sizeof wf_style_orig;
    if (!pb_stop_peers(peers, &count, &range, 1)) return 0;
    if (!code_matches(at, orig) ||
        !VirtualProtect(site, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        pb_resume_peers(peers, count);
        return 0;
    }
    memcpy(site, code, sizeof code);
    VirtualProtect(site, sizeof code, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof code);
    pb_resume_peers(peers, count);
    return 1;
}

// FUN_0041F4B7 entry -> "jmp wf_write_hook"; the trampoline runs the replaced prologue and jumps
// back. The replaced prologue is 6 bytes (push ebp; mov ebp,esp; and esp,-8), so 5 + one NOP.
// The writer's first bytes: the 6-byte prologue the trampoline replays, then what follows it.
// Steam's next instruction reads the config manager (`mov eax,[01608650]`); GOG's writer gets it
// in ECX (__thiscall) and goes on with `sub esp,34h; push ebx/esi/edi; push [ebp+8]; call`.
static const char* wf_writer_bytes(void)
{
    return by_build("55 8b ec 83 e4 f8 a1 50 86 60 01", "55 8b ec 83 e4 f8 83 ec 34 53 56 57 ff 75 08 e8");
}

static int wf_install_writer(void)
{
    const char* const orig = wf_writer_bytes();
    int gog = g_build == BUILD_GOG;
    unsigned char* tramp;
    unsigned char code[6];
    pb_range range;
    HANDLE peers[256];
    unsigned count = 0;
    DWORD old = 0, tmp = 0;
    uintptr_t at = ga(WF_WRITER_SITE);
    void* site = (void*)at;
    void* hook = gog ? (void*)wf_write_hook_gog : (void*)wf_write_hook;
    int64_t delta = (int64_t)(uintptr_t)hook - (int64_t)(at + 5);

    if (delta < INT32_MIN || delta > INT32_MAX) return 0;   // the jump must be reachable
    if (!code_matches(at, orig)) return 0;
    tramp = (unsigned char*)wf_make_trampoline(site, wf_writer_orig, sizeof wf_writer_orig);
    if (!tramp) return 0;
    if (gog) wf_real_write_gog = (wf_write_gog_fn)(void*)tramp;
    else wf_real_write = (wf_write_fn)(void*)tramp;
    code[0] = 0xE9;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    code[5] = 0x90;
    range.start = (const unsigned char*)site;
    range.len = (unsigned)sizeof wf_writer_orig;
    if (!pb_stop_peers(peers, &count, &range, 1)) goto failed;
    if (!code_matches(at, orig) ||
        !VirtualProtect(site, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        pb_resume_peers(peers, count);
        goto failed;
    }
    memcpy(site, code, sizeof code);
    VirtualProtect(site, sizeof code, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof code);
    pb_resume_peers(peers, count);
    return 1;
failed:
    wf_real_write = NULL;
    wf_real_write_gog = NULL;
    VirtualFree(tramp, 0, MEM_RELEASE);
    return 0;
}

// The game's own window (class registered in FUN_009d44e1), never another process's.
static HWND wf_window(void)
{
    DWORD pid = 0;

    if (g_wfWnd && IsWindow(g_wfWnd)) return g_wfWnd;
    g_wfWnd = FindWindowW(L"W2ViewportClass", NULL);
    if (g_wfWnd) GetWindowThreadProcessId(g_wfWnd, &pid);
    if (pid != GetCurrentProcessId()) g_wfWnd = NULL;
    return g_wfWnd;
}

// Give the client area the swapchain's size (g_wfCx/g_wfCy, not the current client area: see
// there) and put the frame outside it. A framed window at the desktop resolution is larger than
// the screen, so the game's own centering was off-screen; borderless centers a smaller window and
// pins a full-screen one at (0,0). A window that was full screen loses its topmost flag here.
static void wf_restyle(HWND w, int on)
{
    RECT c, r;
    LONG frame = on ? (LONG)WF_STYLE_POPUP : (LONG)WF_STYLE_FRAMED;
    LONG style = frame | WS_VISIBLE | WS_CLIPCHILDREN;
    HWND after = (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_NOTOPMOST : HWND_TOP;
    int sw, sh, x = 0, y = 0, ww, wh;

    if (g_wfCx > 0 && g_wfCy > 0) {
        SetRect(&c, 0, 0, g_wfCx, g_wfCy);
    } else if (!GetClientRect(w, &c) || c.right <= 0 || c.bottom <= 0) {
        return;
    }
    r = c;
    AdjustWindowRect(&r, (DWORD)frame, FALSE);
    ww = r.right - r.left;
    wh = r.bottom - r.top;
    if (ww <= 0 || wh <= 0) return;
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    if (c.right != sw || c.bottom != sh) {
        x = (sw - ww) / 2; if (x < 0) x = 0;
        y = (sh - wh) / 2; if (y < 0) y = 0;
    }
    // Never derive WS_VISIBLE from the old style: the game's styles lack it and an invisible
    // window never gets the focus (the W1 borderless black-screen incident).
    SetWindowLongW(w, GWL_STYLE, style);
    SetWindowPos(w, after, x, y, ww, wh,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE);
}

// The device's mode as the engine knows it: 1 full screen, 0 windowed, -1 no device yet.
static int wf_engine_fullscreen(void)
{
    uintptr_t dev = ga(WF_D3D_DEVICE), pp = ga(WF_PP);
    if (!dev || !pp || !*(void* volatile*)dev) return -1;
    return *(volatile BOOL*)(pp + WF_PP_OFF_WINDOWED) == FALSE;
}

// The window the game itself creates for full screen (FUN_009d44e1): a topmost popup at (0,0).
// Done before the Reset, synchronously when this thread owns the window.
static void wf_fullscreen_window(void)
{
    uintptr_t pp = ga(WF_PP);
    HWND w = pp ? *(HWND volatile*)(pp + WF_PP_OFF_HWND) : NULL;
    UINT async = 0;

    if (!w || !IsWindow(w)) return;
    if (GetWindowThreadProcessId(w, NULL) != GetCurrentThreadId()) async = SWP_ASYNCWINDOWPOS;
    SetWindowLongW(w, GWL_STYLE, (LONG)(WF_STYLE_POPUP | WS_VISIBLE | WS_CLIPCHILDREN));
    SetWindowPos(w, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW | async);
}

// DXVK hooks the window procedure when it enters full screen and unhooks when it leaves, but only
// if its procedure is still the top one (src/d3d9/d3d9_window.cpp, ResetWindowProc). NVIDIA's
// Vulkan driver subclasses the same window (opengl32!wglWndProc). After a full screen start the
// chain is wgl -> DXVK -> game; leaving full screen drops DXVK's record but leaves its procedure
// in the chain (it then sends everything to DefWindowProc, so the game's own procedure no longer
// sees messages), and entering full screen again hooks DXVK on top of wgl, whose "previous" is
// DXVK: the two call each other until the stack overflows (owner's test 2026-11-02, WER dump with
// ~10 000 alternating d3d9/wglWndProc frames). So around every switch the window gets its class
// procedure (the game's FUN_009D237E) back first: DXVK then always hooks or unhooks a clean chain.
// Only when the d3d9.dll in use is not the system one (a wrapper such as DXVK).
static void wf_reset_wndproc(HWND w)
{
    static int wrapper = -1;
    LONG cls;

    if (wrapper < 0) {
        wchar_t path[MAX_PATH], sys[MAX_PATH];
        HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);

        wrapper = d3d9 && n && n < MAX_PATH && GetModuleFileNameW(d3d9, path, MAX_PATH) &&
                  _wcsnicmp(path, sys, n) != 0;
        logf_("window: d3d9.dll is %s", wrapper ? "a wrapper (window procedure repaired at switches)"
                                                : "the system one");
    }
    if (!wrapper || !w || !IsWindow(w)) return;
    cls = GetClassLongW(w, GCL_WNDPROC);
    if (!ga(WF_GAME_WNDPROC) || (uint32_t)cls != (uint32_t)ga(WF_GAME_WNDPROC)) return;
    if (GetWindowLongW(w, GWL_WNDPROC) != cls) SetWindowLongW(w, GWL_WNDPROC, cls);
}

// Replaces the viewport tick (vtable slot WF_VP_TICK_SLOT), called by the game once per frame on
// its own thread. Its device state at +0xB0 is the engine's lost-device machine: 1 = flush
// loading and release (-> 2), 2 = wait until Reset is allowed, 3 = Reset with the stored present
// parameters (-> 0). During a switch TestCooperativeLevel returns D3D_OK, and then the engine
// leaves that state alone, so setting it walks the engine's own release/Reset code:
//   frame 1: Windowed flag changed, state 1 -> the engine releases its device resources;
//   frame 2: state 3 -> the engine resets into the new mode and recreates them.
// A refused Reset puts the old flag back; the engine's own retry then resets in the old mode.
static char __fastcall wf_vp_tick_hook(void* vp, void* edx)
{
    wf_vp_tick_fn real = (wf_vp_tick_fn)g_wfVpTick;
    uintptr_t pp = ga(WF_PP);
    volatile LONG* state = (volatile LONG*)((char*)vp + WF_VP_STATE);
    volatile BOOL* windowed = (volatile BOOL*)(pp + WF_PP_OFF_WINDOWED);
    LONG want = g_fsWant;
    char r;

    InterlockedIncrement(&g_fsTicks);
    if (GetCurrentThreadId() == g_mainTid) ko_frame();
    if (g_ddSync && GetCurrentThreadId() != g_mainTid) g_ddOffThread = 1;
    else if (g_ddSync && (wf_real_write || wf_real_write_gog) && InterlockedExchange(&g_ddSync, 0)) {
        float level = (float)g_preset;

        // The trampoline skips the hook, so this is not taken for a menu choice. GOG's writer
        // takes the config manager in ECX, the value Steam's reads from the same global.
        if (wf_real_write_gog) {
            void* config = read_ptr((void*)ga(WF_CONFIG));
            if (config) wf_real_write_gog(config, NULL, L"User", L"Rendering", L"EngineFixDrawDistance", &level);
        } else {
            wf_real_write(L"User", L"Rendering", L"EngineFixDrawDistance", &level);
        }
    }
    if (g_shadowSync && GetCurrentThreadId() == g_mainTid && (wf_real_write || wf_real_write_gog) &&
        InterlockedExchange(&g_shadowSync, 0)) {
        float level = (float)g_shadow;

        if (wf_real_write_gog) {
            void* config = read_ptr((void*)ga(WF_CONFIG));
            if (config) wf_real_write_gog(config, NULL, L"User", L"Rendering", L"EngineFixShadowQuality", &level);
        } else {
            wf_real_write(L"User", L"Rendering", L"EngineFixShadowQuality", &level);
        }
    }
    // A new atlas size walks the same machine as a full screen switch, with the mode unchanged:
    // release, then Reset, where shadow_atlas_alloc applies the size.
    if (g_shadowStage == 0 && g_shadowLive && g_fsStage == 0 && want < 0 && *state == 0 &&
        g_shadowAtlasWant != g_shadowAtlasNow) {
        InterlockedExchange(&g_shadowAllocRan, 0);
        InterlockedExchange(&g_shadowStage, 1);
        *state = 1;
        return real(vp, edx);
    }
    if (g_shadowStage) {
        if (*state != 0 && *state != 2) return real(vp, edx);
        if (*state == 2) {
            *state = 3;
            r = real(vp, edx);
        } else {
            r = real(vp, edx);          // the release never happened
        }
        shadow_switch_end();
        return r;
    }
    if (g_fsStage == 0) {
        if (want < 0 || *state != 0 || wf_engine_fullscreen() < 0) return real(vp, edx);
        if ((*windowed == FALSE) == (want != 0)) {   // already in that mode
            InterlockedCompareExchange(&g_fsWant, -1, want);
            return real(vp, edx);
        }
        g_fsPrevWindowed = *windowed;
        *windowed = want ? FALSE : TRUE;
        *state = 1;
        InterlockedExchange(&g_fsStage, 1);
        return real(vp, edx);
    }
    if (*state == 0) {              // the release never happened: only the flag to undo
        *windowed = g_fsPrevWindowed;
        InterlockedExchange(&g_fsResult, -1);
        InterlockedExchange(&g_fsWant, -1);
        InterlockedExchange(&g_fsStage, 0);
        return real(vp, edx);
    }
    if (*state != 2) return real(vp, edx);
    wf_reset_wndproc(*(HWND volatile*)(pp + WF_PP_OFF_HWND));
    if (*windowed == FALSE) wf_fullscreen_window();
    *state = 3;
    r = real(vp, edx);
    // Out of full screen: drop whatever DXVK left behind. Into it: DXVK's hook is wanted.
    if (*state == 0 && *windowed) wf_reset_wndproc(*(HWND volatile*)(pp + WF_PP_OFF_HWND));
    if (*state == 0) {
        InterlockedExchange(&g_fsResult, *windowed ? 2 : 1);
        InterlockedCompareExchange(&g_fsWant, -1, *windowed ? 0 : 1);
    } else {
        *windowed = g_fsPrevWindowed;
        InterlockedExchange(&g_fsResult, -1);
        InterlockedExchange(&g_fsWant, -1);
    }
    InterlockedExchange(&g_fsStage, 0);
    return r;
}

// Worker tick: the Draw distance row's choice. Like Ctrl+F10 but silent, the menu shows it.
static void dd_tick(void)
{
    static int off_logged;
    LONG level = InterlockedExchange(&g_ddMenu, -1);

    if (g_ddOffThread && !off_logged) {
        off_logged = 1;
        logf_("presets: viewport tick not on the main thread - the Draw distance row is not kept in step with Ctrl+F10");
    }
    if (level < 0 || level >= PRESET_COUNT || level == g_preset) return;
    select_preset((int)level);
    apply_lod();
    logf_("preset: %s mesh=%.2f foliage=%.2f visibility=%.2fx shadow=%.2fx billboards=%.2fx (options menu)",
          cfg_presets[g_preset].name, cfg_mesh, cfg_foliage, cfg_visibility, cfg_shadow_range, ef_billboard_factor);
}

// Worker tick: the switch results, then the wanted style against the live window.
static void wf_tick(void)
{
    static LONG seen_ticks;
    static DWORD pending_since;
    static int force;
    HWND w = wf_window();
    LONG style, ex, hint, res;
    int fullscreen, borderless, topmost, want, engine;

    if (!w) return;
    if (g_wfApplied == -1) {
        // First sight of the window: its client area is still the one it was born with.
        RECT c;

        if (GetClientRect(w, &c)) {
            logf_("window: game window %ldx%ld, User.ini viewport %dx%d", c.right, c.bottom,
                  g_wfCx, g_wfCy);
            if (g_wfCx <= 0 && c.right > 0 && c.bottom > 0) { g_wfCx = c.right; g_wfCy = c.bottom; }
        }
    }

    res = InterlockedExchange(&g_fsResult, 0);
    if (res == 1) {
        logf_("window: full screen on (options menu)");
    } else if (res == 2) {
        logf_("window: full screen off (options menu)");
        force = 1;   // the window still has the full screen style: give it the windowed one
    } else if (res < 0) {
        logf_("window: Reset into the new mode was refused; the old mode is kept");
        osd_show("Display mode: could not switch, restart the game to apply");
    }

    // A switch the viewport tick never picks up (the game is not rendering) must not keep the
    // window locked: after 3 s it is left for the next start.
    if (g_fsWant >= 0 && !g_fsStage) {
        if (!pending_since || g_fsTicks != seen_ticks) {
            pending_since = GetTickCount();
            seen_ticks = g_fsTicks;
        } else if (GetTickCount() - pending_since > 3000) {
            InterlockedExchange(&g_fsWant, -1);
            pending_since = 0;
            logf_("window: full screen switch not picked up by the game; it applies at the next start");
            osd_show("Display mode: restart the game to apply");
        }
        return;
    }
    pending_since = 0;
    if (g_fsStage) return;   // the viewport tick is switching: hands off the window

    style = GetWindowLongW(w, GWL_STYLE);
    ex = GetWindowLongW(w, GWL_EXSTYLE);
    topmost = (ex & WS_EX_TOPMOST) != 0;
    engine = wf_engine_fullscreen();
    fullscreen = engine >= 0 ? engine : ((style & WS_POPUP) != 0 && topmost);
    borderless = (style & WS_POPUP) != 0 && !topmost;
    want = wf_borderless ? 1 : 0;

    hint = InterlockedExchange(&g_wfFullscreenHint, 0);
    if (hint && ((hint > 0 && !fullscreen) || (hint < 0 && fullscreen))) {
        osd_show("Full screen: restart the game to apply");
        logf_("window: Full screen changed in the options menu; it applies at the next start");
    }

    if (fullscreen) {
        if (want && !g_fsHooked && !g_wfWarnedFullscreen) {
            g_wfWarnedFullscreen = 1;
            osd_show("Borderless window: turn Full screen off, then restart");
            logf_("window: borderless is on but the game runs in full screen; set Full screen off (restart)");
        }
        g_wfApplied = -2;
        return;
    }
    g_wfWarnedFullscreen = 0;
    if (IsIconic(w) || (!force && borderless == want && !topmost)) { g_wfApplied = borderless; return; }

    wf_restyle(w, want);
    g_wfApplied = want;
    {
        LONG from = InterlockedExchange(&g_wfFrom, 0);
        const char* src = (from == 1 || force) ? "options menu" : "startup";
        LONG now = GetWindowLongW(w, GWL_STYLE);

        force = 0;
        if (((now & WS_POPUP) != 0) != (want != 0)) {
            logf_("window: restyle failed (style 0x%08lX)", (unsigned long)now);
            return;
        }
        // No on-screen line: the Display mode row already shows the choice.
        logf_("window: borderless %s (%s)", want ? "on" : "off", src);
    }
}

// Viewport tick slot -> wf_vp_tick_hook. A data write (the vtable is in .rdata), aligned and
// atomic, so no thread has to be stopped. Fingerprinted: the slot must hold the tick, the tick
// must read its state from +0xB0, and the Reset must copy the present parameters from WF_PP.
// `mov ecx,0Eh; mov esi,WF_PP` in the Reset: the operand is checked against ga(WF_PP), since the
// loader relocates it on GOG.
static int wf_reset_copy_ok(void)
{
    uintptr_t at = ga(WF_RESET_COPY);
    return code_matches(at, "b9 0e 00 00 00 be") && range_readable((void*)at, 10) &&
           *(const DWORD*)(at + 6) == (DWORD)ga(WF_PP);
}

static int wf_install_vp_tick(void)
{
    volatile LONG* slot = (volatile LONG*)ga(WF_VP_TICK_SLOT);
    uintptr_t tick = ga(WF_VP_TICK);
    DWORD old = 0, tmp = 0;
    LONG prev;

    if (!slot || !tick || !ga(WF_PP) || (uint32_t)*slot != (uint32_t)tick) return 0;
    if (!code_matches(tick, "55 8b ec 83 e4 f8 56 57 8b f9 8b c7 e8") ||
        !code_matches(tick + 0x11, "8b 87 b0 00 00 00") ||
        !wf_reset_copy_ok())
        return 0;
    if (!VirtualProtect((void*)slot, sizeof(LONG), PAGE_READWRITE, &old)) return 0;
    g_wfVpTick = tick;
    prev = InterlockedCompareExchange(slot, (LONG)(uintptr_t)wf_vp_tick_hook, (LONG)tick);
    VirtualProtect((void*)slot, sizeof(LONG), old, &tmp);
    return (uint32_t)prev == (uint32_t)tick;
}

// The shipped exe is SteamStub protected: .text is encrypted until the stub jumps to the real
// entry point, which happens after every static DLL's DllMain has run. A site only reads as its
// real bytes once that has happened, so wait for the fingerprint instead of assuming it.
static int wf_wait_site(uintptr_t at, const char* hex, int timeout_ms)
{
    DWORD t0 = GetTickCount();

    while (g_running && (long)(GetTickCount() - t0) < timeout_ms) {
        if (code_matches(at, hex)) return 1;
        Sleep(1);
    }
    return 0;
}

// The peer-stop barrier can refuse while the process is still creating threads (a new thread in
// every freeze pass makes it fail; the writer site 3 ms later is proof of how transient that is,
// see the 2026-11-02 crash log). Retry briefly. The birth style only matters before the window
// exists: once it is there, stop and let the restyle path take over.
static int wf_install_style_before_window(void)
{
    DWORD t0 = GetTickCount();

    do {
        if (wf_install_style()) return 1;
        if (wf_window()) return 0;
        Sleep(1);
    } while (g_running && (long)(GetTickCount() - t0) < 3000);
    return 0;
}

static int wf_install_writer_retry(void)
{
    DWORD t0 = GetTickCount();

    do {
        if (wf_install_writer()) return 1;
        Sleep(1);
    } while (g_running && (long)(GetTickCount() - t0) < 3000);
    return 0;
}

// Started from DllMain: read the stored option, make the process DPI-aware before the game
// creates its window, and install both patches. A thread, not DllMain, because shell and loader
// APIs must not be called under the loader lock.
static DWORD WINAPI window_init_thread(LPVOID param)
{
    int found = 0, on;

    (void)param;
    on = wf_user_ini_borderless(&found);
    if (!found) on = cfg_wf_borderless;
    wf_user_ini_resolution();
    if (cfg_preset_mode) {
        int level = wf_user_ini_draw_distance();

        if (level >= 0) {
            select_preset(level);
            logf_("presets: startup=%s (options menu)", cfg_presets[g_preset].name);
        }
        InterlockedExchange(&g_ddSync, 1);   // the row opens on the level in use
    }
    wf_state(on);
    if (on && cfg_wf_dpi) wf_set_dpi_aware();
    logf_("window: borderless=%d (%s), dpi_aware=%d, menu=%d",
          on, found ? "options menu" : "ini", cfg_wf_dpi, cfg_window_mode);
    {
        // The engine calls into this DLL through the patched sites: keep it loaded.
        HMODULE pinned;

        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_PIN,
                                (LPCSTR)(uintptr_t)wf_style_stub, &pinned)) {
            logf_("window: could not pin the DLL; the window patches are not installed");
            return 0;
        }
        if (!wf_wait_site(ga(WF_STYLE_SITE), "bf 00 00 c8 00", 60000)) {
            logf_("window: window style site never appeared - borderless off for this start");
        } else if (!wf_install_style_before_window()) {
            if (wf_window())
                logf_("window: window created before the style patch - borderless applies by restyle");
            else
                logf_("window: startup style not patched (bytes differ or out of range) - borderless would appear after creation");
        } else {
            logf_("window: startup borderless style patched");
        }
        if (!wf_wait_site(ga(WF_WRITER_SITE), wf_writer_bytes(), 60000)) {
            logf_("window: config writer site never appeared - the menu setting applies at the next start");
        } else if (!wf_install_writer_retry()) {
            logf_("window: menu channel not hooked (bytes differ or out of range) - the menu setting applies at the next start");
        } else {
            logf_("window: menu channel hooked (Options > Graphics applies borderless at once)");
            // Full screen follows the menu live only through the menu channel.
            if (!wf_wait_site(ga(WF_RESET_COPY), "b9 0e 00 00 00 be", 60000) ||
                !wf_install_vp_tick()) {
                logf_("window: viewport tick not hooked (bytes differ) - Full screen applies at the next start");
            } else {
                g_fsHooked = 1;
                g_shadowLive = 1;
                logf_("window: viewport tick hooked (Full screen switches at once)");
            }
        }
    }
    return 0;
}

// [startup] fast_start=1. While FMOD loads the music system the whole process idles for ~11 s, in
// FMOD's polling sleeps. Its file layer waits for each read from the file thread in a Sleep(10) loop
// (fmodex.dll 100140D1), and the main thread waits for a free async slot in another one
// (fmod_event.dll 10036261), and a Sleep(1) lasts 15.6 ms until something raises the timer around
// the time the game window opens. So: a 1 ms timer from the start, and Sleep(1) for those waits until
// the window is there. See docs/fixes/startup-time.md. The timer is held until the process ends: the
// running game has a 0.5 ms timer anyway, from a request that is not ours.
static DWORD g_startMainTid;              // DllMain runs on the game's main thread
static volatile LONG g_startDone;         // the game window is there: FMOD's sleeps stay as they are
static volatile LONG g_startCutEvent, g_startCutEx;
static VOID (WINAPI *g_realFmodSleep)(DWORD);

static VOID WINAPI hook_FmodEventSleep(DWORD ms)
{
    if (ms == 10 && !g_startDone && GetCurrentThreadId() == g_startMainTid) {
        InterlockedIncrement(&g_startCutEvent);
        ms = 1;
    }
    g_realFmodSleep(ms);
}

static VOID WINAPI hook_FmodExSleep(DWORD ms)
{
    if (ms == 10 && !g_startDone) {
        InterlockedIncrement(&g_startCutEx);
        ms = 1;
    }
    g_realFmodSleep(ms);
}

static void fmod_sleep_cb(void* ctx, const char* dll, const char* fn, uintptr_t slot)
{
    if (!_stricmp(dll, "kernel32.dll") && !strcmp(fn, "Sleep")) *(uintptr_t*)ctx = slot;
}

static int fmod_sleep_hook(const char* module, void* hook)
{
    HMODULE mod = NULL;
    uintptr_t slot = 0;
    int i;

    for (i = 0; i < 2000 && g_running && !mod; i++) {
        mod = GetModuleHandleA(module);
        if (!mod) Sleep(5);
    }
    if (!mod) { logf_("startup: %s not loaded after 10 s - its waits stay as they are", module); return 0; }
    imports_walk((const unsigned char*)mod, fmod_sleep_cb, &slot);
    return patch_import_slot("startup", "kernel32.dll", "Sleep", slot, hook, (void**)&g_realFmodSleep);
}

// Hooks FMOD's two Sleep imports, and ends the shortening when the game window appears.
static void fast_start_fmod(void)
{
    DWORD t0 = GetTickCount();
    int ex = fmod_sleep_hook("fmodex.dll", (void*)hook_FmodExSleep);
    int ev = fmod_sleep_hook("fmod_event.dll", (void*)hook_FmodEventSleep);
    int i;

    if (!ex && !ev) return;
    for (i = 0; i < 12000 && g_running; i++) {
        HWND w = FindWindowW(L"W2ViewportClass", NULL);
        DWORD pid = 0;
        if (w) GetWindowThreadProcessId(w, &pid);
        if (w && pid == GetCurrentProcessId()) break;
        Sleep(5);
    }
    InterlockedExchange(&g_startDone, 1);
    logf_("startup: fast_start: FMOD waits of 10 ms cut to 1 ms until the game window, after %lu ms: fmodex %ld, fmod_event %ld",
          GetTickCount() - t0, g_startCutEx, g_startCutEvent);
}

// [saves] light_previews=1. The first Load menu build hands every save's 640x360 BMP preview to
// D3DXCreateTextureFromFileInMemory, whose defaults scale it to 1024x512 and generate a full mip
// chain: ~18 ms each on the render thread, with the main thread waiting. The same picture at its own
// size with one level needs neither step. Other images pass through. See docs/fixes/startup-time.md.
#define D3DX_NONPOW2 ((UINT)-2)
#define D3DX_NO_FILTER 1u
#define D3DPOOL_MANAGED_ 1u
typedef struct { UINT w, h, depth, mips; DWORD fmt, restype, filefmt; } lp_info;
typedef HRESULT (WINAPI *lp_create_t)(void*, const void*, UINT, void**);
typedef HRESULT (WINAPI *lp_createex_t)(void*, const void*, UINT, UINT, UINT, UINT, DWORD, DWORD, DWORD, DWORD, DWORD,
                                        DWORD, void*, void*, void**);
typedef HRESULT (WINAPI *lp_info_t)(const void*, UINT, lp_info*);
static lp_create_t g_lpCreate;
static lp_createex_t g_lpCreateEx;
static lp_info_t g_lpInfo;
static volatile LONG g_lpLight, g_lpFallback;

static HRESULT WINAPI hook_lpCreate(void* dev, const void* data, UINT size, void** out)
{
    lp_info ti;

    if (g_lpInfo(data, size, &ti) >= 0 && ti.filefmt == 0 /* D3DXIFF_BMP */ && ti.w == 640 && ti.h == 360) {
        HRESULT hr = g_lpCreateEx(dev, data, size, D3DX_NONPOW2, D3DX_NONPOW2, 1, 0, 0 /* format from the file */,
                                  D3DPOOL_MANAGED_, D3DX_NO_FILTER, D3DX_NO_FILTER, 0, NULL, NULL, out);
        if (hr >= 0) {
            LONG n = InterlockedIncrement(&g_lpLight);
            if (n == 1) logf_("saves: first preview made at its own size");
            if (cfg_savelist_trace && (n == 1 || n % 50 == 0)) {
                char what[40];
                sprintf_s(what, sizeof what, "light preview %ld:", n);
                log_mem(what);
            }
            return hr;
        }
        if (InterlockedIncrement(&g_lpFallback) == 1) logf_("saves: light preview failed (%08lX) - the game's own", (unsigned long)hr);
    }
    return g_lpCreate(dev, data, size, out);
}

static void lp_slot_cb(void* ctx, const char* dll, const char* fn, uintptr_t slot)
{
    if (!_stricmp(dll, "d3dx9_39.dll") && !strcmp(fn, "D3DXCreateTextureFromFileInMemory")) *(uintptr_t*)ctx = slot;
}

static DWORD WINAPI light_previews_thread(LPVOID param)
{
    uintptr_t slot = 0;
    HMODULE d3dx;

    (void)param;
    Sleep(1500);   // the SteamStub fills the exe's import table during start-up
    d3dx = GetModuleHandleA("d3dx9_39.dll");
    if (d3dx) {
        g_lpInfo = (lp_info_t)(void*)GetProcAddress(d3dx, "D3DXGetImageInfoFromFileInMemory");
        g_lpCreateEx = (lp_createex_t)(void*)GetProcAddress(d3dx, "D3DXCreateTextureFromFileInMemoryEx");
    }
    if (!g_lpInfo || !g_lpCreateEx) {
        logf_("saves: d3dx9_39.dll functions not found - light_previews off");
        return 0;
    }
    imports_walk((const unsigned char*)g_base, lp_slot_cb, &slot);
    if (patch_import_slot("saves", "d3dx9_39.dll", "D3DXCreateTextureFromFileInMemory", slot, (void*)hook_lpCreate, (void**)&g_lpCreate))
        logf_("saves: light previews on (640x360 BMP at its own size, one level)");
    return 0;
}

static double timed_sleep1_ms(void)
{
    LARGE_INTEGER f, a, b;

    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    Sleep(1);
    QueryPerformanceCounter(&b);
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
}

static DWORD WINAPI fast_start_thread(LPVOID param)
{
    typedef UINT (WINAPI *timeBeginPeriod_t)(UINT);
    HMODULE winmm;
    timeBeginPeriod_t begin;
    double before, after;
    UINT res;

    (void)param;
    winmm = LoadLibraryW(L"winmm.dll");
    begin = winmm ? (timeBeginPeriod_t)(void*)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
    if (!begin) {
        logf_("startup: timeBeginPeriod not found - fast_start off");
        return 0;
    }
    before = timed_sleep1_ms();
    res = begin(1);
    after = timed_sleep1_ms();
    logf_("startup: fast_start: timeBeginPeriod(1) = %u, Sleep(1) took %.1f ms before, %.1f ms after", res, before, after);
    fast_start_fmod();
    return 0;
}

static DWORD WINAPI worker(LPVOID param)
{
    (void)param;
    Sleep(1500);   // let the loader finish; never touch loader APIs during DllMain
    crash_init();
    // Wait for the engine to be up before touching anything.
    for (int i = 0; i < 600 && g_running; i++) {
        if (GetModuleHandleA("witcher2.exe") || GetModuleHandleA("witcher2.EXE")) break;
        Sleep(100);
    }
    osd_init();
    while (g_running) {
        static int skip_logged;
        if (!skip_logged && g_skipDone) {
            skip_logged = 1;
            logf_("startup: %ld startup movie(s) skipped without starting them, %s", g_skipStops,
                  g_skipDone == 1 ? "main menu reached" : "gave up after 60 s");
        }
        crash_tick();
        ray_guard_tick();
        quit_guard_tick();
        quit_guard_report();
        clean_quit_tick();
        path_guard_tick();
        storage_leak_tick();
        text_guard_tick();
        lscr_tick();
        osd_pump();
        dd_tick();
        apply_lod();
        if (cfg_wf_enabled) wf_tick();
        if (cfg_hotkeys) poll_hotkeys();
        distance_tick();
        cascade_tick();
        save_tick();
        doors_tick();
        osd_tick();
        Sleep(120);
    }
    return 0;
}

// ---- keyboard filter ----------------------------------------------------
// F9/F10 are not in Input_QWERTY.ini, so they cannot be rebound - the engine has its
// own quick-load/whatever on them. Ctrl+F9 would therefore still fire. Because we own
// DirectInput8Create we can patch the vtable slots of the objects the game receives:
// IDirectInput8::CreateDevice (slot 3) to see each device, then
// IDirectInputDevice8::GetDeviceState (slot 9) to clear F9/F10 in the 256-byte keyboard
// state whenever Ctrl or Alt is held.

#define DIK_F9_CODE  0x43
#define DIK_F10_CODE 0x44

typedef HRESULT (WINAPI *GDS_t)(void* self, DWORD cb, void* data);

// Each patched DirectInput vtable keeps its own original. On Steam only the game creates
// DirectInput objects (one IDirectInput8W, one keyboard); on GOG other modules in the process
// create some too, and an A and a W interface have different vtables whose CreateDevice
// differ, so one original saved for all of them would call the wrong function. An entry is
// published before its slot points at the hook, under g_vtLock (set up in DllMain).
#define VT_MAX 16
typedef struct { void** vt; int index; void* orig; } vt_hook;
static vt_hook g_vtHooks[VT_MAX];
static volatile LONG g_vtCount;
static CRITICAL_SECTION g_vtLock;
static int g_diLogged, g_kbLogged;   // first DirectInput8Create and keyboard calls are logged

// The original behind slot `index` of obj's vtable; NULL when that slot is not ours.
static void* vt_orig(void* obj, int index)
{
    void** vt = obj ? *(void***)obj : NULL;
    LONG i, n = g_vtCount;
    for (i = 0; i < n; i++)
        if (g_vtHooks[i].vt == vt && g_vtHooks[i].index == index) return g_vtHooks[i].orig;
    return NULL;
}

// 2 = hooked now, 1 = this vtable was already hooked, 0 = not hooked.
static int vt_hook_slot(void* obj, int index, void* fn)
{
    void** vt = obj ? *(void***)obj : NULL;
    DWORD old = 0, tmp = 0;
    int r = 0;
    if (!vt) return 0;
    EnterCriticalSection(&g_vtLock);
    if (vt[index] == fn) {
        r = 1;
    } else if (g_vtCount < VT_MAX &&
               VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) {
        g_vtHooks[g_vtCount].vt = vt;
        g_vtHooks[g_vtCount].index = index;
        g_vtHooks[g_vtCount].orig = vt[index];
        InterlockedIncrement(&g_vtCount);
        vt[index] = fn;
        VirtualProtect(&vt[index], sizeof(void*), old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), &vt[index], sizeof(void*));
        r = 2;
    }
    LeaveCriticalSection(&g_vtLock);
    return r;
}

static HRESULT WINAPI hook_GetDeviceState(void* self, DWORD cb, void* data)
{
    GDS_t orig = (GDS_t)vt_orig(self, 9);
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, cb, data);
    if (SUCCEEDED(hr) && data && cb == 256 && cfg_suppress) {
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_MENU) & 0x8000)) {
            ((unsigned char*)data)[DIK_F9_CODE]  = 0;
            ((unsigned char*)data)[DIK_F10_CODE] = 0;
        }
    }
    return hr;
}

static int patch_vtable(void* obj, int index, void* fn, void** orig)
{
    if (!obj) return 0;
    void** vt = *(void***)obj;
    if (!vt) return 0;
    if (vt[index] == fn) return 1;                      // already ours
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_READWRITE, &old)) return 0;
    if (orig && *orig == NULL) *orig = vt[index];
    vt[index] = fn;
    VirtualProtect(&vt[index], sizeof(void*), old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), &vt[index], sizeof(void*));
    return 1;
}

// IDirectInput8::CreateDevice takes THREE parameters plus this:
//   HRESULT CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDevice, LPUNKNOWN pUnkOuter)
// Reading it as (rguid, outer, riid, out) made the hook patch a garbage pointer.
typedef HRESULT (WINAPI *CD_t)(void* self, const GUID* rguid, void** out, void* outer);

// GUID_SysKeyboard, spelled out so the DLL needs no dinput8 import library.
// Only the keyboard is hooked: mouse and joystick are different classes with their
// own GetDeviceState implementations, and reusing one saved original for all of them
// would call the wrong function.
static const GUID kSysKeyboard =
    { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

// ---- Windows key combinations in game ([hotkeys] windows_keys) -----------
// The game reads the keyboard two ways (FUN_009d24f8 sets the input mode, window object +0x834):
// in the menus through WM_KEYDOWN/WM_KEYUP (window procedure FUN_009d4bd9 -> FUN_009d243e), and
// in mode 3 (play) through a DirectInput keyboard it creates for that mode, acquired with
// DISCL_EXCLUSIVE | DISCL_FOREGROUND (push 5 at 0x009D25EE) and polled with GetDeviceData and
// GetDeviceState (FUN_009d2817). An exclusive keyboard makes every Windows key combination dead
// while the game is in front, and with it Print Screen when Windows opens the Snipping Tool with
// it (Win+Shift+S, Print Screen). Witcher 1 had the same symptom from DISCL_NOWINKEY (its engine
// fix's QoL 6); here both are dropped: the keyboard's SetCooperativeLevel (IDirectInputDevice8
// slot 13: HWND, DWORD flags) gets DISCL_NONEXCLUSIVE without DISCL_NOWINKEY.
//
// A non-exclusive keyboard also lets Windows send the window its key messages, which an exclusive
// one did not, so in mode 3 the engine would get every key twice and Alt or F10 would open the
// window's system menu. The window object's procedure (vtable slot WK_WNDPROC_SLOT) is therefore
// hooked: in mode 3 it drops WM_KEYFIRST..WM_KEYLAST, so the engine sees exactly what it saw
// before. The cooperative level is only changed once that hook is in place.
//
// Without the block a Windows key pressed and released on its own would open the Start menu, so
// a thread watches the key while the game is in front: as soon as it goes down, an unassigned key
// (0xE8, as AutoHotkey does) is pressed and released, and the shell takes the release as the end
// of a combination. Combinations work as usual.

#define WK_DISCL_EXCLUSIVE    0x01
#define WK_DISCL_NONEXCLUSIVE 0x02
#define WK_DISCL_NOWINKEY     0x10
#define WK_MASK               0xE8         // unassigned virtual key
#define WK_POLL               5            // ms; a tap of the key lasts far longer
#define WK_WNDPROC_SLOT       0x00EC6560u  // window object vtable slot 0 -> FUN_009D4BD9
#define WK_WNDPROC            0x009D4BD9u  // __thiscall (msg, wparam, lparam), RET 0xC
#define WK_INPUT_MODE         0x834        // window object: input mode, 3 = DirectInput

typedef HRESULT (WINAPI *SCL_t)(void* self, HWND w, DWORD flags);
typedef LRESULT (__fastcall *wk_wndproc_fn)(void* obj, void* edx, UINT msg, WPARAM wp, LPARAM lp);
static uintptr_t g_wkWndProc;   // this build's FUN_009D4BD9, set before its slot is swapped
static int g_wkThread, g_wkLogged, g_wkProcHooked = -1;
static void* volatile g_wkKeyboard;   // the keyboard the game created last

// The window object's procedure: in DirectInput mode the keyboard comes from DirectInput only.
static LRESULT __fastcall wk_window_proc(void* obj, void* edx, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST &&
        *(volatile LONG*)((char*)obj + WK_INPUT_MODE) == 3)
        return 0;
    return ((wk_wndproc_fn)g_wkWndProc)(obj, edx, msg, wp, lp);
}

// Slot write in .rdata, aligned and atomic; fingerprinted by the slot value, the procedure's
// prologue (push ebp; mov ebp,esp; and esp,-8; sub esp,0x64) and its RET 0xC.
static int wk_hook_window_proc(void)
{
    volatile LONG* slot = (volatile LONG*)ga(WK_WNDPROC_SLOT);
    uintptr_t proc = ga(WK_WNDPROC);
    DWORD old = 0, tmp = 0;
    LONG prev;

    if (g_wkProcHooked >= 0) return g_wkProcHooked;
    g_wkProcHooked = 0;
    if (!slot || !proc || (uint32_t)*slot != (uint32_t)proc ||
        !code_matches(proc, "55 8b ec 83 e4 f8 83 ec 64") ||
        !code_matches(proc + 0x2F2, "c2 0c 00") ||
        !VirtualProtect((void*)slot, sizeof(LONG), PAGE_READWRITE, &old)) {
        logf_("input: window procedure not hooked (bytes differ) - the keyboard stays exclusive");
        return 0;
    }
    g_wkWndProc = proc;
    prev = InterlockedCompareExchange(slot, (LONG)(uintptr_t)wk_window_proc, (LONG)proc);
    VirtualProtect((void*)slot, sizeof(LONG), old, &tmp);
    g_wkProcHooked = (uint32_t)prev == (uint32_t)proc;
    logf_("input: window procedure %s", g_wkProcHooked ? "hooked (key messages ignored while DirectInput reads the keyboard)"
                                                       : "not hooked - the keyboard stays exclusive");
    return g_wkProcHooked;
}

static DWORD WINAPI winkey_thread(LPVOID param)
{
    int was_down = 0;

    (void)param;
    while (g_running) {
        int down = (GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) < 0;

        if (down && !was_down) {
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;

            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId()) {
                INPUT in[2];

                ZeroMemory(in, sizeof in);
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = WK_MASK;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
                if (!g_wkLogged) {
                    g_wkLogged = 1;
                    logf_("input: Windows key pressed in game, masked (no Start menu on its own)");
                }
            }
        }
        was_down = down;
        Sleep(down ? 20 : WK_POLL);
    }
    return 0;
}

static HRESULT WINAPI hook_SetCooperativeLevel(void* self, HWND w, DWORD flags)
{
    SCL_t orig = (SCL_t)vt_orig(self, 13);
    if (!orig) return E_FAIL;
    // The slot may be shared with the mouse's class: only the keyboard is changed.
    if (self == g_wkKeyboard && (flags & (WK_DISCL_EXCLUSIVE | WK_DISCL_NOWINKEY)) &&
        wk_hook_window_proc()) {
        static int logged;
        DWORD want = (flags & ~(WK_DISCL_EXCLUSIVE | WK_DISCL_NOWINKEY)) | WK_DISCL_NONEXCLUSIVE;

        if (!logged) {
            logged = 1;
            logf_("input: keyboard cooperative level 0x%02lX -> 0x%02lX (Windows keys and Print Screen work in game)",
                  (unsigned long)flags, (unsigned long)want);
        }
        flags = want;
        if (!g_wkThread) {
            HANDLE t = CreateThread(NULL, 0, winkey_thread, NULL, 0, NULL);

            if (t) { g_wkThread = 1; CloseHandle(t); }
        }
    }
    return orig(self, w, flags);
}

static HRESULT WINAPI hook_CreateDevice(void* self, const GUID* rguid, void** out, void* outer)
{
    CD_t orig = (CD_t)vt_orig(self, 3);
    void* from = _ReturnAddress();
    if (!orig) return E_FAIL;
    HRESULT hr = orig(self, rguid, out, outer);
    if (rguid && memcmp(rguid, &kSysKeyboard, sizeof(GUID)) == 0 && (g_kbLogged++ < 4 || FAILED(hr))) {
        char who[MAX_PATH + 16];
        crash_where((DWORD)(uintptr_t)from, who, sizeof who);
        logf_("input: keyboard device created by %s (hr %08lX)", who, (unsigned long)hr);
    }
    if (SUCCEEDED(hr) && out && *out && rguid && memcmp(rguid, &kSysKeyboard, sizeof(GUID)) == 0) {
        if (cfg_suppress && vt_hook_slot(*out, 9, (void*)hook_GetDeviceState))
            logf_("input: keyboard state filter installed");
        if (cfg_winkeys) {
            static int logged;

            g_wkKeyboard = *out;
            if (vt_hook_slot(*out, 13, (void*)hook_SetCooperativeLevel) && !logged) {
                logged = 1;
                logf_("input: keyboard cooperative level hook installed");
            }
        }
    }
    return hr;
}

// ---- builds and addresses -----------------------------------------------
// Two builds of 3.5.0.26 are in use and they are not the same binary: Steam's (linked 2013,
// SteamStub, no ASLR) and GOG's 3.5.0.26g (relinked 2023, not packed, ASLR on, Galaxy.dll
// instead of steam_api.dll); see docs/fixes/gog-build.md. The build is read from the loaded
// exe's own header in DllMain: the PE TimeDateStamp and the size of .text, which the
// SteamStub leaves as they are. Anything else is "unknown" and gets only what needs no
// game address.
#define STEAM_TIMESTAMP 0x518B881Cu
#define STEAM_TEXT_SIZE 0x008A94ADu
#define GOG_TIMESTAMP   0x644689F9u
#define GOG_TEXT_SIZE   0x008FC18Du

static DWORD g_buildStamp, g_buildText;

// Every fixed game address above, by its Steam name, with the GOG 3.5.0.26g value at the
// preferred base 0x00400000 (an RVA for the EF_ and RVA_ names, like their Steam values).
// 0 = not found on GOG yet, and build_gate() keeps the features that need it off there.
// Fill a value in only once the site is proven on GOG (docs/fixes/gog-build.md); its byte
// checks are Steam's and must be made to hold there too. tools/address-table.py copies this
// column to docs/addresses.md and fails when an address #define has no row here.
typedef struct { uintptr_t steam, gog; } ga_row;
static const ga_row k_gog[] = {
    { EF_RANGE_RVA,          0x0060C120u },   // CRenderProxy_Mesh distance, 00A0BE2B
    { EF_SHADOW_RVA,         0x0060C4F6u },
    { VA_BILLBOARD_SHOW,     0x00A0D95Fu },   // same helper, 00A0D945, called from the proxy constructor 00A0D995
    { VA_SUN_ATLAS_ALLOC,    0x00B4C301u },   // same function, read and compared
    { VA_SUN_ATLAS_CALL,     0x00A2F0CDu },   // in the device reset 00A2F067
    { VA_SUN_POISSON_SET,    0x00A21FF6u },
    { VA_SUN_POISSON_CALL,   0x00B256E0u },   // in the same loop in 00B2558C
    { VA_SUN_ATLAS_SIZE,     0x00A2DBEDu },   // same store in GOG's renderer constructor
    { VA_RENDERER,           0x0167D4DCu },
    { VA_SUN_CASCADE_FIRST,  0x00A111CDu },   // same interval loop as Steam, loads 00F781F8
    { VA_SUN_CASCADE_FIRST_LEN, 0x00F781F8u },
    { VA_SUN_CASCADE_MID,    0x00A111FDu },   // same interval loop as Steam, loads 00F78620
    { VA_SUN_CASCADE_MID_LEN, 0x00F78620u },
    { VA_SUN_CASCADE_FAR,    0x00A1120Du },   // same interval loop as Steam, loads 00F781D0
    { VA_SUN_CASCADE_FAR_LEN, 0x00F781D0u },
    { RVA_SETTINGS,          0x021EAEB8u },   // [025EAEB8] -> object 025EAF08, loader 00B40DCD
    { VA_CAPTURE_PENDING,    0x01169160u },   // cleared in the save writer 004E721B; its setter 004E6F80 is unreachable
    { VA_CAPTURE_QUEUED,     0x01169161u },   // write-only on GOG (no callback reads it)
    { VA_CNAME_FROM_STR,     0x00B5E7ACu },   // __thiscall on GOG (ECX = out), RET 4
    { VA_DOOR_FADE_EVENT,    0x005B0276u },   // CDoorStreamingBlackScreen vtable 00D7A318 slot 2
    { VA_DOOR_FADE_IN,       0x005B02EDu },   // fld dword [00D269BC] on GOG (D9 05)
    { VA_DOOR_FADE_LEN,      0x00D269BCu },   // the shared 0.5
    { VA_DOOR_FADE_OUT,      0x005B01DFu },   // fld, in the BlackScreen constructor 005B0193
    { VA_DOOR_INTERIOR_VT,   0x00D7A36Cu },   // CDoorStreamingWaitOnKnob
    { VA_DOOR_START,         0x005AF6D9u },
    { VA_DOOR_START_SLOT,    0x00D7A5F8u },   // CStreamingDoor vtable 00D7A394, slot 153
    { VA_DOOR_TELEPORT_VT,   0x00D7A318u },   // CDoorStreamingBlackScreen
    { VA_DOOR_TIMER,         0x005AF658u },
    { VA_DOOR_TIMER_SLOT,    0x00D7A4CCu },   // CStreamingDoor vtable 00D7A394, slot 78
    { VA_DZIP_READER,        0x00AA1DFCu },   // ESI = this on GOG; one call, 00AEBF37
    { VA_DZIP_READER_CALL,   0x00AEBF37u },   // in the save open 00AEBEDA (thiscall, ECX = the gamesaves folder)
    { VA_DZIP_READER_VT,     0x00F2E990u },
    { VA_EMPTY_STRING,       0x01680110u },   // pushed by the request callers
    { VA_ENV_ADD,            0x00AC9EEDu },   // stdcall, 9 args, RET 0x24, as on Steam
    { VA_ENV_ADD_CALL,       0x00A89F59u },   // in the component activate 00A89ED3 (thiscall on GOG)
    { VA_ENV_STAB,           0x00AC8CDFu },   // thiscall on GOG (ECX = the manager), plain RET
    { VA_ENV_STAB_CALL,      0x005AF54Bu },   // a JMP: the tail of the door finish 005AF51F
    { VA_FACTS_TIME,         0x0050B0A9u },   // in FactsAdd 0050B044 ("FactsAdd" 00D441D4)
    { VA_QUIT_CALL,          0x00504E87u },   // in the QuitApplication native 00504E78 (registered in 00508C2D)
    { VA_QUIT_FLAG,          0x00AFA032u },   // ECX = engine (Steam reads the global itself)
    { VA_EXIT_GAME,          0x00AFC92Cu },   // ECX = game, plain RET; native 00AFCE86
    { VA_ENGINE,             0x0167D4CCu },
    { VA_PATH_EDGE_WALK,     0x00725FC0u },   // PathEngine, byte-identical to Steam's 006550C0
    { VA_TEXT_ALLOC,         0x00403268u },   // pool allocator, cdecl (size); called as on Steam
    { VA_TEXT_ALLOC_CALL,    0x00B1B4C2u },   // in the text reader 00B1B4AB (size ESI, buffer EDI)
    { VA_FADE_IN_EXEC,       0x00A2D789u },
    { VA_FADE_IN_EXEC_SLOT,  0x00F1F3CCu },   // CRenderCommand_ScreenFadeIn vtable 00F1F3C0 + 0x0C
    { VA_FADE_OUT_EXEC,      0x00A2D7E1u },
    { VA_FADE_OUT_EXEC_SLOT, 0x00F1F3B8u },   // CRenderCommand_ScreenFadeOut vtable 00F1F3AC + 0x0C
    { VA_GAME,               0x01475740u },   // written at 00AFB1DC; +0x40 activeWorld, +0x250 player
    { VA_GC,                 0x00B11BA4u },
    { VA_GDS_DTOR,           0x00423212u },   // same shape (lea ecx,[esi+4] where Steam has lea eax)
    { VA_GDS_VTBL,           0x00F21290u },   // CGameDataStorage (RTTI 01151A7C, locator 00FB57E8)
    { VA_IMAGES_ADD,         0 },
    { VA_IMAGES_REMOVE,      0 },
    { VA_IMAGES_VTBL_ADD,    0 },
    { VA_IMAGE_KEY,          0 },
    { VA_INDOOR_CMP,         0x00A11286u },   // in the frame setup 00A11181
    { VA_INDOOR_IMM,         0x00A11295u },
    { VA_LAYER_LOAD,         0x00AAE95Eu },   // stdcall (info, ctx), RET 8 on GOG
    { VA_LAYER_LOAD_CALL,    0x00AE5D66u },   // in the bucket loader 00AE5C5A ("Loading layer '%s'...")
    { VA_LAYER_SYNC,         0x00AAE727u },   // EAX = info; called by HideLayerGroup's 00B3F3F7 ("HideLayerGroup() called when no world is loaded." 00ECE518, native 00AE70B8)
    { VA_LAYER_VISIBLE,      0x00AAE25Au },   // same bytes; reads the game at 01475740 (VA_GAME)
    { VA_LAYER_VISIBLE_CALL, 0x00AAE743u },   // its only call
    { VA_DOOR_VTABLE,        0x00D7A394u },   // CStreamingDoor (VA_DOOR_TIMER_SLOT = slot 78); inside/outside +0x200/+0x214 as on Steam (005AFCC5)
    { VA_LS_CLEAR_CALL,      0x0044F348u },   // in the array clear 0044F334 (after mov eax,ebx), from the load 00B37C82
    { VA_LS_RECORD_FREE,     0x00B37334u },   // EAX = record; frees +0x18 only, as on Steam
    { VA_LS_REMOVE_CALL,     0x00B3746Fu },   // in the single removal 00B373CD (after mov eax,edx)
    { VA_MAP_FIND,           0 },
    { VA_MEM_SERIALIZE,      0x00B138EAu },
    { VA_MEM_SIZE,           0x00B138E3u },
    { VA_MEM_STREAM,         0x00F1A248u },   // CMemoryFileReader (RTTI), ctor 00B1391C
    { VA_PART_GC_CALL,       0x00AE614Du },
    { VA_PART_REQUEST,       0x00AFCFA6u },   // __thiscall on GOG (ECX = game)
    { VA_PART_RUN,           0x00AE5FCAu },   // job in EDI on GOG, plain RET
    { VA_PART_RUN_CALL,      0x00AE6418u },   // after mov edi,esi
    { VA_PART_SWITCH,        0x00AE626Cu },   // pushes "Streaming PH: "
    { VA_PART_SWITCH_A,      0x00AFE3F0u },   // in the request queue 00AFE36B
    { VA_PART_SWITCH_B,      0x00AFF2DCu },
    { VA_PROV_FORWARD,       0 },
    { VA_PROV_OPEN,          0 },
    { VA_PROV_STREAM,        0 },
    { VA_PROV_VTBL_OPEN,     0 },
    { VA_RAY_HIT,            0x0091F110u },   // body identical to Steam's 0084C750
    { VA_RAY_SLOT,           0x00E08740u },   // hkpSimpleWorldRayCaster vtable 00E0873C + 4
    { VA_RENDER_THREAD,      0x0167DE94u },   // set by CRenderThread slot 1, 00A23145
    { VA_SAVELIST,           0x005072D1u },   // __thiscall on GOG (ECX = panel), plain RET
    { VA_SAVELIST_CALL_A,    0x00507969u },   // in 00507953 (startup, menu background)
    { VA_SAVELIST_CALL_B,    0x0050794Au },   // in the RefreshSaveData native 00507941
    { VA_STARTVIDEO_CALL,    0x00509EF7u },   // in the start wrapper 00509EEB (ECX = player)
    { VA_STARTVIDEO_LIST,    0x00509DF5u },   // thiscall on GOG
    { VA_STARTVIDEO_STOP,    0x00509D26u },   // thiscall on GOG
    { VA_STARTVIDEO_TICK,    0x0050A01Eu },
    { VA_STARTVIDEO_VSLOT,   0x00D43FB0u },   // CVideoInstance vtable 00D43F3C + 0x74
    { VA_SPINNER_EXEC,       0x00A2D9F9u },
    { VA_SPINNER_EXEC_SLOT,  0x00F1F0C0u },   // CRenderCommand_ToggleLoadingIndicator vtable 00F1F0B4 + 0x0C
    { VA_ENTITY_VTABLE,      0x00F236FCu },   // CEntity, from the GOG exe's RTTI (the same parser gives CStreamingDoor 00D7A394)
    { VA_RCMD_ALLOC,         0x00AF662Fu },   // cdecl (size), as on Steam; called at 004E9DD6 before the ctor
    { VA_RCMD_LOADMOVIE,     0x00AF5D05u },   // ctor of vtable 00F1F0A0: memory in ECX, stdcall (viewport, movie), RET 8; its exec 00A2DA04 -> 00A22EA7
    { VA_LSCR_EXEC_SLOT,     0x00F1F098u },   // loading-screen command vtable 00F1F08C + 0x0C (ctor 00AF654E, from the show 00AFCB72 and the end 00AFC972)
    { VA_LSCR_EXEC,          0x00A2DA11u },   // -> 00A22F42; the game's count is at +0x68 as on Steam
    { VA_STEAM_STREAM,       0 },
    { VA_STREAM_DTOR,        0 },
    { VA_STR_FADE_OUT,       0x00D7A328u },
    { VA_UNPACK,             0x007614F0u },
    { VA_UNPACK_CALL,        0x007618C7u },
    { WF_CONFIG,             0x01679440u },   // written at 00A078CF; loaded into ECX before the writer
    { WF_D3D_DEVICE,         0x01690818u },   // stored at 004CE0D9 in device creation 004CDE00
    { WF_GAME_WNDPROC,       0x00A2362Du },   // W2ViewportClass lpfnWndProc, set at 00A2592D
    { WF_PP,                 0x01690830u },   // the stored present parameters (004CE0DF)
    { WF_RESET_COPY,         0x004CE64Eu },   // in the Reset 004CE510
    { WF_STYLE_SITE,         0x00A258C0u },   // windowed branch of 00A257D7
    { WF_VP_TICK,            0x00A32999u },   // pushes "Flushing all pending loading"
    { WF_VP_TICK_SLOT,       0x00F68B38u },   // CRenderInterface vtable 00F68B2C + 0x0C
    { WF_WRITER_SITE,        0x00420D54u },   // __thiscall on GOG (ECX = [WF_CONFIG])
    { WK_WNDPROC,            0x00A25ED3u },
    { WK_WNDPROC_SLOT,       0x00F68FA0u },   // CViewportWindow vtable, slot 0
};

static const char* build_name(void)
{
    return g_build == BUILD_STEAM ? "Steam 3.5.0.26" : g_build == BUILD_GOG ? "GOG 3.5.0.26g" : "unknown";
}

// The GOG value of a Steam address or RVA; 0 when it has no row or is not found yet.
static uintptr_t ga_gog(uintptr_t steam)
{
    unsigned i;
    for (i = 0; i < sizeof k_gog / sizeof k_gog[0]; i++)
        if (k_gog[i].steam == steam) return k_gog[i].gog;
    return 0;
}

// The one accessor for a fixed address: the running build's address for a Steam VA, with the
// load address applied; 0 when this build does not have it. On Steam it is the VA itself.
static uintptr_t ga(uintptr_t steam_va)
{
    uintptr_t v;
    if (g_build == BUILD_STEAM) return steam_va + g_delta;
    if (g_build != BUILD_GOG || !(v = ga_gog(steam_va))) return 0;
    return v + g_delta;
}

// The same for an RVA (EF_*, RVA_*): this build's RVA, to be added to g_base; 0 when unknown.
static uintptr_t ga_rva(uintptr_t steam_rva)
{
    if (g_build == BUILD_STEAM) return steam_rva;
    return g_build == BUILD_GOG ? ga_gog(steam_rva) : 0;
}

// 1 when the running build has every listed Steam address or RVA (the list ends with 0).
static int ga_have(uintptr_t first, ...)
{
    va_list ap;
    uintptr_t a;
    int ok = 1;
    va_start(ap, first);
    for (a = first; a; a = va_arg(ap, uintptr_t))
        if (g_build != BUILD_STEAM && !(g_build == BUILD_GOG && ga_gog(a))) ok = 0;
    va_end(ap);
    return ok;
}

// The build, load address and import slots of the image at `at` (tools/buildprobe maps the
// real exes this way). Reads only the image's own headers.
static void build_read(uintptr_t at)
{
    const unsigned char* base = (const unsigned char*)at;
    g_base = at;
    g_delta = g_base - IMAGE_BASE;
    g_build = BUILD_UNKNOWN;
    g_buildStamp = g_buildText = 0;
    memset(g_imp, 0, sizeof g_imp);
    __try {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS32* nt;
        const IMAGE_SECTION_HEADER* sec;
        WORD i;
        if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        sec = IMAGE_FIRST_SECTION(nt);
        g_buildStamp = nt->FileHeader.TimeDateStamp;
        for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (!memcmp(sec[i].Name, ".text", 6)) g_buildText = sec[i].Misc.VirtualSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (g_buildStamp == STEAM_TIMESTAMP && g_buildText == STEAM_TEXT_SIZE) g_build = BUILD_STEAM;
    else if (g_buildStamp == GOG_TIMESTAMP && g_buildText == GOG_TEXT_SIZE) g_build = BUILD_GOG;
    imports_find();
}

// DllMain, before any thread starts: witcher2.exe itself.
static void build_detect(void)
{
    build_read((uintptr_t)GetModuleHandleA(NULL));
}

static int g_gated;   // feature groups build_gate turned off

static void gate_off(const char* tag, const char* what)
{
    g_gated++;
    logf_("%s: not on this build (%s) - %s off", tag, build_name(), what);
}

// Turns off every switched-on feature whose addresses the running build does not have, with
// one log line per feature group. On Steam every address is known and nothing changes. Runs
// in DllMain after load_config, before any feature thread starts and before the script patches
// are set up. Needs no address itself: crash capture, the OSD, the DirectInput keyboard filter
// and the interior map files (data). The script patches and the CPU guard need import slots.
static void build_gate(void)
{
    int lod = ga_have(RVA_SETTINGS, 0);
    int distance = lod && ga_have(EF_RANGE_RVA, EF_SHADOW_RVA, 0);
    int registry = ga_have(VA_IMAGES_VTBL_ADD, VA_IMAGES_ADD, VA_IMAGES_REMOVE, VA_IMAGE_KEY, 0);
    // GOG saves to local files: no storage provider, and its save list never hands previews to
    // the image registry. Its preview fix needs only the capture import and CreateFileW.
    int gog = g_build == BUILD_GOG;
    int thumbs = gog ? g_imp[IMP_D3DX_SAVEMEM] && g_imp[IMP_CREATEFILEW]
                     : registry && g_imp[IMP_STEAM_RS] && g_imp[IMP_D3DX_SAVEMEM];
    int savetrace = thumbs && ga_have(VA_GAME, VA_CAPTURE_PENDING, VA_CAPTURE_QUEUED, 0);
    int listcall = ga_have(VA_SAVELIST, VA_SAVELIST_CALL_A, VA_SAVELIST_CALL_B, 0);
    int savelist = gog ? listcall : registry && listcall && ga_have(VA_MAP_FIND, VA_STEAM_STREAM,
        VA_STREAM_DTOR, VA_PROV_VTBL_OPEN, VA_PROV_OPEN, VA_PROV_STREAM, VA_PROV_FORWARD,
        VA_MEM_STREAM, VA_MEM_SERIALIZE, VA_MEM_SIZE, 0);
    int fastlist = savelist && (!gog || ga_have(VA_DZIP_READER, VA_DZIP_READER_CALL, VA_DZIP_READER_VT,
        VA_MEM_STREAM, VA_MEM_SERIALIZE, VA_MEM_SIZE, 0));
    int startvideo = ga_have(VA_STARTVIDEO_LIST, VA_STARTVIDEO_CALL, VA_STARTVIDEO_TICK,
        VA_STARTVIDEO_STOP, VA_STARTVIDEO_VSLOT, 0);
    int partitions = ga_have(VA_GAME, VA_PART_SWITCH, VA_PART_SWITCH_A, VA_PART_SWITCH_B,
        VA_PART_RUN, VA_PART_RUN_CALL, VA_PART_GC_CALL, VA_GC, VA_PART_REQUEST, VA_EMPTY_STRING, 0);
    int indoor = ga_have(VA_INDOOR_CMP, VA_INDOOR_IMM, 0);
    int doortimer = ga_have(VA_GAME, VA_DOOR_TIMER_SLOT, VA_DOOR_TIMER, VA_DOOR_TELEPORT_VT,
        VA_DOOR_FADE_EVENT, VA_CNAME_FROM_STR, VA_STR_FADE_OUT, 0);
    // doors_tick installs the door timer whenever the fades differ from the game's.
    int doorfades = ga_have(VA_GAME, VA_DOOR_FADE_OUT, VA_DOOR_FADE_IN, VA_DOOR_FADE_LEN,
        VA_DOOR_START_SLOT, VA_DOOR_START, VA_DOOR_TIMER_SLOT, VA_DOOR_TIMER, 0);
    int fadeexec = ga_have(VA_FADE_OUT_EXEC_SLOT, VA_FADE_OUT_EXEC, VA_FADE_IN_EXEC_SLOT,
        VA_FADE_IN_EXEC, 0);
    int loadtrace = partitions && doortimer && ga_have(VA_UNPACK, VA_UNPACK_CALL,
        VA_RENDER_THREAD, VA_DOOR_INTERIOR_VT, WF_D3D_DEVICE, WF_PP, RVA_SETTINGS, 0);
    int window = ga_have(WF_STYLE_SITE, WF_WRITER_SITE, WF_VP_TICK_SLOT, WF_VP_TICK,
        WF_RESET_COPY, WF_D3D_DEVICE, WF_PP, WF_GAME_WNDPROC, WF_CONFIG, 0);
    int winkeys = ga_have(WK_WNDPROC_SLOT, WK_WNDPROC, 0);
    int ray = ga_have(VA_RAY_SLOT, VA_RAY_HIT, 0);
    int quit = ga_have(VA_FACTS_TIME, 0);
    int storage = ga_have(VA_LS_RECORD_FREE, VA_LS_CLEAR_CALL, VA_LS_REMOVE_CALL, VA_GDS_VTBL, VA_GDS_DTOR, 0);
    int cleanq = ga_have(VA_QUIT_CALL, VA_QUIT_FLAG, VA_EXIT_GAME, VA_ENGINE, VA_GAME, 0);
    int pathg = ga_have(VA_PATH_EDGE_WALK, 0);
    int textg = ga_have(VA_TEXT_ALLOC, VA_TEXT_ALLOC_CALL, 0);
    // Script patches change data only, through the file hooks; they are tested on both builds.
    int scripts = g_build != BUILD_UNKNOWN && g_imp[IMP_CREATEFILEW] &&
                  g_imp[IMP_FINDFIRSTFILEW] && g_imp[IMP_FINDNEXTFILEW];

    if (!lod && (cfg_enforce || cfg_enforce_down || cfg_preset_mode || cfg_hotkeys)) {
        gate_off("lod", "LOD scales, draw distance presets and their hotkeys");
        cfg_enforce = cfg_enforce_down = cfg_preset_mode = cfg_hotkeys = 0;
    }
    if (!distance && cfg_distance) {
        gate_off("distance", "visibility and shadow range hooks");
        cfg_distance = 0;
    }
    if (!lod || !ga_have(VA_SUN_CASCADE_FIRST, VA_SUN_CASCADE_FIRST_LEN, VA_SUN_CASCADE_MID, VA_SUN_CASCADE_MID_LEN,
                         VA_SUN_CASCADE_FAR, VA_SUN_CASCADE_FAR_LEN, VA_SUN_ATLAS_SIZE, VA_SUN_ATLAS_ALLOC,
                         VA_SUN_ATLAS_CALL, VA_SUN_POISSON_SET, VA_SUN_POISSON_CALL, VA_RENDERER, 0)) {
        if (cfg_cascade) gate_off("shadows", "Shadow quality");
        cfg_cascade = 0;
    }
    if (!thumbs && cfg_save_thumbs) {
        gate_off("saves", "fix_thumbnails");
        cfg_save_thumbs = 0;
    }
    if (!savetrace && cfg_save_trace) {
        gate_off("diagnostics", "save_trace");
        cfg_save_trace = 0;
    }
    if (gog && cfg_list_leak) {
        logf_("saves: fix_list_leak not needed on this build (%s): its save list keeps no previews",
              build_name());
        cfg_list_leak = 0;
    }
    if (!savelist && (cfg_list_leak || cfg_fast_list || cfg_savelist_trace)) {
        gate_off("saves", "fix_list_leak, fast_list and savelist_trace");
        cfg_list_leak = cfg_fast_list = cfg_savelist_trace = 0;
    } else if (!fastlist && cfg_fast_list) {
        gate_off("saves", "fast_list");
        cfg_fast_list = 0;
    }
    if (!startvideo && cfg_skip_intro) {
        gate_off("startup", "skip_intro");
        cfg_skip_intro = 0;
    }
    if (!partitions && (cfg_keep_outdoors || cfg_keep_interiors || cfg_preload_interiors)) {
        gate_off("loading", "interiors (keep_outdoors, keep_interiors, preload_interiors, skip_door_gc, release_kept, hide_kept, preload_areas)");
        cfg_keep_outdoors = cfg_keep_interiors = cfg_preload_interiors = cfg_skip_door_gc = cfg_release_kept = cfg_hide_kept =
            cfg_preload_areas = 0;
    }
    if (!indoor && cfg_indoor_outdoors) {
        if (cfg_keep_outdoors) gate_off("loading", "indoor_outdoors");
        cfg_indoor_outdoors = 0;
    }
    if (!doortimer && cfg_fast_doors) {
        gate_off("loading", "fast_doors");
        cfg_fast_doors = 0;
    }
    if (!doorfades && (cfg_door_fade != 0.5f || cfg_area_fade != 0.5f)) {
        gate_off("loading", "door_fade and area_fade (the game's 0.5 s fades stay)");
        cfg_door_fade = cfg_area_fade = 0.5f;
    }
    if ((!fadeexec || !doorfades) && cfg_instant_doors) {
        if (cfg_keep_interiors || cfg_preload_interiors) gate_off("loading", "instant_doors");
        cfg_instant_doors = 0;
    }
    // Both act on instant doors only; without them they have nothing to do.
    if (!ga_have(VA_GAME, VA_ENV_ADD_CALL, VA_ENV_ADD, VA_ENV_STAB_CALL, VA_ENV_STAB, 0) && cfg_door_light_blend > 0.0f) {
        if (cfg_instant_doors) gate_off("loading", "door_light_blend");
        cfg_door_light_blend = 0.0f;
    }
    if (!ga_have(VA_SPINNER_EXEC_SLOT, VA_SPINNER_EXEC, 0) && !cfg_door_spinner) {
        if (cfg_instant_doors) gate_off("loading", "the hidden spinner on instant doors (door_spinner)");
        cfg_door_spinner = 1;
    }
    if (!loadtrace && cfg_load_trace) {
        gate_off("diagnostics", "load_trace");
        cfg_load_trace = 0;
    }
    if (!window && (cfg_wf_enabled || cfg_window_mode)) {
        gate_off("window", "borderless, the live full screen switch and the Options rows (window_mode)");
        cfg_wf_enabled = cfg_window_mode = 0;
    }
    if (!winkeys && cfg_winkeys) {
        gate_off("input", "windows_keys");
        cfg_winkeys = 0;
    }
    if (!ray && cfg_ray_guard) {
        gate_off("stability", "raycast_guard");
        cfg_ray_guard = 0;
    }
    if (!quit && cfg_quit_guard) {
        gate_off("stability", "quit_guard");
        cfg_quit_guard = 0;
    }
    if (!storage && cfg_storage_leak) {
        gate_off("stability", "layer_storage_leak");
        cfg_storage_leak = 0;
    }
    if (!cleanq && cfg_clean_quit) {
        gate_off("stability", "clean_quit");
        cfg_clean_quit = 0;
    }
    if (!pathg && cfg_path_guard) {
        gate_off("stability", "path_guard");
        cfg_path_guard = 0;
    }
    if (!textg && cfg_text_guard) {
        gate_off("stability", "text_guard");
        cfg_text_guard = 0;
    }
    if (cfg_lscr_cleanup && !ga_have(VA_RCMD_ALLOC, VA_RCMD_LOADMOVIE, VA_LSCR_EXEC_SLOT, VA_LSCR_EXEC, VA_RENDER_THREAD, VA_GAME, 0)) {
        gate_off("stability", "loading_screen_cleanup");
        cfg_lscr_cleanup = 0;
    }
    // The pause menu's Quit ends the game with an area loaded, which crashes in FactsAdd during
    // shutdown unless quit_guard is in (docs/fixes/quit-crash.md): the row waits for the guard.
    if (!quit && cfg_pause_quit && scripts) {
        gate_off("scriptpatches", "pause_quit (it needs quit_guard)");
        cfg_pause_quit = 0;
    }
    if (!g_imp[IMP_GETPROCAFFINITY] && cfg_cpu32_guard) {
        gate_off("stability", "cpu32_guard");
        cfg_cpu32_guard = 0;
    }
    if (!scripts && (cfg_book_marker || cfg_continue || cfg_pause_quit || cfg_window_mode ||
                     cfg_medallion)) {
        gate_off("scriptpatches", "book_marker, continue, pause_quit, window_mode and medallion");
        cfg_book_marker = cfg_continue = cfg_pause_quit = cfg_window_mode = cfg_medallion = 0;
    }
    if (g_gated && g_build == BUILD_GOG)
        logf_("build: %d feature group(s) not ported to GOG yet (listed above); the rest runs as configured",
              g_gated);
    else if (g_gated)
        logf_("build: witcher2.exe not recognised - every feature that needs a game address is off");
}

// ---- proxy plumbing -----------------------------------------------------

static void ensure_real_dinput(void)
{
    if (g_realDinput) return;
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    lstrcatA(sys, "\\dinput8.dll");
    g_realDinput = LoadLibraryA(sys);
}

typedef HRESULT (WINAPI *PFN_Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT (WINAPI *PFN_NoArg)(void);
typedef HRESULT (WINAPI *PFN_ClassObj)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT (WINAPI *PFN_Server)(void);
typedef void*   (WINAPI *PFN_Joystick)(void);

static FARPROC real_proc(const char* name)
{
    ensure_real_dinput();
    return g_realDinput ? GetProcAddress(g_realDinput, name) : NULL;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    PFN_Create fn = (PFN_Create)real_proc("DirectInput8Create");
    if (!fn) return E_FAIL;
    void* from = _ReturnAddress();
    HRESULT hr = fn(hinst, ver, riid, out, outer);
    if (!g_proxyOnly && SUCCEEDED(hr) && out && *out && (cfg_suppress || cfg_winkeys)) {
        // Interface A or W and the caller: other modules than the game create DirectInput too.
        static const GUID iid8A =
            { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
        static const GUID iid8W =
            { 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
        char who[MAX_PATH + 16];
        void* vt = *(void**)*out;
        int r = vt_hook_slot(*out, 3, (void*)hook_CreateDevice);
        if (r == 1 && g_diLogged >= 8) return hr;   // the game recreates it on every switch to play
        g_diLogged++;
        crash_where((DWORD)(uintptr_t)from, who, sizeof who);
        logf_("input: DirectInput8Create (%s) from %s: CreateDevice hook %s on vtable %08X",
              !riid ? "?" : !memcmp(riid, &iid8W, sizeof(GUID)) ? "W" :
              !memcmp(riid, &iid8A, sizeof(GUID)) ? "A" : "other", who,
              r == 2 ? "installed" : r == 1 ? "already there" : "NOT installed", (unsigned)(uintptr_t)vt);
    }
    return hr;
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    PFN_NoArg fn = (PFN_NoArg)real_proc("DllCanUnloadNow");
    return fn ? fn() : S_FALSE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID a, REFIID b, LPVOID* c)
{
    PFN_ClassObj fn = (PFN_ClassObj)real_proc("DllGetClassObject");
    return fn ? fn(a, b, c) : E_FAIL;
}

HRESULT WINAPI DllRegisterServer(void)
{
    PFN_Server fn = (PFN_Server)real_proc("DllRegisterServer");
    return fn ? fn() : E_FAIL;
}

HRESULT WINAPI DllUnregisterServer(void)
{
    PFN_Server fn = (PFN_Server)real_proc("DllUnregisterServer");
    return fn ? fn() : E_FAIL;
}

void* WINAPI GetdfDIJoystick(void)
{
    PFN_Joystick fn = (PFN_Joystick)real_proc("GetdfDIJoystick");
    return fn ? fn() : NULL;
}

// <game>\bin\dinput8.dll -> game = <game>, cache = <game>\bin\witcher2_enginefix_cache
// (decision 2 of the plan: next to the DLL, removed together with it).
static int script_dirs(wchar_t* game, wchar_t* cache)
{
    wchar_t bin[MAX_PATH], *slash;

    if (!GetModuleFileNameW(g_self, bin, MAX_PATH) || !bin[0]) return 0;
    slash = wcsrchr(bin, L'\\');
    if (!slash) return 0;
    *slash = 0;
    if (_snwprintf_s(cache, MAX_PATH, _TRUNCATE, L"%s\\witcher2_enginefix_cache", bin) < 0)
        return 0;
    slash = wcsrchr(bin, L'\\');
    if (!slash) return 0;
    *slash = 0;
    return _snwprintf_s(game, MAX_PATH, _TRUNCATE, L"%s", bin) >= 0;
}

// Other programs in bin load this DLL too: the launcher's Configurator.exe imports dinput8.
// There it only forwards to the system DLL, with no log (it rotated and overwrote the game's
// log and .prev.log, so a crash log was lost when the settings were opened), no hooks and no
// threads. The input probe (tools\inputprobe\probe.exe) keeps everything: it tests the hooks.
static int host_is_game(void)
{
    wchar_t exe[MAX_PATH];
    const wchar_t* name;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);

    if (!n || n >= MAX_PATH) return 1;      // unknown: behave as before
    name = wcsrchr(exe, L'\\');
    name = name ? name + 1 : exe;
    return _wcsicmp(name, L"witcher2.exe") == 0 || _wcsicmp(name, L"probe.exe") == 0;
}

// ---------------------------------------------------------------------------------------------
// Script bindings, [diagnostics] script_bindings=1. Research, Steam only, off by default.
//
// Registers natives of its own before the engine compiles base_scripts.dzip. The registration
// alone is not enough - see docs/engine-script-bindings.md:
//
//   The VM has no ordinary C calling convention. A native receives the interpreter frame in its
//   second argument, consumes its own inline operand bytes from the frame's code pointer
//   ([frame+0x18]) and writes its result through the VM's value stack. Engine natives do this by
//   hand (ScriptAreaEnvironmentActivate, 0x00A75295, is the smallest example). sb_call() below is
//   that sequence once, so a native is a plain C body; a body that skips it desynchronises the
//   interpreter and the game then executes a data byte as an opcode.
//
// W2EF_Echo/Num/Flag return what they are given (string, float, bool; the int case was proved by
// the first probe, since removed), W2EF_Name and W2EF_Object do the same for the name and object
// reference slots, and W2EF_MeshDistance reads live engine state when a script calls it.
//
// Plugins (src/w2ef_plugin.h) register their own natives the same way: every *.dll in
// <dinput8.dll's folder>\witcher2_enginefix_plugins is loaded from here, before the compile.
//
// The call shapes used here are the engine's own (docs/engine-script-bindings.md):
//   alloc    00ABE94A  cdecl (0xAC) -> zeroed block
//   build    00B0F409  EDX = descriptor, stack (CName, fn), RET 8
//   register 00B102A4  EAX = descriptor, stack (registry), RET 4
//   registry 0040387C  -> [0x02589018]
//   CName    00B28D76  __stdcall (DWORD* out, const wchar_t*), RET 8
//   release  00ABC99F  ESI = slot: release [slot] as a reference, clear it, RET
#define SB_COMPILE_SITE 0x00AA69A0u   // CompileScriptsFromSources, __stdcall (bool), RET 4
#define SB_COMPILE_ORIG "55 8b ec a1 34 24 0d 01"   // push ebp; mov ebp,esp; mov eax,[0x010d2434]
#define SB_ALLOC        0x00ABE94Au
#define SB_BUILD        0x00B0F409u
#define SB_REGISTER     0x00B102A4u
#define SB_REGISTRY     0x0040387Cu
#define SB_CNAME        0x00B28D76u
#define SB_FREE_OBJECT  0x00ABC99Fu
#define SB_DESC_SIZE    0xAC
#define SB_DESC_SLOT    0xA0          // the descriptor's own index, written by the builder

typedef void (__stdcall *sb_cname_fn)(DWORD* out, const wchar_t* name);
typedef char (__stdcall *sb_compile_fn)(char from_release);

static void* sb_real_compile;    // trampoline: the replaced prologue, then a jump back
static LONG  sb_registered;

// The native side of docs/engine-script-bindings.md, "The native's contract". A script call
// arrives as native(ctx, frame, result), cdecl; the native evaluates its own arguments - one
// FUN_00abc328 per declared parameter - then consumes the call's terminator byte, and writes
// its result into `result` when that is not null.
//
// sb_run() is that sequence once, so a new script function is a plain C body instead of
// hand-written assembly. The three engine helpers go through naked shims: two of them take
// their first argument in EAX, and their stack cleaning is not a C function's - the shims
// restore ESP from EBP, so it does not matter which way the callee cleans.
#define SB_FRAME_CODE 0x18
#define SB_FRAME_CTX  0x08
#define SB_INIT_VALUE 0x00401FAFu   // EAX = slot, stack = the empty-script default
#define SB_EVAL       0x00ABC328u   // EAX = frame, stack (ctx, destination); cdecl
#define SB_FREE_VALUE 0x004021EEu   // EAX = slot, no stack arguments
#define SB_VALUE_SIZE 16
#define SB_MAX_ARGS   3u

// The slot is the plugin ABI's slot as well, so a plugin body sees the same bytes we do.
typedef w2ef_value sb_value;

static uintptr_t sb_init_ptr, sb_eval_ptr, sb_free_ptr, sb_free_object_ptr;
static LONG sb_calls, sb_enters;

// The shims read their arguments out of [ebp+..] in assembly, so C sees them as unused.
#pragma warning(push)
#pragma warning(disable: 4100)

// EAX = slot, the source pushed. EAX-first, so a shim is the only way in.
__declspec(naked) static void sb_value_init(sb_value* slot)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  eax, dword ptr [ebp+0x8]
        push 0x0160fba0                 // DAT_0160FBA0, the empty-script default
        call dword ptr [sb_init_ptr]
        mov  esp, ebp
        pop  ebp
        ret
    }
}

// EAX = frame, stack (frame->ctx, destination); one bytecode instruction per call.
__declspec(naked) static void sb_value_eval(char* frame, sb_value* dest)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  eax, dword ptr [ebp+0x8]
        push dword ptr [ebp+0xc]        // destination
        push dword ptr [eax+SB_FRAME_CTX]
        call dword ptr [sb_eval_ptr]
        mov  esp, ebp
        pop  ebp
        ret
    }
}

// Releases the value's payload as a heap block. Only for a type that owns memory: an int slot
// holds the number itself at +0, so this would call the allocator with the number.
__declspec(naked) static void sb_value_free(sb_value* slot)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  eax, dword ptr [ebp+0x8]
        call dword ptr [sb_free_ptr]
        mov  esp, ebp
        pop  ebp
        ret
    }
}

// FUN_00ABC99F releases [slot] as an object reference - a refcount decrement, and the object is
// destroyed when it reaches zero - then clears the slot. It takes the slot in ESI, which is
// callee-saved, so the shim restores it for the C caller. ThrowEntity (0x00A72721), a kind-3
// global registered exactly like ours, releases its CEntity argument with it.
__declspec(naked) static void sb_value_free_object(sb_value* slot)
{
    __asm {
        push ebp
        mov  ebp, esp
        push esi
        mov  esi, dword ptr [ebp+0x8]
        call dword ptr [sb_free_object_ptr]
        pop  esi
        pop  ebp
        ret
    }
}

// A value slot is a 16-byte wide-string container: +0 the buffer, +4 the length in code units
// including the NUL, +8 the capacity, +12 sizeof(wchar_t). All four fields are written by
// FUN_00401faf, which is why an int slot that was initialised carries 2 at +12 as well - it is
// not a type id. An int uses +0 as the number; a string owns the buffer at +0 and must be
// deep-copied, never memcpy'd, or two slots free the same block.

// EAX = slot, the source pushed: also the copy constructor, and the way a string result is
// produced (FUN_00401faf deep-copies the source's buffer).
__declspec(naked) static void sb_value_copy(sb_value* dst, const sb_value* src)
{
    __asm {
        push ebp
        mov  ebp, esp
        mov  eax, dword ptr [ebp+0x8]
        push dword ptr [ebp+0xc]
        call dword ptr [sb_init_ptr]
        mov  esp, ebp
        pop  ebp
        ret
    }
}

#pragma warning(pop)

static void sb_log_text(const wchar_t* text)
{
    char buf[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, text ? text : L"", -1, buf, sizeof buf, NULL, NULL);

    if (n > 0) logf_("script bindings: string \"%s\"", buf);
}

// One script call: evaluate nargs arguments, consume the terminator, run the body, release the
// types that own memory. `kinds` is one entry per argument: a string owns its buffer and is
// released with FUN_004021ee, an object reference owns a reference (measured: the evaluator
// hands one out per argument, refs 4 -> 5 for two arguments of the same object) and is released
// with FUN_00ABC99F. A name and an int/float/bool own nothing, and a name starts at 0 instead of
// being built as a string container - it is a CName dword, not a 16-byte string.
enum { SB_ARG_PLAIN = W2EF_ARG_PLAIN, SB_ARG_STRING = W2EF_ARG_STRING,
       SB_ARG_NAME = W2EF_ARG_NAME, SB_ARG_OBJECT = W2EF_ARG_OBJECT };

typedef w2ef_body_fn sb_body_fn;

static void __cdecl sb_call(void* ctx, char* frame, void* result, const unsigned char* kinds,
                            unsigned nargs, sb_body_fn body)
{
    sb_value args[SB_MAX_ARGS];
    unsigned i;

    if (nargs > SB_MAX_ARGS) nargs = SB_MAX_ARGS;
    for (i = 0; i < nargs; i++) {
        // Zero before FUN_00401faf: it releases whatever [slot+0] holds, so an uninitialised
        // slot frees a stack value (measured: heap corruption at STATUS_HEAP_CORRUPTION). A name
        // or an object reference is a single dword and must not be built as a string container:
        // the engine's own natives zero one and then evaluate into it.
        memset(&args[i], 0, sizeof args[i]);
        if (kinds[i] != SB_ARG_NAME && kinds[i] != SB_ARG_OBJECT) sb_value_init(&args[i]);
        sb_value_eval(frame, &args[i]);
    }
    ++*(int*)(frame + SB_FRAME_CODE);
    body(ctx, frame, args, result);
    for (i = 0; i < nargs; i++) {
        if (kinds[i] == SB_ARG_STRING) sb_value_free(&args[i]);
        else if (kinds[i] == SB_ARG_OBJECT) sb_value_free_object(&args[i]);
    }
}

// One string in, the same string out: proves the string layout, the deep copy and the release.
static void __cdecl sb_echo_body(void* ctx, char* frame, sb_value* args, void* result)
{
    const wchar_t* text = *(const wchar_t* const*)args[0].b;

    (void)ctx;
    (void)frame;
    if (InterlockedIncrement(&sb_calls) <= 6) sb_log_text(text);
    if (result) sb_value_copy((sb_value*)result, &args[0]);
}

static void __cdecl sb_echo(void* ctx, char* frame, void* result)
{
    static const unsigned char kinds[1] = { SB_ARG_STRING };

    if (InterlockedIncrement(&sb_enters) <= 12) logf_("script bindings: enter W2EF_Echo");
    sb_call(ctx, frame, result, kinds, 1, sb_echo_body);
}

// One float in, the same float out: the payload is a 32-bit IEEE value at +0.
static void __cdecl sb_num_body(void* ctx, char* frame, sb_value* args, void* result)
{
    float value = *(const float*)args[0].b;

    (void)ctx;
    (void)frame;
    if (InterlockedIncrement(&sb_calls) <= 6) logf_("script bindings: float %g", (double)value);
    if (result) *(float*)result = value;
}

static void __cdecl sb_num(void* ctx, char* frame, void* result)
{
    static const unsigned char kinds[1] = { SB_ARG_PLAIN };

    if (InterlockedIncrement(&sb_enters) <= 12) logf_("script bindings: enter W2EF_Num");
    sb_call(ctx, frame, result, kinds, 1, sb_num_body);
}

// One bool in, the same bool out: the payload is a single byte at +0.
static void __cdecl sb_flag_body(void* ctx, char* frame, sb_value* args, void* result)
{
    unsigned char value = args[0].b[0] ? 1 : 0;

    (void)ctx;
    (void)frame;
    if (InterlockedIncrement(&sb_calls) <= 6) logf_("script bindings: bool %u", value);
    if (result) *(unsigned char*)result = value;
}

static void __cdecl sb_flag(void* ctx, char* frame, void* result)
{
    static const unsigned char kinds[1] = { SB_ARG_PLAIN };

    if (InterlockedIncrement(&sb_enters) <= 12) logf_("script bindings: enter W2EF_Flag");
    sb_call(ctx, frame, result, kinds, 1, sb_flag_body);
}

// The first native with a body of its own instead of an argument echoed back: it reads live
// engine state when the script calls it. MeshDistanceScale sits at +0x6C of the lazily created
// settings object; the look is through grab_settings, the same path the LOD code writes, so this
// also proves that a value the DLL set can be read back from a script. CGame exists by the time a
// script runs (it is created at 00AA7075, after the compile), which is what makes this possible -
// registration itself still cannot read any game state.
static void __cdecl sb_mesh_body(void* ctx, char* frame, sb_value* args, void* result)
{
    settings_view sv;
    float value;

    (void)ctx;
    (void)frame;
    (void)args;
    if (!grab_settings(&sv)) {
        if (InterlockedIncrement(&sb_calls) <= 6)
            logf_("script bindings: MeshDistanceScale not readable at call time");
        return;
    }
    value = sv.mesh;
    if (InterlockedIncrement(&sb_calls) <= 6)
        logf_("script bindings: MeshDistanceScale %.3f (foliage %.3f)", (double)value,
              (double)sv.foliage);
    if (result) *(float*)result = value;
}

static void __cdecl sb_mesh(void* ctx, char* frame, void* result)
{
    if (InterlockedIncrement(&sb_enters) <= 12)
        logf_("script bindings: enter W2EF_MeshDistance");
    sb_call(ctx, frame, result, NULL, 0, sb_mesh_body);
}

// A name in, the same name out: the slot is one CName dword at +0 and owns nothing
// (StringToName, 0x008E4C85, writes one dword into its result; NameToString, 0x008E5987,
// evaluates into a 4-byte local). Measured in game: slot 00004DD8 00000000 00000000 00000000.
static void __cdecl sb_name_body(void* ctx, char* frame, sb_value* args, void* result)
{
    DWORD value = *(const DWORD*)args[0].b;

    (void)ctx;
    (void)frame;
    if (InterlockedIncrement(&sb_calls) <= 6)
        logf_("script bindings: name %08X (slot %08X %08X %08X %08X)", value,
              *(const DWORD*)(args[0].b), *(const DWORD*)(args[0].b + 4),
              *(const DWORD*)(args[0].b + 8), *(const DWORD*)(args[0].b + 12));
    if (result) *(DWORD*)result = value;
}

static void __cdecl sb_name(void* ctx, char* frame, void* result)
{
    static const unsigned char kinds[1] = { SB_ARG_NAME };

    if (InterlockedIncrement(&sb_enters) <= 12) logf_("script bindings: enter W2EF_Name");
    sb_call(ctx, frame, result, kinds, 1, sb_name_body);
}

// An object reference in, the same reference out. The slot is one pointer at +0 whose object
// carries its refcount in the first dword; the evaluator hands out an owned reference per
// argument (measured: two arguments of the same object gave refs 4 -> 5, 3 after both releases),
// and sb_call releases it. A result is produced by moving that reference into the result slot, so
// the script owns it - a move, never a second reference.
static void __cdecl sb_object_body(void* ctx, char* frame, sb_value* args, void* result)
{
    DWORD obj = *(const DWORD*)args[0].b;

    (void)ctx;
    (void)frame;
    if (InterlockedIncrement(&sb_calls) <= 6)
        logf_("script bindings: object %08X refs %u", obj, obj ? *(volatile const DWORD*)obj : 0);
    if (result && obj) {
        *(DWORD*)result = obj;          // the reference moves; sb_call's release then sees 0
        *(DWORD*)args[0].b = 0;
    }
}

static void __cdecl sb_object(void* ctx, char* frame, void* result)
{
    static const unsigned char kinds[1] = { SB_ARG_OBJECT };

    if (InterlockedIncrement(&sb_enters) <= 12) logf_("script bindings: enter W2EF_Object");
    sb_call(ctx, frame, result, kinds, 1, sb_object_body);
}

// Register one global script function the way RegisterEnvironmentScriptNatives (0x00A76637) does.
static int sb_register_native(const wchar_t* name, void* fn)
{
    uintptr_t p_alloc = ga(SB_ALLOC), p_build = ga(SB_BUILD), p_register = ga(SB_REGISTER);
    uintptr_t p_registry = ga(SB_REGISTRY), p_cname = ga(SB_CNAME);
    DWORD cn = 0;
    void* desc = NULL;
    void* registry = NULL;

    if (!p_alloc || !p_build || !p_register || !p_registry || !p_cname) {
        logf_("script bindings: an engine address is missing on this build - not registered");
        return 0;
    }
    __asm {
        mov  eax, p_alloc
        push SB_DESC_SIZE
        call eax
        pop  ecx                       // the allocator is cdecl: the caller removes the size
        mov  desc, eax
    }
    if (!desc) { logf_("script bindings: the 0xAC descriptor allocation failed"); return 0; }

    ((sb_cname_fn)p_cname)(&cn, name);   // __stdcall, RET 8
    __asm {
        mov  eax, p_build
        mov  edx, desc                     // the builder takes its `this` in EDX
        push fn
        push cn
        call eax                           // RET 8: the callee removes both arguments
    }

    __asm {
        mov  eax, p_registry
        call eax
        mov  registry, eax
    }
    if (!registry) { logf_("script bindings: no registry object"); return 0; }

    __asm {
        push ebx
        mov  ebx, p_register
        mov  eax, desc                     // EAX = descriptor, the builder's return value
        push registry
        call ebx                           // RET 4
        pop  ebx
    }
    {
        char narrow[64];

        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof narrow, NULL, NULL) > 0)
            logf_("script bindings: %s registered at slot %u", narrow,
                  *(unsigned*)((char*)desc + SB_DESC_SLOT));
    }
    return 1;
}

// ---- plugin interface ------------------------------------------------------------------------
// The API handed to every plugin (src/w2ef_plugin.h): the same functions the built-in natives
// use, so a plugin body behaves exactly like one of ours. `log` prefixes every line with
// "plugin: ", which is the only way the log shows where a plugin's message came from.
static void __cdecl sb_api_log(const char* fmt, ...)
{
    char line[512];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    logf_("plugin: %s", line);
}

static int __cdecl sb_api_register(const wchar_t* name, w2ef_native_fn fn)
{
    return sb_register_native(name, (void*)fn);
}

static const w2ef_plugin_api g_pluginApi = {
    sizeof(w2ef_plugin_api),
    W2EF_PLUGIN_API_VERSION,
    sb_api_register,
    sb_call,
    sb_value_free,
    sb_value_copy,
    sb_value_free_object,
    sb_api_log
};

// ---- plugin loader ---------------------------------------------------------------------------
// [diagnostics] script_bindings=1: every *.dll in <dinput8.dll's folder>\witcher2_enginefix_plugins
// is loaded before the compile and asked to register its natives through g_pluginApi, in file
// name order. A plugin is native code inside the game process, so it is trusted: the loader can
// report a DLL that will not load or whose init returns 0, it cannot survive a plugin that
// crashes. The folder may be absent; nothing is loaded then.
#define SB_MAX_PLUGINS 32

static int __cdecl sb_plugin_name_cmp(const void* a, const void* b)
{
    return wcscmp((const wchar_t*)a, (const wchar_t*)b);
}

// <folder of this DLL>\witcher2_enginefix_plugins, with no trailing backslash.
static int sb_plugin_dir(wchar_t* dir, size_t cap)
{
    wchar_t path[MAX_PATH];
    wchar_t* slash;
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);

    if (!n || n >= MAX_PATH) return 0;
    slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    *slash = 0;
    return swprintf_s(dir, cap, L"%s\\witcher2_enginefix_plugins", path) >= 0;
}

static void sb_load_plugins(void)
{
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    wchar_t names[SB_MAX_PLUGINS][MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0, found = 0, loaded = 0, i;

    if (!sb_plugin_dir(dir, MAX_PATH)) {
        logf_("plugins: the plugin folder path is unavailable - none loaded");
        return;
    }
    if (swprintf_s(pattern, MAX_PATH, L"%s\\*.dll", dir) < 0) return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        logf_("plugins: no plugin folder at %ls - none loaded", dir);
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            found++;
            if (count < SB_MAX_PLUGINS) wcscpy_s(names[count++], MAX_PATH, fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found == 0) {
        logf_("plugins: %ls holds no DLLs", dir);
        return;
    }
    if (found > count)
        logf_("plugins: %d DLLs found, only the first %d are loaded", found, count);

    qsort(names, (size_t)count, sizeof names[0], sb_plugin_name_cmp);
    for (i = 0; i < count; i++) {
        wchar_t full[MAX_PATH];
        w2ef_plugin_init_fn init;
        HMODULE mod;

        if (swprintf_s(full, MAX_PATH, L"%s\\%s", dir, names[i]) < 0) continue;
        mod = LoadLibraryExW(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!mod) {
            logf_("plugins: %ls: LoadLibrary failed (%lu)", names[i], GetLastError());
            continue;
        }
        init = (w2ef_plugin_init_fn)(uintptr_t)GetProcAddress(mod, W2EF_PLUGIN_INIT);
        if (!init) {
            logf_("plugins: %ls: no %s export", names[i], W2EF_PLUGIN_INIT);
            continue;
        }
        if (init(&g_pluginApi)) {
            loaded++;
            logf_("plugins: %ls loaded", names[i]);
        } else {
            logf_("plugins: %ls: %s returned 0", names[i], W2EF_PLUGIN_INIT);
        }
    }
    logf_("plugins: %d of %d loaded", loaded, count);
}

// Runs before the compiler: register once, then continue into the engine's own body.
static char __stdcall sb_compile_hook(char from_release)
{
    if (InterlockedExchange(&sb_registered, 1) == 0) {
        sb_register_native(L"W2EF_Echo", (void*)sb_echo);
        sb_register_native(L"W2EF_Num", (void*)sb_num);
        sb_register_native(L"W2EF_Flag", (void*)sb_flag);
        sb_register_native(L"W2EF_MeshDistance", (void*)sb_mesh);
        sb_register_native(L"W2EF_Name", (void*)sb_name);
        sb_register_native(L"W2EF_Object", (void*)sb_object);
        sb_load_plugins();
    }
    return ((sb_compile_fn)sb_real_compile)(from_release);
}

// Entry replacement: "jmp sb_compile_hook" over the 8-byte prologue, which the trampoline runs.
static int sb_install(uintptr_t at)
{
    static const unsigned char prologue[8] = { 0x55, 0x8B, 0xEC, 0xA1, 0x34, 0x24, 0x0D, 0x01 };
    unsigned char code[8];
    unsigned char* tramp;
    pb_range range;
    HANDLE peers[256];
    unsigned count = 0;
    DWORD old = 0, tmp = 0;
    int64_t delta = (int64_t)(uintptr_t)sb_compile_hook - (int64_t)(at + 5);

    if (delta < INT32_MIN || delta > INT32_MAX) return 0;   // the jump must be reachable
    if (!code_matches(at, SB_COMPILE_ORIG)) return 0;
    tramp = (unsigned char*)wf_make_trampoline((void*)at, prologue, sizeof prologue);
    if (!tramp) return 0;
    sb_real_compile = tramp;
    code[0] = 0xE9;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    code[5] = 0x90;
    code[6] = 0x90;
    code[7] = 0x90;
    range.start = (const unsigned char*)at;
    range.len = (unsigned)sizeof prologue;
    if (!pb_stop_peers(peers, &count, &range, 1)) goto failed;
    if (!code_matches(at, SB_COMPILE_ORIG) ||
        !VirtualProtect((void*)at, sizeof code, PAGE_EXECUTE_READWRITE, &old)) {
        pb_resume_peers(peers, count);
        goto failed;
    }
    memcpy((void*)at, code, sizeof code);
    VirtualProtect((void*)at, sizeof code, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, sizeof code);
    pb_resume_peers(peers, count);
    return 1;
failed:
    sb_real_compile = NULL;
    VirtualFree(tramp, 0, MEM_RELEASE);
    return 0;
}

// Started from DllMain while script_bindings=1. The SteamStub decrypts .text after every
// DllMain, so wait for the fingerprint; the barrier can refuse while threads are still being
// created, hence the retry (the window patches' pattern).
static DWORD WINAPI scriptbind_thread(LPVOID param)
{
    uintptr_t at = ga(SB_COMPILE_SITE);
    DWORD t0 = GetTickCount();

    (void)param;
    if (!at) { logf_("script bindings: address unknown on this build - off"); return 0; }
    sb_init_ptr = ga(SB_INIT_VALUE);
    sb_eval_ptr = ga(SB_EVAL);
    sb_free_ptr = ga(SB_FREE_VALUE);
    sb_free_object_ptr = ga(SB_FREE_OBJECT);
    if (!sb_init_ptr || !sb_eval_ptr || !sb_free_ptr || !sb_free_object_ptr) {
        logf_("script bindings: a value helper address is unknown on this build - off");
        return 0;
    }
    if (!wf_wait_site(at, SB_COMPILE_ORIG, 30000)) {
        logf_("script bindings: CompileScriptsFromSources never read as its real bytes - off");
        return 0;
    }
    // One line before the freeze: if the game ever hangs on the peer barrier again, this and the
    // absence of the "hooked" line below name the step that was interrupted.
    logf_("script bindings: site matched at %08X, installing", (unsigned)at);
    do {
        if (sb_install(at)) {
            logf_("script bindings: hooked CompileScriptsFromSources at %08X", (unsigned)at);
            return 0;
        }
        Sleep(1);
    } while (g_running && (long)(GetTickCount() - t0) < 3000);
    logf_("script bindings: install failed - off (the engine compiles without our name)");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        if (!host_is_game()) {
            g_proxyOnly = 1;
            return TRUE;
        }
        g_mainTid = GetCurrentThreadId();   // DllMain runs on the game's main thread
        build_detect();
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_pbLock);
        InitializeCriticalSection(&g_thumbLock);
        InitializeCriticalSection(&g_ftLock);
        InitializeCriticalSection(&g_vtLock);
        g_ftT0 = GetTickCount();
        ini_path();
        load_config();
        if (cfg_log) {
            // Keep the last session's log: after a crash the game is usually restarted
            // before anyone looks, and "w" would wipe the evidence.
            MoveFileExA(g_logPath, g_prevLogPath, MOVEFILE_REPLACE_EXISTING);
            // _SH_DENYWR, not fopen_s (exclusive): the log can be read while the game runs.
            g_log = _fsopen(g_logPath, "w", _SH_DENYWR);
        }
        logf_("witcher2_enginefix %s loaded (image base 0x%08X)", ENGINEFIX_VERSION, (unsigned)g_base);
        {
            char missing[200] = "";
            size_t used = 0;
            int found = 0, i, n;
            for (i = 0; i < IMP_COUNT; i++) {
                if (g_imp[i]) { found++; continue; }
                n = _snprintf_s(missing + used, sizeof missing - used, _TRUNCATE, " %s", k_imports[i].fn);
                if (n > 0) used += (size_t)n;
            }
            logf_("build: %s (TimeDateStamp %08lX, .text %08lX), delta 0x%08X from 0x%08X; "
                  "import slots %d/%d found by name%s%s", build_name(), g_buildStamp, g_buildText,
                  (unsigned)g_delta, (unsigned)IMAGE_BASE, found, IMP_COUNT,
                  *missing ? ", not imported:" : "", missing);
        }
        logf_("diagnostics: crash_dump=%d exit_trace=%d (previous log kept as witcher2_enginefix.prev.log)",
              cfg_crash_dump, cfg_exit_trace);
        // Replaced by the game's static initializer right after; crash_tick re-arms.
        if (cfg_enabled && cfg_crash_dump) g_prevFilter = SetUnhandledExceptionFilter(crash_filter);
        logf_("config: enabled=%d enforce=%d mesh=%.2f foliage=%.2f downscale=%d hotkeys=%d",
              cfg_enabled, cfg_enforce, cfg_mesh, cfg_foliage, cfg_enforce_down, cfg_hotkeys);
        logf_("distance config: experimental=%d visibility=%.2fx shadow=%.2fx",
              cfg_distance, cfg_visibility, cfg_shadow_range);
        logf_("saves: fix_thumbnails=%d save_trace=%d", cfg_save_thumbs, cfg_save_trace);
        logf_("startup: skip_intro=%d fast_start=%d", cfg_skip_intro, cfg_fast_start);
        logf_("diagnostics: file_trace=%d load_trace=%d mem_trace=%d script_bindings=%d", cfg_file_trace, cfg_load_trace, cfg_mem_trace, cfg_script_bindings);
        logf_("loading: fast_doors=%d door_fade=%.2f area_fade=%.2f interiors=%d (keep_outdoors=%d keep_interiors=%d preload_interiors=%d skip_door_gc=%d release_kept=%d hide_kept=%d preload_areas=%d instant_doors=%d indoor_outdoors=%d) door_light_blend=%.2f preload_street=%d door_spinner=%d preload_memory_mb=%d door_memory_mb=%d area_memory_mb=%d", cfg_fast_doors, cfg_door_fade, cfg_area_fade, cfg_interiors, cfg_keep_outdoors, cfg_keep_interiors, cfg_preload_interiors, cfg_skip_door_gc, cfg_release_kept, cfg_hide_kept, cfg_preload_areas, cfg_instant_doors, cfg_indoor_outdoors, cfg_door_light_blend, cfg_preload_street, cfg_door_spinner, cfg_preload_memory_mb, cfg_door_memory_mb, cfg_area_memory_mb);
        logf_("scriptpatches: book_marker=%d continue=%d pause_quit=%d window_mode=%d medallion=%d",
              cfg_book_marker, cfg_continue, cfg_pause_quit, cfg_window_mode, cfg_medallion);
        logf_("window: enabled=%d borderless=%d dpi_aware=%d",
              cfg_wf_enabled, cfg_wf_borderless, cfg_wf_dpi);
        if (cfg_preset_mode)
            logf_("presets: default=%s, cycle modifiers=%d VK=%02X (Options > Graphics remembers the level)",
                  cfg_presets[g_preset].name, cfg_mod_preset, cfg_vk_preset);
        build_gate();
        if (cfg_enabled) exit_trace_install();
        // Before the renderer is constructed.
        if (cfg_enabled && cfg_cascade) {
            HANDLE h = CreateThread(NULL, 0, shadow_thread, NULL, 0, NULL);
            if (h) CloseHandle(h); else logf_("shadows: CreateThread failed - stays the game's");
        }
        // Before the game's own start-up can reach the CPU count.
        if (cfg_enabled && cfg_cpu32_guard && !cpu32_try()) {
            HANDLE h = CreateThread(NULL, 0, cpu32_thread, NULL, 0, NULL);
            if (h) CloseHandle(h); else logf_("cpu32: CreateThread failed - off");
        }
        if (cfg_enabled || cfg_file_trace || cfg_load_trace) {
            g_running = 1;
            if (cfg_enabled) {
                HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
                if (h) CloseHandle(h); else logf_("CreateThread failed");
                if (cfg_script_bindings) {
                    h = CreateThread(NULL, 0, scriptbind_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h);
                    else { logf_("script bindings: CreateThread failed - off"); cfg_script_bindings = 0; }
                }
                if (cfg_wf_enabled) {
                    h = CreateThread(NULL, 0, window_init_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("window: CreateThread failed - borderless off");
                }
                if (cfg_skip_intro) {
                    h = CreateThread(NULL, 0, startvideo_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("startup: CreateThread failed - skip_intro off");
                }
                if (cfg_fast_start) {
                    g_startMainTid = GetCurrentThreadId();
                    h = CreateThread(NULL, 0, fast_start_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("startup: CreateThread failed - fast_start off");
                }
                if (cfg_light_previews) {
                    h = CreateThread(NULL, 0, light_previews_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("saves: CreateThread failed - light_previews off");
                }
                if (cfg_mem_trace) {
                    h = CreateThread(NULL, 0, memtrace_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("memtrace: CreateThread failed - off");
                }
            }
            if (cfg_file_trace) {
                HANDLE h = CreateThread(NULL, 0, filetrace_thread, NULL, 0, NULL);
                if (h) CloseHandle(h);
                else { logf_("file-trace: CreateThread failed - file trace off"); cfg_file_trace = 0; }
            }
            if (cfg_load_trace) {
                HANDLE h;
                g_ltMainTid = GetCurrentThreadId();   // DllMain runs on the game's main thread
                g_ltTls = TlsAlloc();
                h = g_ltTls != TLS_OUT_OF_INDEXES ? CreateThread(NULL, 0, loadtrace_thread, NULL, 0, NULL) : NULL;
                if (h) CloseHandle(h);
                else { logf_("load-trace: TlsAlloc or CreateThread failed - load trace off"); cfg_load_trace = 0; }
            }
            if (cfg_enabled && (cfg_book_marker || cfg_continue || cfg_pause_quit || cfg_window_mode ||
                                cfg_medallion)) {
                wchar_t sgame[MAX_PATH], scache[MAX_PATH];

                if (script_dirs(sgame, scache) &&
                    sp_hooks_start(sgame, scache, cfg_book_marker, cfg_continue, cfg_pause_quit,
                                   cfg_window_mode, cfg_medallion, logf_)) {
                    g_scriptHooks = 1;
                } else {
                    logf_("script patches: cannot locate the game or cache folder - off");
                }
            }
            if (g_scriptHooks) {
                HANDLE h = CreateThread(NULL, 0, scripthooks_thread, NULL, 0, NULL);
                if (h) CloseHandle(h);
                else { logf_("script patches: CreateThread failed - off"); g_scriptHooks = 0; }
            }
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_running = 0;
        // On process exit (reserved != NULL) Windows has already ended every other thread, maybe
        // one inside logf_ holding the log lock (the worker logs the CRT's filter change during
        // GOG's shutdown). Waiting for an orphaned lock at that point makes Windows end the
        // process on the spot, skipping the remaining DLL detach work; so only try the lock and
        // leave the last lines out when it is taken. Every writer to g_log holds this lock.
        if (g_log && (!reserved || TryEnterCriticalSection(&g_logLock))) {
            if (g_factsNoWorld)
                logf_("stability: quit guard: FactsAdd ran without a world %ld time(s), time 0 used",
                      (long)g_factsNoWorld);
            if (g_pathFlat)
                logf_("stability: path guard: %ld pathfinding line(s) of zero length answered this session",
                      (long)g_pathFlat);
            if (g_lsFreed)
                logf_("stability: layer storage: %ld saved entity state(s) freed this session (%lld MB)",
                      (long)g_lsFreed, (long long)(g_lsBytes >> 20));
            logf_("unloaded");
            fclose(g_log);
            g_log = NULL;
            if (reserved) LeaveCriticalSection(&g_logLock);
        } else if (g_log) {
            // The lock's owner was ended inside logf_: one line straight to the file handle, so
            // the log shows that the detach ran.
            char line[160];
            sprintf_s(line, sizeof line, "unloaded (the log lock was held by thread %lu, already ended; quit guard: %ld)",
                      (DWORD)(uintptr_t)g_logLock.OwningThread, (long)g_factsNoWorld);
            log_raw(line);
        }
    }
    return TRUE;
}
