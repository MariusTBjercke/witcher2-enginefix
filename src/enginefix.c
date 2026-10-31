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
#include <objbase.h>   // LPUNKNOWN / REFIID for the DirectInput8Create signature
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>
#include "distance_patch.h"

#define ENGINEFIX_VERSION   "0.5.0"   // keep in sync with CHANGELOG.md
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
static int cfg_skip_intro;      // [startup] skip_intro: no logo/intro videos before the main menu

// ---- logging ------------------------------------------------------------

static void logf_(const char* fmt, ...)
{
    if (!g_log || !cfg_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
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
    cfg_skip_intro = ini_int("startup", "skip_intro", 0);

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
static unsigned g_seen[SEEN_MAX];   // name hashes AddImage has seen this session
static int      g_seenCount;

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

static HANDLE WINAPI hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share,
                                      LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    HANDLE h = g_origCreateFileW(path, access, share, sa, disp, flags, tmpl);
    if (trace_path(path)) {
        DWORD err = GetLastError();
        logf_("save-trace: CreateFileW \"%ls\" access=%08lX disp=%lu -> %s (err %lu)",
              path, access, disp, h == INVALID_HANDLE_VALUE ? "FAILED" : "ok", err);
        SetLastError(err);
    }
    return h;
}

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
    DWORD t0;
    if (!cfg_savelist_trace) {
        InterlockedIncrement(&g_listDepth);
        ((SaveList_t)(uintptr_t)VA_SAVELIST)(panel);
        InterlockedDecrement(&g_listDepth);
        return;
    }
    mem_snapshot(&a);
    g_listImages = g_listNewImages = g_listReadsSav = g_listReadsOther = g_listFreed = 0;
    g_listBytes = 0; g_listReadMs = 0;
    t0 = GetTickCount();
    InterlockedIncrement(&g_listDepth);
    ((SaveList_t)(uintptr_t)VA_SAVELIST)(panel);
    InterlockedDecrement(&g_listDepth);
    t0 = GetTickCount() - t0;
    mem_snapshot(&b);
    logf_("savelist: build %d (%s) %lu ms; Steam reads %ld .sav + %ld other, %lu MB in %lu ms; "
          "previews %ld (%ld new, %d held this session), leaked streams freed %ld",
          ++builds, who, t0, g_listReadsSav, g_listReadsOther, (DWORD)(g_listBytes >> 20), g_listReadMs,
          g_listImages, g_listNewImages, g_seenCount, g_listFreed);
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
        logf_("savelist: list builder not recognised - leak fix and trace off");
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
    if (!g_leakFixOn && !cfg_savelist_trace) return;
    a = patch_call(VA_SAVELIST_CALL_A, VA_SAVELIST, (void*)savelist_startup);
    b = patch_call(VA_SAVELIST_CALL_B, VA_SAVELIST, (void*)savelist_menu);
    logf_("savelist: startup call %s, menu call %s; leak fix=%d trace=%d",
          a ? "hooked" : "NOT hooked", b ? "hooked" : "NOT hooked", g_leakFixOn, cfg_savelist_trace);
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
static int patch_iat(uintptr_t slot, const char* dll, const char* fn, void* hook, void** orig)
{
    HMODULE mod = GetModuleHandleA(dll);
    void* expect = mod ? (void*)GetProcAddress(mod, fn) : NULL;
    void* have = range_readable((void*)slot, sizeof(void*)) ? read_ptr((void*)slot) : NULL;
    if (have == hook) return 1;
    if (!expect || have != expect) {
        logf_("save-trace: %s!%s slot %08X holds %08X, expected %08X - not hooked",
              dll, fn, (unsigned)slot, (unsigned)(uintptr_t)have, (unsigned)(uintptr_t)expect);
        return 0;
    }
    DWORD old = 0, tmp = 0;
    if (!VirtualProtect((void*)slot, sizeof(void*), PAGE_READWRITE, &old)) return 0;
    *orig = have;
    *(void**)slot = hook;
    VirtualProtect((void*)slot, sizeof(void*), old, &tmp);
    logf_("save-trace: hooked %s!%s", dll, fn);
    return 1;
}

static void save_tick(void)
{
    static int iat_done, steam_done, steam_tries;
    static unsigned char last_pending = 0xFE, last_queued = 0xFE;
    static DWORD mem_tick;
    if (!cfg_save_trace && !cfg_save_thumbs && !cfg_savelist_trace && !cfg_list_leak) return;
    if (!iat_done) {
        HMODULE pinned;
        iat_done = 1;
        // Import slots must not outlive this DLL if a caller releases its handle.
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                (LPCSTR)(uintptr_t)hook_CreateFileW, &pinned)) {
            logf_("saves: could not pin module - thumbnail fix and trace disabled");
            cfg_save_trace = cfg_save_thumbs = cfg_savelist_trace = cfg_list_leak = 0;
            return;
        }
        if (cfg_save_trace) {
            patch_iat(IAT_CREATEFILEW, "kernel32.dll", "CreateFileW", (void*)hook_CreateFileW, (void**)&g_origCreateFileW);
            patch_iat(IAT_DELETEFILEW, "kernel32.dll", "DeleteFileW", (void*)hook_DeleteFileW, (void**)&g_origDeleteFileW);
            patch_iat(IAT_COPYFILEW, "kernel32.dll", "CopyFileW", (void*)hook_CopyFileW, (void**)&g_origCopyFileW);
            patch_iat(IAT_D3DX_SAVEFILE, "d3dx9_39.dll", "D3DXSaveSurfaceToFileW", (void*)hook_SaveFile, (void**)&g_origSaveFile);
        }
        if (!patch_iat(IAT_D3DX_SAVEMEM, "d3dx9_39.dll", "D3DXSaveSurfaceToFileInMemory", (void*)hook_SaveMem, (void**)&g_origSaveMem) &&
            cfg_save_thumbs) {
            logf_("thumbnail: capture import not found - fix disabled");
            cfg_save_thumbs = 0;
        }
        if (cfg_save_thumbs || cfg_savelist_trace || cfg_list_leak) hook_image_registry();
        if (cfg_savelist_trace || cfg_list_leak) { savelist_install(); mem_tick = GetTickCount(); }
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

static DWORD WINAPI worker(LPVOID param)
{
    (void)param;
    Sleep(1500);   // let the loader finish; never touch loader APIs during DllMain
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
        osd_pump();
        apply_lod();
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

static HRESULT WINAPI hook_CreateDevice(void* self, const GUID* rguid, void** out, void* outer)
{
    if (!g_origCreateDevice) return E_FAIL;
    HRESULT hr = g_origCreateDevice(self, rguid, out, outer);
    if (SUCCEEDED(hr) && out && *out && rguid && memcmp(rguid, &kSysKeyboard, sizeof(GUID)) == 0) {
        if (patch_vtable(*out, 9, (void*)hook_GetDeviceState, (void**)&g_origGDS))
            logf_("input: keyboard state filter installed");
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
    if (SUCCEEDED(hr) && out && *out && cfg_suppress) {
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

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_thumbLock);
        ini_path();
        load_config();
        if (cfg_log) fopen_s(&g_log, g_logPath, "w");
        logf_("witcher2_enginefix %s loaded (image base 0x%08X)", ENGINEFIX_VERSION, (unsigned)IMAGE_BASE);
        logf_("config: enabled=%d enforce=%d mesh=%.2f foliage=%.2f downscale=%d hotkeys=%d",
              cfg_enabled, cfg_enforce, cfg_mesh, cfg_foliage, cfg_enforce_down, cfg_hotkeys);
        logf_("distance config: experimental=%d visibility=%.2fx shadow=%.2fx",
              cfg_distance, cfg_visibility, cfg_shadow_range);
        logf_("saves: fix_thumbnails=%d save_trace=%d", cfg_save_thumbs, cfg_save_trace);
        logf_("startup: skip_intro=%d", cfg_skip_intro);
        if (cfg_preset_mode)
            logf_("presets: startup=%s, cycle modifiers=%d VK=%02X (session only)",
                  cfg_presets[g_preset].name, cfg_mod_preset, cfg_vk_preset);
        if (cfg_enabled) {
            g_running = 1;
            HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
            if (h) CloseHandle(h); else logf_("CreateThread failed");
            if (cfg_skip_intro) {
                h = CreateThread(NULL, 0, startvideo_thread, NULL, 0, NULL);
                if (h) CloseHandle(h); else logf_("startup: CreateThread failed - skip_intro off");
            }
        }
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_running = 0;
        if (g_log) { logf_("unloaded"); fclose(g_log); g_log = NULL; }
    }
    return TRUE;
}
