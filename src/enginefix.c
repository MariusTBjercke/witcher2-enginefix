// witcher2_enginefix - runtime fixes for The Witcher 2: Assassins of Kings (Steam 3.5.0.26)
//
// Loads as bin\dinput8.dll (the game imports dinput8.dll for controller input) and
// forwards every export to the real system dinput8.dll. witcher2.exe on disk is never
// touched. Delete bin\dinput8.dll and bin\witcher2_enginefix.ini to uninstall.
//
// The game is built with ASLR disabled (DllCharacteristics has no DYNAMICBASE) and an
// image base of 0x00400000, so absolute addresses below are stable at runtime.
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
#include <dbghelp.h>   // MINIDUMP_* types only; MiniDumpWriteDump is resolved at runtime
#include <objbase.h>   // LPUNKNOWN / REFIID for the DirectInput8Create signature
#include <shlobj.h>    // SHGetFolderPathW: the game's Documents folder (User.ini)
#include <stdio.h>
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

#define IMAGE_BASE          0x00400000u
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

static HINSTANCE g_self;
static FILE*     g_log;
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

typedef struct {
    const char* name;
    float mesh, foliage, visibility, shadow;
} quality_preset;
#define PRESET_COUNT 4
static const quality_preset k_presets[PRESET_COUNT] = {
    { "baseline", 1.0f, 1.0f, 1.0f, 1.0f },
    { "high",     2.0f, 2.0f, 1.5f, 1.5f },
    { "ultra",    4.0f, 3.0f, 2.0f, 2.0f },
    { "extreme",  8.0f, 5.0f, 3.0f, 3.0f }
};
static quality_preset cfg_presets[PRESET_COUNT];
static int cfg_preset_mode, g_preset = PRESET_COUNT - 1;
static int cfg_vk_preset = VK_F10, cfg_mod_preset = EF_MOD_CTRL;
static int cfg_save_trace;      // [diagnostics] save_trace: log the save-thumbnail path
static int cfg_save_thumbs = 1; // [saves] fix_thumbnails: write the late save preview
static int cfg_savelist_trace;  // [diagnostics] savelist_trace: time/memory of the save list
static int cfg_list_leak = 1;   // [saves] fix_list_leak: free the preview the save list leaks
static int cfg_fast_list = 1;   // [saves] fast_list: unpack only the save header blocks (2 = every other build)
static int cfg_skip_intro;      // [startup] skip_intro: no logo/intro videos before the main menu
static int cfg_file_trace;      // [diagnostics] file_trace: how the game opens its data files
static int cfg_book_marker = 1; // [scriptpatches] book_marker: Read/Nothing new/Unread in lists
static int cfg_continue = 1;    // [scriptpatches] continue: Continue button in the main menu
static int cfg_pause_quit = 1;  // [scriptpatches] pause_quit: Quit asks first in the pause menu
static int cfg_window_mode = 1; // [scriptpatches] window_mode: Full screen / Borderless in Options
static int cfg_wf_enabled = 1;  // [window] enabled: borderless support (patch + live toggle + menu)
static int cfg_wf_borderless;   // [window] borderless: start borderless until the menu has saved it
static int cfg_wf_dpi = 1;      // [window] dpi_aware: DPI awareness when borderless starts on
static int cfg_crash_dump = 1;  // [diagnostics] crash_dump: 0 off, 1 log + minidump, 2 full dump
static int g_scriptHooks;       // sp_hooks_start succeeded: script patches are active
static volatile LONG g_scriptRedirects;

// ---- logging ------------------------------------------------------------
// One lock for whole lines: the worker, the file-trace hooks (called on any game
// thread) and the save hooks all append to the same FILE*.

static CRITICAL_SECTION g_logLock;

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
        p->mesh = preset_float(section, "mesh_scale", def->mesh, 0);
        p->foliage = preset_float(section, "foliage_scale", def->foliage, 0);
        p->visibility = preset_float(section, "visibility_scale", def->visibility, 1);
        p->shadow = preset_float(section, "shadow_scale", def->shadow, 1);
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
    cfg_skip_intro = ini_int("startup", "skip_intro", 0);
    cfg_file_trace = ini_int("diagnostics", "file_trace", 0);
    cfg_book_marker = ini_int("scriptpatches", "book_marker", 1);
    cfg_continue = ini_int("scriptpatches", "continue", 1);
    cfg_pause_quit = ini_int("scriptpatches", "pause_quit", 1);
    cfg_window_mode = ini_int("scriptpatches", "window_mode", 1);
    cfg_wf_enabled = ini_int("window", "enabled", 1);
    cfg_wf_borderless = ini_int("window", "borderless", 0);
    cfg_wf_dpi = ini_int("window", "dpi_aware", 1);
    cfg_crash_dump = ini_int("diagnostics", "crash_dump", 1);

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
    unsigned char* base = (unsigned char*)(uintptr_t)GetModuleHandleA(NULL);
    if (!base) return 0;
    const void* slot = base + RVA_SETTINGS;
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
        if (cfg_preset_mode) preset_show();
        else osd_show("LOD fix active   mesh %.1f   foliage %.1f", cfg_mesh, cfg_foliage);
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
            logf_("preset: %s mesh=%.2f foliage=%.2f visibility=%.2fx shadow=%.2fx; distance_active=%d",
                  cfg_presets[g_preset].name, cfg_mesh, cfg_foliage,
                  cfg_visibility, cfg_shadow_range, ef_distance_active);
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
              EF_RANGE_RVA, EF_SHADOW_RVA, cfg_visibility, cfg_shadow_range);
        if (cfg_preset_mode) preset_show();
        else osd_show("Distance test ready   visibility %.1fx   shadow %.1fx", cfg_visibility, cfg_shadow_range);
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
        last_stats = now;
    }
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
// No debugger, no code patch - only IAT slots of the unpacked 3.5.0.26 import table (each
// verified against GetProcAddress first) and slot 0 of the ISteamRemoteStorage vtable.
// See docs/fixes/save-previews.md.
//
//   00CAD314 DeleteFileW   00CAD320 CreateFileW   00CAD308 CopyFileW
//   00CAD4A8 D3DXSaveSurfaceToFileW   00CAD4AC D3DXSaveSurfaceToFileInMemory
//   00CAD624 SteamRemoteStorage (accessor; called, never patched)
//   DAT_01404950 engine: +0x234 CloudStorage flag, +0x230 provider, +0x114 storage state
//   DAT_0160D0ED capture pending, DAT_0160D0EE capture requested (009B747E / 009B733A)

#define IAT_COPYFILEW      0x00CAD308u
#define IAT_DELETEFILEW    0x00CAD314u
#define IAT_CREATEFILEW    0x00CAD320u
#define IAT_D3DX_SAVEFILE  0x00CAD4A8u
#define IAT_D3DX_SAVEMEM   0x00CAD4ACu
#define IAT_STEAM_RS       0x00CAD624u
#define VA_ENGINE          0x01404950u
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
    uintptr_t find = VA_MAP_FIND;
    __try {
        key = ((ImageKey_t)(uintptr_t)VA_IMAGE_KEY)(name);
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
        unsigned key = ((ImageKey_t)(uintptr_t)VA_IMAGE_KEY)(name);
        ((RemoveImage_t)(uintptr_t)VA_IMAGES_REMOVE)(self, NULL, key);
        logf_("thumbnail: menu reloads %ls", text);
    }
    if (g_leakFixOn && g_listDepth && stream && !stale && registry_holds(self, name)) {
        __try {
            if (*(void**)stream == (void*)(uintptr_t)VA_STEAM_STREAM) {
                ((void (__fastcall*)(void*, void*, int))(uintptr_t)VA_STREAM_DTOR)(stream, NULL, 1);
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
    for (const char* p = hex; *p && n < sizeof want; ) {
        unsigned v;
        if (sscanf_s(p, "%2x", &v) != 1) return 0;
        want[n++] = (unsigned char)v;
        p += 2;
        while (*p == ' ') p++;
    }
    return range_readable((void*)at, n) && memcmp((void*)at, want, n) == 0;
}

static void hook_image_registry(void)
{
    DWORD old = 0, tmp = 0;
    void** slot = (void**)(uintptr_t)VA_IMAGES_VTBL_ADD;
    if (!range_readable(slot, sizeof(void*)) || read_ptr(slot) != (void*)(uintptr_t)VA_IMAGES_ADD ||
        !code_matches(VA_IMAGES_ADD, "55 8b ec 51 53 56 57 ff 75 08 8b d9") ||
        !code_matches(VA_IMAGES_REMOVE, "55 8b ec 83 ec 10 83 65 fc 00 56 8d") ||
        !code_matches(VA_IMAGE_KEY, "55 8b ec 83 ec 18 53 56 57 33 db 68")) {
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
            free(g_thumbData); g_thumbData = NULL; g_thumbSize = 0;
        } else {
            lstrcpynA(g_saveName, name, sizeof g_saveName);
            g_saveTick = now;
        }
    }
    LeaveCriticalSection(&g_thumbLock);
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
    unsigned char* engine = (unsigned char*)read_ptr((void*)(uintptr_t)VA_ENGINE);
    if (!engine || !range_readable(engine, 0x238)) { logf_("save-trace: engine not readable"); return; }
    logf_("save-trace: engine cloud=%u provider=%08X state=%d pending=%u queued=%u",
          read_u8((uintptr_t)engine + 0x234), (unsigned)(uintptr_t)read_ptr(engine + 0x230),
          (int)(uintptr_t)read_ptr(engine + 0x114),
          read_u8(VA_CAPTURE_PENDING), read_u8(VA_CAPTURE_QUEUED));
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

// Worker thread: pair a .sav that got no BMP with the capture that finished after it.
static void thumb_tick(void)
{
    char sav[MAX_PATH], bmp[MAX_PATH];
    unsigned char* data = NULL;
    DWORD size = 0, now = GetTickCount();
    if (!cfg_save_thumbs || !g_origFileWrite || !g_remoteStorage) return;
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
#define LAZY_BLOCK         0x10000u
#define LAZY_MAX           (256u << 20)

typedef void* (__fastcall *ProvOpen_t)(void* self, void* edx, void* name, int mode);

static ProvOpen_t g_origProvOpen;
static int        g_fastBuild;      // this build uses the lazy reads
static struct {
    unsigned char* region;          // MEM_RESERVE only until a page is read
    size_t         reserved;
    void*          arr[2];          // what the memory stream reads: {data, size}
    sp_dzip        z;               // borrows the provider stream's compressed buffer
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

// `outer` is a mode-0 provider stream: inner memory stream over the compressed file.
static int lazy_attach(void* outer)
{
    void** o = (void**)outer;
    void** inner;
    sp_buf b;
    long long usize;
    __try {
        if (o[0] != (void*)(uintptr_t)VA_STEAM_STREAM) return 0;
        inner = (void**)o[0x20 / 4];
        if (!inner || inner[0] != (void*)(uintptr_t)VA_MEM_STREAM || inner[0x10 / 4] != (void*)&o[0x10 / 4])
            return 0;
        b.p = (unsigned char*)o[0x10 / 4];
        b.n = (size_t)o[0x14 / 4];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
    lazy_release();
    if (!b.p || !sp_dzip_open(&b, &g_lazy.z)) return 0;
    usize = g_lazy.z.count >= 1 ? g_lazy.z.e[0].usize : 0;
    if (usize <= 0 || usize > LAZY_MAX) {
        sp_dzip_close(&g_lazy.z);
        return 0;
    }
    g_lazy.reserved = ((size_t)usize + LAZY_BLOCK - 1) & ~(size_t)(LAZY_BLOCK - 1);
    g_lazy.region = (unsigned char*)VirtualAlloc(NULL, g_lazy.reserved, MEM_RESERVE, PAGE_READWRITE);
    if (!g_lazy.region) { g_lazy.reserved = 0; sp_dzip_close(&g_lazy.z); return 0; }
    g_lazy.arr[0] = g_lazy.region;
    g_lazy.arr[1] = (void*)(uintptr_t)usize;
    g_lazyBlocksAll += (LONG)(g_lazy.reserved / LAZY_BLOCK);
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
            if (s) __try { ((void (__fastcall*)(void*, void*, int))(uintptr_t)VA_STREAM_DTOR)(s, NULL, 1); }
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
    void** slot = (void**)(uintptr_t)VA_PROV_VTBL_OPEN;
    void** mem = (void**)(uintptr_t)VA_MEM_STREAM;
    if (!range_readable(slot, sizeof(void*)) || read_ptr(slot) != (void*)(uintptr_t)VA_PROV_OPEN ||
        !code_matches(VA_PROV_OPEN, "55 8b ec 56 57 6a 00 6a 03 6a 3c 8b f9") ||
        !code_matches(VA_PROV_STREAM, "55 8b ec 83 e4 f8 81 ec 30 01 00 00") ||
        !code_matches(VA_PROV_FORWARD, "55 8b ec 8b 49 20 8b 01 5d ff 60 04") ||
        !code_matches(VA_STREAM_DTOR, "55 8b ec 56 8b f1 8b 4e 20 c7 06 2c 2c f0 00") ||
        read_ptr((void*)(uintptr_t)VA_STEAM_STREAM) != (void*)(uintptr_t)VA_STREAM_DTOR ||
        read_ptr(&mem[1]) != (void*)(uintptr_t)VA_MEM_SERIALIZE || read_ptr(&mem[3]) != (void*)(uintptr_t)VA_MEM_SIZE ||
        !code_matches(VA_MEM_SERIALIZE, "55 8b ec 56 8b f1 8b 4e 14 8b 46 10") ||
        !code_matches(VA_MEM_SIZE, "8b 41 10 8b 40 04 c3")) {
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

static void savelist_run(void* panel, const char* who)
{
    static int builds;
    mem_view a, b;
    LONGLONG t0;
    builds++;
    // fast_list=2 alternates (odd builds fast) so one session measures both ways.
    g_fastBuild = g_origProvOpen && (cfg_fast_list == 1 || (cfg_fast_list == 2 && (builds & 1)));
    if (!cfg_savelist_trace) {
        InterlockedIncrement(&g_listDepth);
        ((SaveList_t)(uintptr_t)VA_SAVELIST)(panel);
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
    ((SaveList_t)(uintptr_t)VA_SAVELIST)(panel);
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

// Swap one verified `call target` for a call to `hook`. The rel32 lies inside one
// cache line at every site used, so the locked exchange is seen whole by the game thread.
static int patch_call(uintptr_t at, uintptr_t target, void* hook)
{
    LONG want = (LONG)(target - (at + 5)), mine = (LONG)((uintptr_t)hook - (at + 5));
    DWORD old = 0, tmp = 0;
    if (!range_readable((void*)at, 5) || *(unsigned char*)at != 0xE8) return 0;
    if (*(LONG*)(at + 1) == mine) return 1;
    if (*(LONG*)(at + 1) != want) return 0;
    if (!VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old)) return 0;
    InterlockedCompareExchange((volatile LONG*)(at + 1), mine, want);
    VirtualProtect((void*)at, 5, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return *(LONG*)(at + 1) == mine;
}

// Needs the image registry hook (g_origAddImage) for the leak fix.
static void savelist_install(void)
{
    int a, b;
    if (!code_matches(VA_SAVELIST, "55 8b ec 83 e4 f8 81 ec ec 01 00 00")) {
        logf_("savelist: list builder not recognised - leak fix, fast_list and trace off");
        return;
    }
    if (cfg_list_leak) {
        if (g_origAddImage &&
            code_matches(VA_MAP_FIND, "55 8b ec 83 ec 0c 56 ff 75 08 8b f0") &&
            code_matches(VA_STREAM_DTOR, "55 8b ec 56 8b f1 8b 4e 20 c7 06 2c 2c f0 00") &&
            read_ptr((void*)(uintptr_t)VA_STEAM_STREAM) == (void*)(uintptr_t)VA_STREAM_DTOR)
            g_leakFixOn = 1;
        else
            logf_("savelist: registry lookup or stream destructor not recognised - leak fix off");
    }
    if (!g_leakFixOn && !cfg_savelist_trace && !cfg_fast_list) return;
    if ((cfg_fast_list || cfg_savelist_trace) && !hook_provider_open()) cfg_fast_list = 0;
    a = patch_call(VA_SAVELIST_CALL_A, VA_SAVELIST, (void*)savelist_startup);
    b = patch_call(VA_SAVELIST_CALL_B, VA_SAVELIST, (void*)savelist_menu);
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
// So the build call at 009B588F only arms the skip; the wrapped tick calls 009A1F2E once
// a startup movie has played for STARTVIDEO_MIN_MS of real time, until +0xAC is set.
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
        mov ecx, VA_STARTVIDEO_LIST
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
        mov eax, VA_STARTVIDEO_STOP
        call eax
        pop esi
        ret 4
    }
}
#pragma warning(pop)

static void __fastcall startvideo_tick(void* self, void* edx, float dt)
{
    unsigned char* p = (unsigned char*)self;
    ((StartTick_t)(uintptr_t)VA_STARTVIDEO_TICK)(self, edx, dt);
    if (self != g_skipPlayer) return;
    if (p[0xAC] || GetTickCount() - g_skipArmedAt > STARTVIDEO_MAX_MS) {
        g_skipPlayer = NULL;
        InterlockedExchange(&g_skipDone, p[0xAC] ? 1 : 2);
        return;
    }
    if (!p[0xAD]) { g_skipPlayAt = 0; return; }
    if (!g_skipPlayAt) { g_skipPlayAt = GetTickCount(); return; }
    if (GetTickCount() - g_skipPlayAt < STARTVIDEO_MIN_MS) return;
    g_skipPlayAt = 0;
    InterlockedIncrement(&g_skipStops);
    logf_("startup: stopping startup movie %lu of %lu", *(DWORD*)(p + 0xB0), *(DWORD*)(p + 0xA0));
    startvideo_stop(self);
}

// The code is SteamStub-encrypted until the stub has run, and 009B581C runs a few
// seconds later, so poll closely from DllMain's thread start instead of the worker's
// 1.5 s delay.
static DWORD WINAPI startvideo_thread(LPVOID param)
{
    DWORD t0 = GetTickCount(), old = 0, tmp = 0;
    (void)param;
    while (g_running && GetTickCount() - t0 < 120000) {
        if (code_matches(VA_STARTVIDEO_CALL - 2, "8b c6 e8 fc c8 fe ff")) {
            if (!code_matches(VA_STARTVIDEO_LIST, "55 8b ec 83 e4 f8 83 ec 34 53 56 05 9c 00 00 00") ||
                !code_matches(VA_STARTVIDEO_TICK + 0x1F, "8b 86 b0 00 00 00 8b 8e a0 00 00 00 3b c1 72 1b") ||
                !code_matches(VA_STARTVIDEO_STOP, "55 8b ec 83 e4 f8 83 ec 10 53 33 db 57 68 1c 38") ||
                read_ptr((void*)(uintptr_t)VA_STARTVIDEO_VSLOT) != (void*)(uintptr_t)VA_STARTVIDEO_TICK) {
                logf_("startup: movie code not recognised - skip_intro off");
                return 0;
            }
            if (!VirtualProtect((void*)(uintptr_t)VA_STARTVIDEO_VSLOT, 4, PAGE_READWRITE, &old)) {
                logf_("startup: movie tick slot not writable - skip_intro off");
                return 0;
            }
            InterlockedExchangePointer((void* volatile*)(uintptr_t)VA_STARTVIDEO_VSLOT, (void*)startvideo_tick);
            VirtualProtect((void*)(uintptr_t)VA_STARTVIDEO_VSLOT, 4, old, &tmp);
            if (patch_call(VA_STARTVIDEO_CALL, VA_STARTVIDEO_LIST, (void*)startvideo_build_hook))
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

// Replace one import slot only if it still holds the expected export.
static int patch_iat(const char* tag, uintptr_t slot, const char* dll, const char* fn, void* hook, void** orig)
{
    HMODULE mod = GetModuleHandleA(dll);
    void* expect = mod ? (void*)GetProcAddress(mod, fn) : NULL;
    void* have = range_readable((void*)slot, sizeof(void*)) ? read_ptr((void*)slot) : NULL;
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

// ---- file trace (research for the DLL script patches) -------------------
// [diagnostics] file_trace=1 answers how the game opens its data - questions Q1-Q6 of
// docs/plans/2026-10-31-dll-script-patches.md; results in docs/fixes/dll-script-patches.md.
// Passive: every hook calls the real function first and changes nothing. The slots are
// the unpacked 3.5.0.26 kernel32 imports, re-checked against the import table of
// witcher2.exe.unpacked.exe:
//   00CAD0B4 CreateFileMappingW  00CAD0BC GetFileAttributesExW  00CAD318 GetFileAttributesW
//   00CAD198 FindFirstFileExW    00CAD264 FindNextFileW        00CAD26C FindFirstFileW
//   00CAD280 GetFileSize         00CAD290 GetFileSizeEx        00CAD320 CreateFileW
// The game imports the ANSI variants as well (CreateFileA 00CAD234, GetFileAttributesA
// 00CAD1D8, FindFirstFileA 00CAD1DC, FindNextFileA 00CAD1E0) and reports sizes/times
// through GetFileInformationByHandle (00CAD0A8), so those are traced too - missing them
// would make a "not opened" answer wrong.
// CreateFileW is shared with save_trace: one hook chains both behaviours.
//
// The Windows loader fills the original IAT (KERNEL32 thunks at 0x00CAD070) before any
// DllMain runs, so the slots are already usable there; filetrace_thread still polls for
// the real kernel32 export and keeps watching for a later rewrite (Q2).

#define IAT_CREATEFILEA        0x00CAD234u
#define IAT_CREATEFILEMAPPINGW 0x00CAD0B4u
#define IAT_GETFILEATTREXW     0x00CAD0BCu
#define IAT_GETFILEATTRA       0x00CAD1D8u
#define IAT_GETFILEINFO        0x00CAD0A8u
#define IAT_FINDFIRSTFILEA     0x00CAD1DCu
#define IAT_FINDFIRSTFILEEXW   0x00CAD198u
#define IAT_FINDNEXTFILEA      0x00CAD1E0u
#define IAT_FINDNEXTFILEW      0x00CAD264u
#define IAT_FINDFIRSTFILEW     0x00CAD26Cu
#define IAT_GETFILESIZE        0x00CAD280u
#define IAT_GETFILESIZEEX      0x00CAD290u
#define IAT_GETFILEATTRW       0x00CAD318u

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

// Absolute form of a path the script redirect might own: only CookedPC paths matter, and the
// game opens its data with absolute paths (docs/fixes/dll-script-patches.md, Q1).
static int script_full_path(const wchar_t* path, wchar_t* full)
{
    DWORD n;

    full[0] = 0;
    if (!path || !*path) return 0;
    if (path[0] == L'\\' || (path[0] && path[1] == L':')) {
        if (!wcsistr(path, L"cookedpc")) return 0;
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

static HANDLE WINAPI hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    wchar_t scriptFull[MAX_PATH], scriptCache[MAX_PATH];
    const wchar_t* use = path;
    HANDLE h;
    DWORD err;

    if (g_scriptHooks && script_full_path(path, scriptFull) &&
        sp_hooks_path(scriptFull, scriptCache, MAX_PATH)) {
        use = scriptCache;
        if (InterlockedIncrement(&g_scriptRedirects) <= 8)
            logf_("script patches: CreateFileW \"%ls\" -> cached copy", path);
    }
    h = g_origCreateFileW(use, access, share, sa, disp, flags, tmpl);
    err = GetLastError();
    if (use != path && h == INVALID_HANDLE_VALUE) {
        logf_("script patches: cached copy for \"%ls\" did not open (err %lu) - using the original",
              path, err);
        h = g_origCreateFileW(path, access, share, sa, disp, flags, tmpl);
        err = GetLastError();
    }
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

static void* ft_slot(uintptr_t slot)
{
    return range_readable((void*)slot, sizeof(void*)) ? read_ptr((void*)slot) : NULL;
}

// Install all trace slots; returns how many hold our hooks afterwards.
static int ft_install(void)
{
    int n = 0;
    n += patch_iat("file-trace", IAT_CREATEFILEW, "kernel32.dll", "CreateFileW",
                   (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
    n += patch_iat("file-trace", IAT_CREATEFILEA, "kernel32.dll", "CreateFileA",
                   (void*)hook_CreateFileA, (void**)&g_origCreateFileA);
    n += patch_iat("file-trace", IAT_GETFILEATTREXW, "kernel32.dll", "GetFileAttributesExW",
                   (void*)hook_GetFileAttributesExW, (void**)&g_origGetFileAttributesExW);
    n += patch_iat("file-trace", IAT_GETFILEATTRW, "kernel32.dll", "GetFileAttributesW",
                   (void*)hook_GetFileAttributesW, (void**)&g_origGetFileAttributesW);
    n += patch_iat("file-trace", IAT_GETFILEATTRA, "kernel32.dll", "GetFileAttributesA",
                   (void*)hook_GetFileAttributesA, (void**)&g_origGetFileAttributesA);
    n += patch_iat("file-trace", IAT_GETFILEINFO, "kernel32.dll", "GetFileInformationByHandle",
                   (void*)hook_GetFileInformationByHandle, (void**)&g_origGetFileInformationByHandle);
    n += patch_iat("file-trace", IAT_FINDFIRSTFILEW, "kernel32.dll", "FindFirstFileW",
                   (void*)hook_FindFirstFileW, (void**)&g_origFindFirstFileW);
    n += patch_iat("file-trace", IAT_FINDFIRSTFILEA, "kernel32.dll", "FindFirstFileA",
                   (void*)hook_FindFirstFileA, (void**)&g_origFindFirstFileA);
    n += patch_iat("file-trace", IAT_FINDFIRSTFILEEXW, "kernel32.dll", "FindFirstFileExW",
                   (void*)hook_FindFirstFileExW, (void**)&g_origFindFirstFileExW);
    n += patch_iat("file-trace", IAT_FINDNEXTFILEW, "kernel32.dll", "FindNextFileW",
                   (void*)hook_FindNextFileW, (void**)&g_origFindNextFileW);
    n += patch_iat("file-trace", IAT_FINDNEXTFILEA, "kernel32.dll", "FindNextFileA",
                   (void*)hook_FindNextFileA, (void**)&g_origFindNextFileA);
    n += patch_iat("file-trace", IAT_GETFILESIZE, "kernel32.dll", "GetFileSize",
                   (void*)hook_GetFileSize, (void**)&g_origGetFileSize);
    n += patch_iat("file-trace", IAT_GETFILESIZEEX, "kernel32.dll", "GetFileSizeEx",
                   (void*)hook_GetFileSizeEx, (void**)&g_origGetFileSizeEx);
    n += patch_iat("file-trace", IAT_CREATEFILEMAPPINGW, "kernel32.dll", "CreateFileMappingW",
                   (void*)hook_CreateFileMappingW, (void**)&g_origCreateFileMappingW);
    return n;
}

static int ft_hooked(void)
{
    int n = 0;
    n += ft_slot(IAT_CREATEFILEW) == (void*)hook_CreateFileW;
    n += ft_slot(IAT_CREATEFILEA) == (void*)hook_CreateFileA;
    n += ft_slot(IAT_GETFILEATTREXW) == (void*)hook_GetFileAttributesExW;
    n += ft_slot(IAT_GETFILEATTRW) == (void*)hook_GetFileAttributesW;
    n += ft_slot(IAT_GETFILEATTRA) == (void*)hook_GetFileAttributesA;
    n += ft_slot(IAT_GETFILEINFO) == (void*)hook_GetFileInformationByHandle;
    n += ft_slot(IAT_FINDFIRSTFILEW) == (void*)hook_FindFirstFileW;
    n += ft_slot(IAT_FINDFIRSTFILEA) == (void*)hook_FindFirstFileA;
    n += ft_slot(IAT_FINDFIRSTFILEEXW) == (void*)hook_FindFirstFileExW;
    n += ft_slot(IAT_FINDNEXTFILEW) == (void*)hook_FindNextFileW;
    n += ft_slot(IAT_FINDNEXTFILEA) == (void*)hook_FindNextFileA;
    n += ft_slot(IAT_GETFILESIZE) == (void*)hook_GetFileSize;
    n += ft_slot(IAT_GETFILESIZEEX) == (void*)hook_GetFileSizeEx;
    n += ft_slot(IAT_CREATEFILEMAPPINGW) == (void*)hook_CreateFileMappingW;
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
          (unsigned)(uintptr_t)ft_slot(IAT_CREATEFILEW));
    for (;;) {
        if (!k32) k32 = GetModuleHandleA("kernel32.dll");
        if (k32 && !expect) expect = (void*)GetProcAddress(k32, "CreateFileW");
        if (expect && ft_slot(IAT_CREATEFILEW) == expect) break;
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
        if (expect && ft_slot(IAT_CREATEFILEW) == expect) break;
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
    n += patch_iat("script-patcher", IAT_CREATEFILEW, "kernel32.dll", "CreateFileW",
                   (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
    n += patch_iat("script-patcher", IAT_FINDFIRSTFILEW, "kernel32.dll", "FindFirstFileW",
                   (void*)hook_FindFirstFileW, (void**)&g_origFindFirstFileW);
    n += patch_iat("script-patcher", IAT_FINDNEXTFILEW, "kernel32.dll", "FindNextFileW",
                   (void*)hook_FindNextFileW, (void**)&g_origFindNextFileW);
    logf_("script patches: hooks installed after %lu ms (%d/3 slots)", GetTickCount() - t0, n);
    if (ft_slot(IAT_CREATEFILEW) != (void*)hook_CreateFileW) {
        logf_("script patches: CreateFileW not hooked - the patched files cannot be served, off");
        g_scriptHooks = 0;
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
            patch_iat("save-trace", IAT_CREATEFILEW, "kernel32.dll", "CreateFileW", (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
            patch_iat("save-trace", IAT_DELETEFILEW, "kernel32.dll", "DeleteFileW", (void*)hook_DeleteFileW, (void**)&g_origDeleteFileW);
            patch_iat("save-trace", IAT_COPYFILEW, "kernel32.dll", "CopyFileW", (void*)hook_CopyFileW, (void**)&g_origCopyFileW);
            patch_iat("save-trace", IAT_D3DX_SAVEFILE, "d3dx9_39.dll", "D3DXSaveSurfaceToFileW", (void*)hook_SaveFile, (void**)&g_origSaveFile);
        }
        if (!patch_iat("save-trace", IAT_D3DX_SAVEMEM, "d3dx9_39.dll", "D3DXSaveSurfaceToFileInMemory", (void*)hook_SaveMem, (void**)&g_origSaveMem) &&
            cfg_save_thumbs) {
            logf_("thumbnail: capture import not found - fix disabled");
            cfg_save_thumbs = 0;
        }
        if (cfg_save_thumbs || cfg_savelist_trace || cfg_list_leak) hook_image_registry();
        if (cfg_savelist_trace || cfg_list_leak || cfg_fast_list) { savelist_install(); mem_tick = GetTickCount(); }
    }
    if (!steam_done && steam_tries < 600) {
        HMODULE api = GetModuleHandleA("steam_api.dll");
        SteamRS_t accessor = api ? (SteamRS_t)GetProcAddress(api, "SteamRemoteStorage") : NULL;
        steam_tries++;
        if (accessor && read_ptr((void*)(uintptr_t)IAT_STEAM_RS) == (void*)accessor) {
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
    unsigned char pending = read_u8(VA_CAPTURE_PENDING), queued = read_u8(VA_CAPTURE_QUEUED);
    if (pending != last_pending || queued != last_queued) {
        logf_("save-trace: capture flags pending=%u queued=%u", pending, queued);
        last_pending = pending; last_queued = queued;
    }
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
static volatile LONG g_fsWant = -1;      // 1 full screen, 0 windowed, -1 nothing pending
static volatile LONG g_fsResult;         // 1/2 switched to full screen/windowed, -1 Reset refused
static volatile LONG g_fsTicks;          // viewport ticks seen, so a stalled switch is noticed
static volatile LONG g_fsStage;          // 0 idle, 1 released (the hook asked for state 1)
static BOOL g_fsPrevWindowed;
static HWND g_wfWnd;
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
    if (_wcsicmp(key, L"EngineFixBorderless") == 0) {
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
    void* site = (void*)(uintptr_t)WF_STYLE_SITE;
    int64_t delta = (int64_t)(uintptr_t)wf_style_stub - (int64_t)(WF_STYLE_SITE + 5);

    if (delta < INT32_MIN || delta > INT32_MAX) return 0;   // the call must be reachable
    if (!code_matches(WF_STYLE_SITE, orig)) return 0;
    code[0] = 0xE8;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    range.start = (const unsigned char*)site;
    range.len = (unsigned)sizeof wf_style_orig;
    if (!pb_stop_peers(peers, &count, &range, 1)) return 0;
    if (!code_matches(WF_STYLE_SITE, orig) ||
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
static int wf_install_writer(void)
{
    static const char* const orig = "55 8b ec 83 e4 f8 a1 50 86 60 01";
    unsigned char* tramp;
    unsigned char code[6];
    pb_range range;
    HANDLE peers[256];
    unsigned count = 0;
    DWORD old = 0, tmp = 0;
    void* site = (void*)(uintptr_t)WF_WRITER_SITE;
    int64_t delta = (int64_t)(uintptr_t)wf_write_hook - (int64_t)(WF_WRITER_SITE + 5);

    if (delta < INT32_MIN || delta > INT32_MAX) return 0;   // the jump must be reachable
    if (!code_matches(WF_WRITER_SITE, orig)) return 0;
    tramp = (unsigned char*)wf_make_trampoline(site, wf_writer_orig, sizeof wf_writer_orig);
    if (!tramp) return 0;
    wf_real_write = (wf_write_fn)(void*)tramp;
    code[0] = 0xE9;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    code[5] = 0x90;
    range.start = (const unsigned char*)site;
    range.len = (unsigned)sizeof wf_writer_orig;
    if (!pb_stop_peers(peers, &count, &range, 1)) goto failed;
    if (!code_matches(WF_WRITER_SITE, orig) ||
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
    if (!*(void* volatile*)(uintptr_t)WF_D3D_DEVICE) return -1;
    return *(volatile BOOL*)(uintptr_t)WF_PP_WINDOWED == FALSE;
}

// The window the game itself creates for full screen (FUN_009d44e1): a topmost popup at (0,0).
// Done before the Reset, synchronously when this thread owns the window.
static void wf_fullscreen_window(void)
{
    HWND w = *(HWND volatile*)(uintptr_t)WF_PP_HWND;
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
    if ((uint32_t)cls != WF_GAME_WNDPROC) return;
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
    wf_vp_tick_fn real = (wf_vp_tick_fn)(uintptr_t)WF_VP_TICK;
    volatile LONG* state = (volatile LONG*)((char*)vp + WF_VP_STATE);
    volatile BOOL* windowed = (volatile BOOL*)(uintptr_t)WF_PP_WINDOWED;
    LONG want = g_fsWant;
    char r;

    InterlockedIncrement(&g_fsTicks);
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
    wf_reset_wndproc(*(HWND volatile*)(uintptr_t)WF_PP_HWND);
    if (*windowed == FALSE) wf_fullscreen_window();
    *state = 3;
    r = real(vp, edx);
    // Out of full screen: drop whatever DXVK left behind. Into it: DXVK's hook is wanted.
    if (*state == 0 && *windowed) wf_reset_wndproc(*(HWND volatile*)(uintptr_t)WF_PP_HWND);
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
static int wf_install_vp_tick(void)
{
    volatile LONG* slot = (volatile LONG*)(uintptr_t)WF_VP_TICK_SLOT;
    DWORD old = 0, tmp = 0;
    LONG prev;

    if ((uint32_t)*slot != WF_VP_TICK) return 0;
    if (!code_matches(WF_VP_TICK, "55 8b ec 83 e4 f8 56 57 8b f9 8b c7 e8") ||
        !code_matches(WF_VP_TICK + 0x11, "8b 87 b0 00 00 00") ||
        !code_matches(WF_RESET_COPY, "b9 0e 00 00 00 be a0 67 62 01"))
        return 0;
    if (!VirtualProtect((void*)slot, sizeof(LONG), PAGE_READWRITE, &old)) return 0;
    prev = InterlockedCompareExchange(slot, (LONG)(uintptr_t)wf_vp_tick_hook, (LONG)WF_VP_TICK);
    VirtualProtect((void*)slot, sizeof(LONG), old, &tmp);
    return (uint32_t)prev == WF_VP_TICK;
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
        if (!wf_wait_site(WF_STYLE_SITE, "bf 00 00 c8 00", 60000)) {
            logf_("window: window style site never appeared - borderless off for this start");
        } else if (!wf_install_style_before_window()) {
            if (wf_window())
                logf_("window: window created before the style patch - borderless applies by restyle");
            else
                logf_("window: startup style not patched (bytes differ or out of range) - borderless would appear after creation");
        } else {
            logf_("window: startup borderless style patched");
        }
        if (!wf_wait_site(WF_WRITER_SITE, "55 8b ec 83 e4 f8 a1 50 86 60 01", 60000)) {
            logf_("window: config writer site never appeared - the menu setting applies at the next start");
        } else if (!wf_install_writer_retry()) {
            logf_("window: menu channel not hooked (bytes differ or out of range) - the menu setting applies at the next start");
        } else {
            logf_("window: menu channel hooked (Options > Graphics applies borderless at once)");
            // Full screen follows the menu live only through the menu channel.
            if (!wf_wait_site(WF_RESET_COPY, "b9 0e 00 00 00 be a0 67 62 01", 60000) ||
                !wf_install_vp_tick()) {
                logf_("window: viewport tick not hooked (bytes differ) - Full screen applies at the next start");
            } else {
                g_fsHooked = 1;
                logf_("window: viewport tick hooked (Full screen switches at once)");
            }
        }
    }
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
            logf_("startup: %ld startup movie(s) stopped, %s", g_skipStops,
                  g_skipDone == 1 ? "main menu reached" : "gave up after 60 s");
        }
        crash_tick();
        osd_pump();
        apply_lod();
        if (cfg_wf_enabled) wf_tick();
        if (cfg_hotkeys) poll_hotkeys();
        distance_tick();
        save_tick();
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
static GDS_t g_origGDS;

static HRESULT WINAPI hook_GetDeviceState(void* self, DWORD cb, void* data)
{
    if (!g_origGDS) return E_FAIL;
    HRESULT hr = g_origGDS(self, cb, data);
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
static CD_t g_origCreateDevice;

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
static SCL_t g_origSCL;
static int g_wkThread, g_wkLogged, g_wkProcHooked = -1;
static void* volatile g_wkKeyboard;   // the keyboard the game created last

// The window object's procedure: in DirectInput mode the keyboard comes from DirectInput only.
static LRESULT __fastcall wk_window_proc(void* obj, void* edx, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg >= WM_KEYFIRST && msg <= WM_KEYLAST &&
        *(volatile LONG*)((char*)obj + WK_INPUT_MODE) == 3)
        return 0;
    return ((wk_wndproc_fn)(uintptr_t)WK_WNDPROC)(obj, edx, msg, wp, lp);
}

// Slot write in .rdata, aligned and atomic; fingerprinted by the slot value, the procedure's
// prologue (push ebp; mov ebp,esp; and esp,-8; sub esp,0x64) and its RET 0xC.
static int wk_hook_window_proc(void)
{
    volatile LONG* slot = (volatile LONG*)(uintptr_t)WK_WNDPROC_SLOT;
    DWORD old = 0, tmp = 0;
    LONG prev;

    if (g_wkProcHooked >= 0) return g_wkProcHooked;
    g_wkProcHooked = 0;
    if ((uint32_t)*slot != WK_WNDPROC ||
        !code_matches(WK_WNDPROC, "55 8b ec 83 e4 f8 83 ec 64") ||
        !code_matches(WK_WNDPROC + 0x2F2, "c2 0c 00") ||
        !VirtualProtect((void*)slot, sizeof(LONG), PAGE_READWRITE, &old)) {
        logf_("input: window procedure not hooked (bytes differ) - the keyboard stays exclusive");
        return 0;
    }
    prev = InterlockedCompareExchange(slot, (LONG)(uintptr_t)wk_window_proc, (LONG)WK_WNDPROC);
    VirtualProtect((void*)slot, sizeof(LONG), old, &tmp);
    g_wkProcHooked = (uint32_t)prev == WK_WNDPROC;
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
    if (!g_origSCL) return E_FAIL;
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
    return g_origSCL(self, w, flags);
}

static HRESULT WINAPI hook_CreateDevice(void* self, const GUID* rguid, void** out, void* outer)
{
    if (!g_origCreateDevice) return E_FAIL;
    HRESULT hr = g_origCreateDevice(self, rguid, out, outer);
    if (SUCCEEDED(hr) && out && *out && rguid && memcmp(rguid, &kSysKeyboard, sizeof(GUID)) == 0) {
        if (cfg_suppress && patch_vtable(*out, 9, (void*)hook_GetDeviceState, (void**)&g_origGDS))
            logf_("input: keyboard state filter installed");
        if (cfg_winkeys) {
            static int logged;

            g_wkKeyboard = *out;
            if (patch_vtable(*out, 13, (void*)hook_SetCooperativeLevel, (void**)&g_origSCL) && !logged) {
                logged = 1;
                logf_("input: keyboard cooperative level hook installed");
            }
        }
    }
    return hr;
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
    HRESULT hr = fn(hinst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && (cfg_suppress || cfg_winkeys)) {
        if (patch_vtable(*out, 3, (void*)hook_CreateDevice, (void**)&g_origCreateDevice))
            logf_("input: CreateDevice hook installed");
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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_logLock);
        InitializeCriticalSection(&g_thumbLock);
        InitializeCriticalSection(&g_ftLock);
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
        logf_("witcher2_enginefix %s loaded (image base 0x%08X)", ENGINEFIX_VERSION, (unsigned)IMAGE_BASE);
        logf_("diagnostics: crash_dump=%d (previous log kept as witcher2_enginefix.prev.log)", cfg_crash_dump);
        // Replaced by the game's static initializer right after; crash_tick re-arms.
        if (cfg_enabled && cfg_crash_dump) g_prevFilter = SetUnhandledExceptionFilter(crash_filter);
        logf_("config: enabled=%d enforce=%d mesh=%.2f foliage=%.2f downscale=%d hotkeys=%d",
              cfg_enabled, cfg_enforce, cfg_mesh, cfg_foliage, cfg_enforce_down, cfg_hotkeys);
        logf_("distance config: experimental=%d visibility=%.2fx shadow=%.2fx",
              cfg_distance, cfg_visibility, cfg_shadow_range);
        logf_("saves: fix_thumbnails=%d save_trace=%d", cfg_save_thumbs, cfg_save_trace);
        logf_("startup: skip_intro=%d", cfg_skip_intro);
        logf_("diagnostics: file_trace=%d", cfg_file_trace);
        logf_("scriptpatches: book_marker=%d continue=%d pause_quit=%d window_mode=%d",
              cfg_book_marker, cfg_continue, cfg_pause_quit, cfg_window_mode);
        logf_("window: enabled=%d borderless=%d dpi_aware=%d",
              cfg_wf_enabled, cfg_wf_borderless, cfg_wf_dpi);
        if (cfg_preset_mode)
            logf_("presets: startup=%s, cycle modifiers=%d VK=%02X (session only)",
                  cfg_presets[g_preset].name, cfg_mod_preset, cfg_vk_preset);
        if (cfg_enabled || cfg_file_trace) {
            g_running = 1;
            if (cfg_enabled) {
                HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
                if (h) CloseHandle(h); else logf_("CreateThread failed");
                if (cfg_wf_enabled) {
                    h = CreateThread(NULL, 0, window_init_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("window: CreateThread failed - borderless off");
                }
                if (cfg_skip_intro) {
                    h = CreateThread(NULL, 0, startvideo_thread, NULL, 0, NULL);
                    if (h) CloseHandle(h); else logf_("startup: CreateThread failed - skip_intro off");
                }
            }
            if (cfg_file_trace) {
                HANDLE h = CreateThread(NULL, 0, filetrace_thread, NULL, 0, NULL);
                if (h) CloseHandle(h);
                else { logf_("file-trace: CreateThread failed - file trace off"); cfg_file_trace = 0; }
            }
            if (cfg_enabled && (cfg_book_marker || cfg_continue || cfg_pause_quit || cfg_window_mode)) {
                wchar_t sgame[MAX_PATH], scache[MAX_PATH];

                if (script_dirs(sgame, scache) &&
                    sp_hooks_start(sgame, scache, cfg_book_marker, cfg_continue, cfg_pause_quit,
                                   cfg_window_mode, logf_)) {
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
        if (g_log) { logf_("unloaded"); fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
