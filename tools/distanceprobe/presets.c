// Exercise startup config and the actual hotkey dispatch, without a game process.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int test_mods, test_f9, test_f10, test_focused = 1;
static SHORT WINAPI probe_key_state(int key) {
    int down = key == VK_CONTROL ? test_mods & 1 :
               key == VK_SHIFT ? test_mods & 4 :
               key == VK_MENU ? test_mods & 2 :
               key == VK_F9 ? test_f9 : key == VK_F10 ? test_f10 : 0;
    return down ? (-32767-1) : 0;
}
static HWND WINAPI probe_foreground(void) { return (HWND)(uintptr_t)1; }
static DWORD WINAPI probe_window_pid(HWND wnd, LPDWORD pid) {
    (void)wnd;
    *pid = test_focused ? GetCurrentProcessId() : 0;
    return 1;
}
#define GetAsyncKeyState probe_key_state
#define GetForegroundWindow probe_foreground
#define GetWindowThreadProcessId probe_window_pid
#include "../../src/enginefix.c"
#undef GetAsyncKeyState
#undef GetForegroundWindow
#undef GetWindowThreadProcessId
static int failures;
static void check(const char* label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}
static void keys(int mods, int f9, int f10) {
    test_mods=mods; test_f9=f9; test_f10=f10;
    poll_hotkeys(); poll_distance_hotkeys();
}
static void cascade_keys(int mods, int f10) {
    test_mods=mods; test_f9=0; test_f10=f10;
    poll_hotkeys(); cascade_tick();
}
static int shadow_is(int level, float first, float mid, float len, float filter, int atlas) {
    return g_shadow==level && ef_cascade_first_len==first && ef_cascade_mid_len==mid && ef_cascade_len==len &&
           ef_filter_factor==filter && g_shadowAtlasWant==atlas;
}
static int values(float m, float f, float v, float s) {
    return cfg_mesh==m && cfg_foliage==f && cfg_visibility==v && cfg_shadow_range==s &&
           ef_range_sq==v*v && ef_shadow_sq==s*s;
}
static void config(const char* text) {
    FILE* file = NULL;
    keys(0,0,0);
    fopen_s(&file, g_ini, "w");
    if (!file) { fprintf(stderr,"Cannot write probe ini\n"); exit(2); }
    fputs("[general]\nlog=0\n[osd]\nenabled=0\n",file);
    fputs(text,file); fclose(file);
    WritePrivateProfileStringA(NULL,NULL,NULL,g_ini); // discard Win32 profile cache
    load_config();
}
int main(void) {
    char buf[32];
    GetFullPathNameA("build\\preset-probe.ini",MAX_PATH,g_ini,NULL);
    cfg_osd=0; cfg_log=0;
    config("[presets]\nenabled=1\ndefault=extreme\n[lod]\nenforce=0\n[distance]\nenabled=0\n");
    check("configured startup preset sets all four controls and squared thresholds", values(8,5,3,3));
    check("preset mode activates LOD enforcement and existing distance hooks", cfg_enforce && cfg_distance);
    keys(1,0,1);
    check("Ctrl+F10 wraps extreme to vanilla together", values(1,1,1,1));
    keys(1,0,1);
    check("held hotkey does not repeat", values(1,1,1,1));
    keys(0,0,0); keys(1,0,1);
    check("next press selects high", values(2,2,1.5f,1.5f));
    keys(0,0,0); keys(1,0,1);
    check("next press selects ultra", values(4,3,2,2));
    keys(0,0,0); keys(5,0,1);
    check("old shifted control cannot alter a preset", values(4,3,2,2));
    keys(1,0,1);
    check("releasing Shift with F10 held cannot advance preset", values(4,3,2,2));
    keys(0,0,0); keys(1,1,0);
    check("old mesh key cannot alter a preset", values(4,3,2,2));
    keys(0,0,0); test_focused=0; keys(1,0,1);
    check("background keypress cannot change preset", values(4,3,2,2));
    test_focused=1; keys(1,0,1);
    check("focus return while F10 held cannot advance preset", values(4,3,2,2));
    keys(0,0,0); keys(1,0,1);
    check("foreground new press selects extreme", values(8,5,3,3));
    GetPrivateProfileStringA("presets","default","",buf,sizeof buf,g_ini);
    check("cycling never changes the configured startup preset", !strcmp(buf,"extreme"));

    // Shadow quality: its own binding, independent of the presets. Install is the probe's job.
    config("[presets]\nenabled=1\ndefault=extreme\n");
    ef_cascade_active = 1;
    g_shadowSync = 0;
    check("Shadow quality is on and starts at vanilla without a section", cfg_cascade && shadow_is(0,7,17,40,1,1024));
    cascade_keys(5,1);
    check("Ctrl+Shift+F10 selects high and leaves the preset", shadow_is(1,12,12,40,0.6f,2048) && values(8,5,3,3));
    check("the row follows the hotkey", g_shadowSync==1);
    cascade_keys(5,1);
    check("held shadow hotkey does not repeat", shadow_is(1,12,12,40,0.6f,2048));
    cascade_keys(0,0); cascade_keys(5,1);
    check("next press selects ultra", shadow_is(2,12,12,40,0.5f,4096));
    cascade_keys(0,0); cascade_keys(5,1);
    check("next press wraps to vanilla, the game's values", shadow_is(0,7,17,40,1,1024));
    cascade_keys(0,0); cascade_keys(1,1);
    check("Ctrl+F10 moves the preset, not the shadows", shadow_is(0,7,17,40,1,1024) && values(1,1,1,1));
    cascade_keys(5,1);
    check("adding Shift with F10 held cannot advance the shadows", shadow_is(0,7,17,40,1,1024));
    cascade_keys(0,0); test_focused=0; cascade_keys(5,1);
    check("background keypress cannot change the shadows", shadow_is(0,7,17,40,1,1024));
    test_focused=1; cascade_keys(5,1);
    check("focus return while F10 held cannot advance the shadows", shadow_is(0,7,17,40,1,1024));
    cascade_keys(0,0);
    g_shadowSync = 0;
    g_shadowMenu = 2;
    cascade_keys(0,0);
    check("the options row selects a level", shadow_is(2,12,12,40,0.5f,4096));
    check("a row choice is not written back", g_shadowSync==0 && g_shadowMenu==-1);
    config("[presets]\nenabled=1\n[shadows]\nquality=ULTRA\n");
    check("quality sets the startup level, in any case", shadow_is(2,12,12,40,0.5f,4096));
    config("[presets]\nenabled=1\n[shadows]\nquality=nonsense\n");
    check("invalid quality means vanilla", shadow_is(0,7,17,40,1,1024));
    config("[presets]\nenabled=1\n[shadows]\nquality=high\n[shadows.high]\nfirst_distance=14\nseam_distance=48\nsun_distance=128\natlas=4096\nfilter=0.8\n");
    check("a level's values come from the ini", shadow_is(1,14,34,80,0.8f,4096));
    config("[presets]\nenabled=1\n[shadows]\nquality=high\n[shadows.high]\nfirst_distance=1\nseam_distance=1\nsun_distance=1\natlas=3000\nfilter=5\n");
    check("invalid fields use the level's built-in values", shadow_is(1,12,12,40,0.6f,2048));
    config("[presets]\nenabled=1\n[shadows]\ncycle=F10\n");
    check("unsafe shadow binding falls back to Ctrl+Shift+F10",
        cfg_vk_shadow==VK_F10 && cfg_mod_shadow==(EF_MOD_CTRL|EF_MOD_SHIFT));
    config("[presets]\nenabled=0\n[shadows]\nquality=vanilla\n");
    cascade_keys(5,1); cascade_keys(0,0);
    check("legacy mode keeps Ctrl+Shift+F10 for the mesh shadow control", shadow_is(0,7,17,40,1,1024));
    config("[presets]\nenabled=1\n[shadows]\nenabled=0\n");
    check("enabled=0 turns the feature off", !cfg_cascade);
    config("[presets]\nenabled=1\ndefault=baseline\n[preset.baseline]\nmesh_scale=1.5\n");
    check("an older ini's baseline still means vanilla", g_preset==0 && cfg_mesh==1.5f);
    config("[presets]\nenabled=1\ndefault=vanilla\n[preset.baseline]\nmesh_scale=1.5\n[preset.vanilla]\nmesh_scale=1.25\n");
    check("preset.vanilla wins over preset.baseline", g_preset==0 && cfg_mesh==1.25f);
    config("[presets]\nenabled=1\n");
    ef_cascade_active = 0;

    config("[presets]\nenabled=1\ndefault=HIGH\n[preset.high]\nmesh_scale=6\nfoliage_scale=4\nvisibility_scale=2.5\nshadow_scale=1.5\n");
    check("case-insensitive name loads user-defined preset values", values(6,4,2.5f,1.5f));
    check("a preset without billboard_scale keeps its built-in factor", ef_billboard_factor == 1.5f);
    config("[presets]\nenabled=1\ndefault=high\n[preset.high]\nbillboard_scale=6\n");
    check("billboard_scale is read per preset", ef_billboard_factor == 6.0f);
    config("[presets]\nenabled=1\ndefault=high\n[preset.high]\nbillboard_scale=9\n");
    check("an out-of-range billboard_scale means the built-in factor", ef_billboard_factor == 1.5f);
    config("[presets]\nenabled=1\ndefault=vanilla\n");
    check("vanilla keeps the game's billboard distance", ef_billboard_factor == 1.0f);
    // A level picked while the atlas switches is not a refusal: the next frame switches again.
    g_shadowAtlasWant = 4096; g_shadowAtlasNow = 1024; g_shadowAllocRan = 1; g_shadowResult = 0; g_shadowStage = 1;
    shadow_switch_end();
    check("a level picked during a switch stays wanted", g_shadowAtlasWant == 4096 && g_shadowAtlasNow == 1024 &&
        g_shadowResult == 1 && g_shadowStage == 0);
    g_shadowAtlasWant = 4096; g_shadowAtlasNow = 1024; g_shadowAllocRan = 0; g_shadowResult = 0; g_shadowStage = 1;
    shadow_switch_end();
    check("a Reset that never reached the allocator is a refusal", g_shadowAtlasWant == 1024 && g_shadowResult == -1 &&
        g_shadowStage == 0);
    g_shadowAtlasWant = g_shadowAtlasNow = 1024; g_shadowResult = 0;
    config("[presets]\nenabled=1\ndefault=high\n[preset.high]\nmesh_scale=nan\nfoliage_scale=-2\nvisibility_scale=99\nshadow_scale=garbage\n");
    check("invalid per-field values use that preset's built-in defaults", values(2,2,1.5f,1.5f));
    config("[presets]\nenabled=1\ndefault=typo\ncycle=F9\n");
    check("invalid startup name falls back to extreme", values(8,5,3,3));
    keys(1,0,1);
    check("unsafe preset binding falls back to Ctrl+F10", values(1,1,1,1));
    config("[presets]\nenabled=1\ndefault=vanilla\ncycle=Ctrl+F9\n");
    keys(1,1,0);
    check("preset binding can use already-filtered Ctrl+F9", values(2,2,1.5f,1.5f));
    config("[presets]\nenabled=0\n[lod]\nmesh_scale=6\nfoliage_scale=4\n[distance]\nenabled=0\nvisibility_scale=2\nshadow_scale=3\n");
    check("disabled preset mode retains legacy settings and distance opt-out", values(6,4,2,3) && !cfg_distance);
    config("[lod]\nmesh_scale=5\nfoliage_scale=3\n");
    check("old ini without preset section retains legacy startup behavior", values(5,3,1,1) && !cfg_distance);
    DeleteFileA(g_ini);
    GetFullPathNameA("dist-files\\witcher2_enginefix.ini",MAX_PATH,g_ini,NULL);
    load_config();
    check("shipped ini selects extreme and retains keyboard suppression", cfg_preset_mode && values(8,5,3,3) && cfg_suppress==1);
    printf("%s: %d failures\n",failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
