// Build detection, import slots by name and the per-build feature gate (src/enginefix.c),
// against the real witcher2.exe files; no game run needed.
//
//   tools\buildprobe\build-and-run.bat "<steam>\bin\witcher2.exe" "<gog>\bin\witcher2.exe"
//
// Each exe is mapped as an image (LOAD_LIBRARY_AS_IMAGE_RESOURCE: sections at their RVAs, no
// code run, no imports resolved) at whatever address Windows picks, so the delta ga() adds is
// not 0 here even for Steam. The expected import slots are the fixed IAT_ values 0.9.0 used on
// Steam and, for GOG 3.5.0.26g, the slots read from its import table with a separate parser
// (2026-11-04). The gate is run with every switch on and must change nothing on Steam.
#include "../../src/enginefix.c"

static int failures;
static void check(const char* label, int ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}

static const DWORD k_steamSlots[IMP_COUNT] = {
    [IMP_CREATEFILEW] = 0x00CAD320, [IMP_CREATEFILEA] = 0x00CAD234,
    [IMP_DELETEFILEW] = 0x00CAD314, [IMP_COPYFILEW] = 0x00CAD308,
    [IMP_CREATEFILEMAPPINGW] = 0x00CAD0B4, [IMP_GETFILEATTREXW] = 0x00CAD0BC,
    [IMP_GETFILEATTRW] = 0x00CAD318, [IMP_GETFILEATTRA] = 0x00CAD1D8,
    [IMP_GETFILEINFO] = 0x00CAD0A8, [IMP_FINDFIRSTFILEW] = 0x00CAD26C,
    [IMP_FINDFIRSTFILEA] = 0x00CAD1DC, [IMP_FINDFIRSTFILEEXW] = 0x00CAD198,
    [IMP_FINDNEXTFILEW] = 0x00CAD264, [IMP_FINDNEXTFILEA] = 0x00CAD1E0,
    [IMP_GETFILESIZE] = 0x00CAD280, [IMP_GETFILESIZEEX] = 0x00CAD290,
    [IMP_READFILE] = 0x00CAD27C, [IMP_SLEEP] = 0x00CAD304,
    [IMP_WAITSINGLE] = 0x00CAD2B0, [IMP_WAITSINGLEEX] = 0x00CAD07C,
    [IMP_GETPROCAFFINITY] = 0x00CAD20C, [IMP_D3DX_SAVEFILE] = 0x00CAD4A8,
    [IMP_D3DX_SAVEMEM] = 0x00CAD4AC, [IMP_STEAM_RS] = 0x00CAD624,
};

static const DWORD k_gogSlots[IMP_COUNT] = {
    [IMP_CREATEFILEW] = 0x00D00324, [IMP_CREATEFILEA] = 0x00D00234,
    [IMP_DELETEFILEW] = 0x00D00318, [IMP_COPYFILEW] = 0x00D0030C,
    [IMP_CREATEFILEMAPPINGW] = 0x00D000BC, [IMP_GETFILEATTREXW] = 0x00D000C4,
    [IMP_GETFILEATTRW] = 0x00D0031C, [IMP_GETFILEATTRA] = 0x00D001D8,
    [IMP_GETFILEINFO] = 0x00D001A0, [IMP_FINDFIRSTFILEW] = 0x00D00268,
    [IMP_FINDFIRSTFILEA] = 0x00D001DC, [IMP_FINDFIRSTFILEEXW] = 0x00D001AC,
    [IMP_FINDNEXTFILEW] = 0x00D00260, [IMP_FINDNEXTFILEA] = 0x00D001E0,
    [IMP_GETFILESIZE] = 0x00D0027C, [IMP_GETFILESIZEEX] = 0x00D0032C,
    [IMP_READFILE] = 0x00D00278, [IMP_SLEEP] = 0x00D00308,
    [IMP_WAITSINGLE] = 0x00D002B0, [IMP_WAITSINGLEEX] = 0x00D000A0,
    [IMP_GETPROCAFFINITY] = 0x00D0020C, [IMP_D3DX_SAVEFILE] = 0x00D004DC,
    [IMP_D3DX_SAVEMEM] = 0x00D004D8, [IMP_STEAM_RS] = 0,   // GOG has no steam_api.dll
};

// Every on/off switch build_gate may touch.
static int* const k_switches[] = {
    &cfg_enforce, &cfg_enforce_down, &cfg_preset_mode, &cfg_hotkeys, &cfg_distance,
    &cfg_save_thumbs, &cfg_save_trace, &cfg_list_leak, &cfg_fast_list, &cfg_savelist_trace,
    &cfg_skip_intro, &cfg_keep_outdoors, &cfg_keep_interiors, &cfg_preload_interiors,
    &cfg_skip_door_gc, &cfg_indoor_outdoors, &cfg_fast_doors, &cfg_instant_doors,
    &cfg_load_trace, &cfg_wf_enabled, &cfg_window_mode, &cfg_winkeys, &cfg_ray_guard,
    &cfg_quit_guard, &cfg_cpu32_guard, &cfg_book_marker, &cfg_continue, &cfg_pause_quit,
    &cfg_medallion, &cfg_suppress, &cfg_crash_dump, &cfg_file_trace,
};
#define SWITCHES (sizeof k_switches / sizeof k_switches[0])

static void all_on(void)
{
    unsigned i;
    for (i = 0; i < SWITCHES; i++) *k_switches[i] = 1;
    cfg_door_fade = 0.05f;
    cfg_area_fade = 0.25f;
    g_gated = 0;
}

static uintptr_t map_exe(const char* path)
{
    HMODULE h = LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) printf("cannot map %s (%lu)\n", path, GetLastError());
    return h ? (uintptr_t)h & ~(uintptr_t)3 : 0;   // the low bits mark a resource mapping
}

static int slots_match(uintptr_t base, const DWORD* want)
{
    int i, ok = 1;
    for (i = 0; i < IMP_COUNT; i++) {
        uintptr_t expect = want[i] ? base + (want[i] - IMAGE_BASE) : 0;
        if (g_imp[i] != expect) {
            printf("  %s!%s: found %08X, expected %08X\n", k_imports[i].dll, k_imports[i].fn,
                   (unsigned)g_imp[i], (unsigned)expect);
            ok = 0;
        }
    }
    return ok;
}

int main(int argc, char** argv)
{
    uintptr_t steam, gog;
    int before[SWITCHES];
    unsigned i;
    int same;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 3) {
        printf("usage: probe <steam witcher2.exe> <gog witcher2.exe>\n");
        return 2;
    }
    InitializeCriticalSection(&g_logLock);
    g_log = stdout;
    cfg_log = 1;

    steam = map_exe(argv[1]);
    gog = map_exe(argv[2]);
    if (!steam || !gog) return 2;

    // Steam (the shipped, packed exe)
    build_read(steam);
    printf("Steam mapped at %08X, delta %08X\n", (unsigned)steam, (unsigned)g_delta);
    check("Steam: detected as Steam 3.5.0.26", g_build == BUILD_STEAM);
    check("Steam: import slots are the 0.9.0 fixed slots, moved with the image", slots_match(steam, k_steamSlots));
    check("Steam: ga adds the load delta to the Steam address", ga(VA_GAME) == 0x01404950u + g_delta);
    check("Steam: ga_rva returns the Steam RVA", ga_rva(RVA_SETTINGS) == RVA_SETTINGS && ga_rva(EF_RANGE_RVA) == EF_RANGE_RVA);
    check("Steam: every address is there", ga_have(VA_GAME, WF_PP, WK_WNDPROC, EF_SHADOW_RVA, 0));
    all_on();
    for (i = 0; i < SWITCHES; i++) before[i] = *k_switches[i];
    build_gate();
    for (same = 1, i = 0; i < SWITCHES; i++) same &= before[i] == *k_switches[i];
    check("Steam: the gate changes no switch", same && g_gated == 0 &&
          cfg_door_fade == 0.05f && cfg_area_fade == 0.25f);

    // GOG 3.5.0.26g
    build_read(gog);
    printf("GOG mapped at %08X, delta %08X\n", (unsigned)gog, (unsigned)g_delta);
    check("GOG: detected as GOG 3.5.0.26g", g_build == BUILD_GOG);
    check("GOG: import slots found by name (no SteamRemoteStorage)", slots_match(gog, k_gogSlots));
    check("GOG: unported addresses read as 0", !ga(VA_PART_SWITCH) && !ga(VA_SAVELIST) &&
          !ga_have(VA_DOOR_TIMER, 0));
    check("GOG: ported RVAs and the delta", ga_rva(RVA_SETTINGS) == 0x021EAEB8u &&
          ga_rva(EF_RANGE_RVA) == 0x0060C120u && ga_rva(EF_SHADOW_RVA) == 0x0060C4F6u &&
          ga_have(RVA_SETTINGS, EF_RANGE_RVA, EF_SHADOW_RVA, 0));
    // GOG is a DYNAMIC_BASE image: Windows relocates its shared image section once, to the
    // random base it picked for this file (the one the game gets too), even when this mapping
    // lands elsewhere. So the getter's operand is the slot at that base: preferred + a 64K step.
    printf("GOG getter operand %08X (preferred %08X)\n", *(const unsigned*)(gog + 0x24F0),
           (unsigned)(IMAGE_BASE + ga_rva(RVA_SETTINGS)));
    check("GOG: the settings getter (004024EF) reads the k_gog slot",
          *(const unsigned char*)(gog + 0x24EF) == 0xA1 &&
          ((*(const DWORD*)(gog + 0x24F0) - (IMAGE_BASE + ga_rva(RVA_SETTINGS))) & 0xFFFF) == 0);
    ef_select_build(1);
    check("GOG: the GOG distance signatures sit at the k_gog RVAs",
          !memcmp((const void*)(gog + ga_rva(EF_RANGE_RVA)), ef_range_sig, ef_range_sig_len) &&
          !memcmp((const void*)(gog + ga_rva(EF_SHADOW_RVA)), ef_shadow_sig, ef_shadow_sig_len));
    ef_select_build(0);
    check("GOG: Steam's distance signatures do not", memcmp((const void*)(gog + ga_rva(EF_RANGE_RVA)),
          ef_range_sig, ef_range_sig_len) && memcmp((const void*)(gog + ga_rva(EF_SHADOW_RVA)),
          ef_shadow_sig, ef_shadow_sig_len));
    // The DLL's own per-build checks, run against the mapped GOG code (ga() = this mapping).
    check("GOG: the quit guard's 12 bytes sit at VA_FACTS_TIME", code_matches(ga(VA_FACTS_TIME), quit_site_bytes()));
    g_startvideoList = ga(VA_STARTVIDEO_LIST);
    g_startvideoStop = ga(VA_STARTVIDEO_STOP);
    g_startvideoTick = ga(VA_STARTVIDEO_TICK);
    check("GOG: the skip-intro call site and code are recognised",
          startvideo_site_ok(ga(VA_STARTVIDEO_CALL)) && startvideo_code_ok());
    {
        uintptr_t call = ga(VA_STARTVIDEO_CALL);
        LONG rel = *(const LONG*)(call + 1);
        check("GOG: VA_STARTVIDEO_CALL calls VA_STARTVIDEO_LIST",
              *(const unsigned char*)call == 0xE8 && call + 5 + rel == g_startvideoList);
    }
    // Slots in the mapped image are relocated to the file's shared random base, not this mapping.
    check("GOG: the ray and start-movie slots hold the k_gog functions (low 16 bits)",
          ((*(const DWORD*)ga(VA_RAY_SLOT) - (DWORD)ga(VA_RAY_HIT)) & 0xFFFF) == 0 &&
          ((*(const DWORD*)ga(VA_STARTVIDEO_VSLOT) - (DWORD)g_startvideoTick) & 0xFFFF) == 0);
    check("GOG: window style site, viewport tick and window procedure are recognised",
          code_matches(ga(WF_STYLE_SITE), "bf 00 00 c8 00") &&
          code_matches(ga(WF_VP_TICK), "55 8b ec 83 e4 f8 56 57 8b f9 8b c7 e8") &&
          code_matches(ga(WF_VP_TICK) + 0x11, "8b 87 b0 00 00 00") &&
          code_matches(ga(WK_WNDPROC), "55 8b ec 83 e4 f8 83 ec 64") &&
          code_matches(ga(WK_WNDPROC) + 0x2F2, "c2 0c 00"));
    check("GOG: the config writer's bytes (GOG prologue and body start)",
          code_matches(ga(WF_WRITER_SITE), wf_writer_bytes()));
    check("GOG: the Reset copies WF_PP (operand low 16 bits)",
          code_matches(ga(WF_RESET_COPY), "b9 0e 00 00 00 be") &&
          ((*(const DWORD*)(ga(WF_RESET_COPY) + 6) - (DWORD)ga(WF_PP)) & 0xFFFF) == 0);
    check("GOG: the tick and window-procedure slots hold them (low 16 bits)",
          ((*(const DWORD*)ga(WF_VP_TICK_SLOT) - (DWORD)ga(WF_VP_TICK)) & 0xFFFF) == 0 &&
          ((*(const DWORD*)ga(WK_WNDPROC_SLOT) - (DWORD)ga(WK_WNDPROC)) & 0xFFFF) == 0);
    {
        // WriteConfigParamFloat (004EB142): mov ecx,[WF_CONFIG]; push eax; call writer.
        const unsigned char* c = (const unsigned char*)(gog + 0xEB1D8);
        LONG rel = *(const LONG*)(c + 8);
        check("GOG: the writer is called with ECX = [WF_CONFIG] (thiscall)",
              c[0] == 0x8B && c[1] == 0x0D && c[6] == 0x50 && c[7] == 0xE8 &&
              ((*(const DWORD*)(c + 2) - (DWORD)(ga(WF_CONFIG) - g_delta)) & 0xFFFF) == 0 &&
              (uintptr_t)(c + 12) + rel == ga(WF_WRITER_SITE));
    }
    check("GOG: the game global is written after 'Initializing Game...' (00AFB1DC)",
          *(const unsigned char*)(gog + 0x6FB1DC) == 0xA3 &&
          ((*(const DWORD*)(gog + 0x6FB1DD) - (DWORD)(ga(VA_GAME) - g_delta)) & 0xFFFF) == 0);
    all_on();
    build_gate();
    check("GOG: LOD, presets, hotkeys and distance stay on", cfg_enforce && cfg_enforce_down &&
          cfg_preset_mode && cfg_hotkeys && cfg_distance);
    check("GOG: save features off", !cfg_save_thumbs && !cfg_save_trace && !cfg_list_leak &&
          !cfg_fast_list && !cfg_savelist_trace);
    check("GOG: doors and interiors off", !cfg_keep_outdoors && !cfg_keep_interiors &&
          !cfg_preload_interiors && !cfg_skip_door_gc && !cfg_indoor_outdoors && !cfg_fast_doors &&
          !cfg_instant_doors && cfg_door_fade == 0.5f && cfg_area_fade == 0.5f);
    check("GOG: window, window_mode rows, Windows keys stay on", cfg_wf_enabled && cfg_window_mode && cfg_winkeys);
    check("GOG: load_trace off", !cfg_load_trace);
    check("GOG: skip_intro, raycast and quit guards stay on", cfg_skip_intro && cfg_ray_guard && cfg_quit_guard);
    check("GOG: script patches stay on, pause_quit too now that quit_guard is", cfg_book_marker &&
          cfg_continue && cfg_medallion && cfg_pause_quit);
    check("GOG: cpu32 guard, crash capture, keyboard filter, file trace stay on", cfg_cpu32_guard &&
          cfg_crash_dump && cfg_suppress && cfg_file_trace);

    // Unknown: this probe itself
    build_read((uintptr_t)GetModuleHandleA(NULL));
    check("unknown exe: not taken for a game build", g_build == BUILD_UNKNOWN && !ga(VA_GAME));
    all_on();
    build_gate();
    check("unknown exe: script patches and every address feature off", !cfg_book_marker &&
          !cfg_medallion && !cfg_enforce && !cfg_wf_enabled && !cfg_quit_guard);
    check("unknown exe: crash capture and keyboard filter stay on", cfg_crash_dump && cfg_suppress);

    printf("%s: %d failures\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
