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
    [IMP_GETPROCAFFINITY] = 0x00CAD20C, [IMP_EXITPROCESS] = 0x00CAD248,
    [IMP_TERMINATEPROCESS] = 0x00CAD178, [IMP_GETPROCADDRESS] = 0x00CAD250,
    [IMP_D3DX_SAVEFILE] = 0x00CAD4A8,
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
    [IMP_GETPROCAFFINITY] = 0x00D0020C, [IMP_EXITPROCESS] = 0x00D00248,
    [IMP_TERMINATEPROCESS] = 0x00D0017C, [IMP_GETPROCADDRESS] = 0x00D00250,
    [IMP_D3DX_SAVEFILE] = 0x00D004DC,
    [IMP_D3DX_SAVEMEM] = 0x00D004D8, [IMP_STEAM_RS] = 0,   // GOG has no steam_api.dll
};

// Every on/off switch build_gate may touch.
static int* const k_switches[] = {
    &cfg_enforce, &cfg_enforce_down, &cfg_preset_mode, &cfg_hotkeys, &cfg_distance,
    &cfg_save_thumbs, &cfg_save_trace, &cfg_list_leak, &cfg_fast_list, &cfg_savelist_trace,
    &cfg_skip_intro, &cfg_keep_outdoors, &cfg_keep_interiors, &cfg_preload_interiors,
    &cfg_skip_door_gc, &cfg_indoor_outdoors, &cfg_fast_doors, &cfg_instant_doors,
    &cfg_load_trace, &cfg_wf_enabled, &cfg_window_mode, &cfg_winkeys, &cfg_ray_guard,
    &cfg_quit_guard, &cfg_storage_leak, &cfg_clean_quit, &cfg_path_guard, &cfg_cpu32_guard, &cfg_book_marker, &cfg_continue, &cfg_pause_quit,
    &cfg_medallion, &cfg_suppress, &cfg_crash_dump, &cfg_file_trace,
};
#define SWITCHES (sizeof k_switches / sizeof k_switches[0])

static void all_on(void)
{
    unsigned i;
    for (i = 0; i < SWITCHES; i++) *k_switches[i] = 1;
    cfg_door_fade = 0.05f;
    cfg_area_fade = 0.25f;
    cfg_door_light_blend = 1.0f;
    cfg_door_spinner = 0;   // 0 = the spinner hidden on instant doors (the feature on)
    g_gated = 0;
}

// Holds the log lock until told to let go (the exit_logf check).
static HANDLE g_lockHeld, g_lockRelease;
static DWORD WINAPI hold_log_lock(LPVOID param)
{
    (void)param;
    EnterCriticalSection(&g_logLock);
    SetEvent(g_lockHeld);
    WaitForSingleObject(g_lockRelease, 5000);
    LeaveCriticalSection(&g_logLock);
    return 0;
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
          cfg_door_fade == 0.05f && cfg_area_fade == 0.25f && cfg_door_light_blend == 1.0f &&
          !cfg_door_spinner);

    // GOG 3.5.0.26g
    build_read(gog);
    printf("GOG mapped at %08X, delta %08X\n", (unsigned)gog, (unsigned)g_delta);
    check("GOG: detected as GOG 3.5.0.26g", g_build == BUILD_GOG);
    check("GOG: import slots found by name (no SteamRemoteStorage)", slots_match(gog, k_gogSlots));
    check("GOG: unported addresses read as 0", !ga(VA_IMAGES_ADD) && !ga(VA_STEAM_STREAM) &&
          !ga_have(VA_PROV_OPEN, 0));
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
    // Doors and interiors (phase 1c). Calls inside .text are not relocated; absolutes and slots
    // are compared by their low 16 bits, as above.
    {
        uintptr_t a = ga(VA_PART_SWITCH_A), b = ga(VA_PART_SWITCH_B), r = ga(VA_PART_RUN_CALL);
        uintptr_t gc = ga(VA_PART_GC_CALL), u = ga(VA_UNPACK_CALL);
        check("GOG: switch, run, request and GC bytes (the GOG conventions)",
              code_matches(ga(VA_PART_SWITCH), "55 8b ec 83 ec 50 53 56 57 ff 75 08") &&
              code_matches(ga(VA_PART_RUN), pt_run_bytes()) && code_matches(ga(VA_PART_REQUEST), pt_request_bytes()) &&
              code_matches(ga(VA_GC), pt_gc_bytes()));
        check("GOG: Steam's run and request bytes do not match there (EDI job, thiscall request)",
              !code_matches(ga(VA_PART_RUN), "55 8b ec 83 ec 18 53 8b 5d 08 56 57") &&
              !code_matches(ga(VA_PART_REQUEST), "55 8b ec 53 57 8b f8 8b 47 40 33 db"));
        check("GOG: both switch calls, the sync run (after mov edi,esi) and the GC call",
              *(const unsigned char*)a == 0xE8 && a + 5 + *(const LONG*)(a + 1) == ga(VA_PART_SWITCH) &&
              *(const unsigned char*)b == 0xE8 && b + 5 + *(const LONG*)(b + 1) == ga(VA_PART_SWITCH) &&
              code_matches(r - 2, "8b fe e8") && r + 5 + *(const LONG*)(r + 1) == ga(VA_PART_RUN) &&
              *(const unsigned char*)gc == 0xE8 && gc + 5 + *(const LONG*)(gc + 1) == ga(VA_GC));
        check("GOG: the door timer and start slots hold them (low 16 bits)",
              ((*(const DWORD*)ga(VA_DOOR_TIMER_SLOT) - (DWORD)ga(VA_DOOR_TIMER)) & 0xFFFF) == 0 &&
              ((*(const DWORD*)ga(VA_DOOR_START_SLOT) - (DWORD)ga(VA_DOOR_START)) & 0xFFFF) == 0 &&
              code_matches(ga(VA_DOOR_TIMER), "55 8b ec f6 05") &&
              code_matches(ga(VA_DOOR_START), "55 8b ec 83 ec 10 53 56 8b 75 08 8b 06 33 db 57"));
        check("GOG: BlackScreen slot 2 is the fade_out handler; the fade commands' execute slots",
              ((*(const DWORD*)(ga(VA_DOOR_TELEPORT_VT) + 8) - (DWORD)ga(VA_DOOR_FADE_EVENT)) & 0xFFFF) == 0 &&
              ((*(const DWORD*)ga(VA_FADE_OUT_EXEC_SLOT) - (DWORD)ga(VA_FADE_OUT_EXEC)) & 0xFFFF) == 0 &&
              ((*(const DWORD*)ga(VA_FADE_IN_EXEC_SLOT) - (DWORD)ga(VA_FADE_IN_EXEC)) & 0xFFFF) == 0);
        check("GOG: CName lookup is the thiscall one; L\"fade_out\" is at VA_STR_FADE_OUT",
              code_matches(ga(VA_CNAME_FROM_STR), "55 8b ec 56 6a 00 ff 75 08 8b f1 e8") &&
              !wcscmp((const wchar_t*)ga(VA_STR_FADE_OUT), L"fade_out"));
        {
            const unsigned char* fo = (const unsigned char*)ga(VA_DOOR_FADE_OUT);
            const unsigned char* fi = (const unsigned char*)ga(VA_DOOR_FADE_IN);
            const unsigned char* op;
            unsigned n = door_fade_op(&op);
            DWORD len = (DWORD)ga(VA_DOOR_FADE_LEN);
            check("GOG: both door fades load the shared 0.5 with fld (D9 05, disp32 at +2)",
                  n == 2 && !memcmp(fo, op, n) && !memcmp(fi, op, n) &&
                  ((*(const DWORD*)(fo + n) - len) & 0xFFFF) == 0 && ((*(const DWORD*)(fi + n) - len) & 0xFFFF) == 0 &&
                  *(const float*)ga(VA_DOOR_FADE_LEN) == 0.5f);
        }
        check("GOG: the indoor draw check and its immediate",
              code_matches(ga(VA_INDOOR_CMP), "80 b8 ac 00 00 00 00 74 0d 83 b8 a8 00 00 00 01 75 04") &&
              ga(VA_INDOOR_IMM) == ga(VA_INDOOR_CMP) + 15 && *(const unsigned char*)ga(VA_INDOOR_IMM) == 0x01);
        check("GOG: the block unpack and its only call",
              code_matches(ga(VA_UNPACK), "55 8b ec 8b 4d 08 0f b6 01 c1 e8 05") &&
              *(const unsigned char*)u == 0xE8 && u + 5 + *(const LONG*)(u + 1) == ga(VA_UNPACK));
    }
    // Saves (phase 2): the list builder (thiscall) and its two calls; the capture flags are
    // relocated absolutes in the save writer (low 16 bits).
    {
        uintptr_t a = ga(VA_SAVELIST_CALL_A), b = ga(VA_SAVELIST_CALL_B);
        const unsigned char* clr = (const unsigned char*)(gog + 0xE7480);   // 004E7480: mov [pending],bl
        check("GOG: the save list builder (thiscall) and both calls",
              code_matches(ga(VA_SAVELIST), "55 8b ec 81 ec 00 02 00 00 53 56 33 db 8b f1") &&
              *(const unsigned char*)a == 0xE8 && a + 5 + *(const LONG*)(a + 1) == ga(VA_SAVELIST) &&
              *(const unsigned char*)b == 0xE8 && b + 5 + *(const LONG*)(b + 1) == ga(VA_SAVELIST) &&
              code_matches(a - 2, "8b cf e8"));
        {
            uintptr_t call = ga(VA_DZIP_READER_CALL);
            const DWORD* vt = (const DWORD*)ga(VA_DZIP_READER_VT);
            const DWORD* mem = (const DWORD*)ga(VA_MEM_STREAM);
            // Slot values are relocated to the file's shared base: compare their low 16 bits and
            // read the code at the k_gog address of the function they name.
            check("GOG: the save reader ctor, its one call and its forwarding slots (fast_list)",
                  code_matches(ga(VA_DZIP_READER), "55 8b ec 51 83 66 0c 00 53 57 ff 75 08 8d 4e 10") &&
                  code_matches(call - 1, "50 e8") && call + 5 + *(const LONG*)(call + 1) == ga(VA_DZIP_READER) &&
                  ((vt[1] - (DWORD)(gog + 0x6A1D96)) & 0xFFFF) == 0 && ((vt[3] - (DWORD)(gog + 0x6A1D86)) & 0xFFFF) == 0 &&
                  code_matches(gog + 0x6A1D96, "55 8b ec 8b 49 30 8b 01 5d ff 60 04") &&
                  code_matches(gog + 0x6A1D86, "8b 49 30 8b 01 ff 60 0c") &&
                  ((mem[1] - (DWORD)ga(VA_MEM_SERIALIZE)) & 0xFFFF) == 0 && ((mem[3] - (DWORD)ga(VA_MEM_SIZE)) & 0xFFFF) == 0 &&
                  code_matches(ga(VA_MEM_SERIALIZE), "55 8b ec 56 8b f1 8b 4e 14 8b 46 10") &&
                  code_matches(ga(VA_MEM_SIZE), "8b 41 10 8b 40 04 c3"));
        }
        {
            uintptr_t lc = ga(VA_LAYER_LOAD_CALL);
            check("GOG: the per-layer load (stdcall info, ctx; RET 8) and its one call (memory guard)",
                  code_matches(lc - 14, "8d 45 d4 50 ff 75 ec 89 5d d4 c6 45 d8 01 e8 f3") &&
                  lc + 5 + *(const LONG*)(lc + 1) == ga(VA_LAYER_LOAD) &&
                  code_matches(ga(VA_LAYER_LOAD), "55 8b ec 83 ec 0c 53 8b 5d 08 56 57 33 ff c6 43") &&
                  code_matches(ga(VA_LAYER_LOAD) + 0x85, "c2 08 00"));
        }
        check("GOG: the loading spinner command's execute slot (door_spinner)",
              ((*(const DWORD*)ga(VA_SPINNER_EXEC_SLOT) - (DWORD)ga(VA_SPINNER_EXEC)) & 0xFFFF) == 0 &&
              code_matches(ga(VA_SPINNER_EXEC), "0f b6 41 08 50 e8 65 54 ff ff c3"));
        {
            uintptr_t add = ga(VA_ENV_ADD_CALL), stab = ga(VA_ENV_STAB_CALL);
            check("GOG: the area environment add (call) and the door's Stabilize (jmp) (door_light_blend)",
                  code_matches(add - 9, "51 8d 8e c0 01 00 00 51 50 e8 8f ff 03 00") &&
                  add + 5 + *(const LONG*)(add + 1) == ga(VA_ENV_ADD) &&
                  code_matches(stab - 9, "8b 40 40 8b 88 b8 02 00 00 e9 8f 97 51 00") &&
                  stab + 5 + *(const LONG*)(stab + 1) == ga(VA_ENV_STAB) &&
                  code_matches(ga(VA_ENV_STAB), "0f 57 c0 8b c1") && code_matches(ga(VA_ENV_STAB) + 0x56, "c3") &&
                  code_matches(ga(VA_ENV_ADD) + 0x256, "c2 24 00"));
        }
        check("GOG: the save writer clears both capture flags (low 16 bits)",
              clr[0] == 0x88 && clr[1] == 0x1D && clr[6] == 0x88 && clr[7] == 0x1D &&
              ((*(const DWORD*)(clr + 2) - (DWORD)(ga(VA_CAPTURE_PENDING) - g_delta)) & 0xFFFF) == 0 &&
              ((*(const DWORD*)(clr + 8) - (DWORD)(ga(VA_CAPTURE_QUEUED) - g_delta)) & 0xFFFF) == 0);
    }
    check("GOG: the game global is written after 'Initializing Game...' (00AFB1DC)",
          *(const unsigned char*)(gog + 0x6FB1DC) == 0xA3 &&
          ((*(const DWORD*)(gog + 0x6FB1DD) - (DWORD)(ga(VA_GAME) - g_delta)) & 0xFFFF) == 0);
    all_on();
    build_gate();
    check("GOG: LOD, presets, hotkeys and distance stay on", cfg_enforce && cfg_enforce_down &&
          cfg_preset_mode && cfg_hotkeys && cfg_distance);
    check("GOG: preview fix, fast_list, save_trace and savelist_trace on; fix_list_leak not needed",
          cfg_save_thumbs && cfg_fast_list && cfg_save_trace && cfg_savelist_trace && !cfg_list_leak);
    check("GOG: doors and interiors stay on", cfg_keep_outdoors && cfg_keep_interiors &&
          cfg_preload_interiors && cfg_skip_door_gc && cfg_indoor_outdoors && cfg_fast_doors &&
          cfg_instant_doors && cfg_door_fade == 0.05f && cfg_area_fade == 0.25f);
    check("GOG: door_light_blend and the hidden spinner on",
          cfg_door_light_blend == 1.0f && cfg_door_spinner == 0);
    check("GOG: window, window_mode rows, Windows keys stay on", cfg_wf_enabled && cfg_window_mode && cfg_winkeys);
    check("GOG: load_trace stays on", cfg_load_trace);
    check("GOG: skip_intro, raycast and quit guards stay on", cfg_skip_intro && cfg_ray_guard && cfg_quit_guard);
    check("GOG: layer storage leak fix stays on", cfg_storage_leak);
    check("GOG: clean quit stays on", cfg_clean_quit);
    check("GOG: path guard stays on", cfg_path_guard);
    check("GOG: script patches stay on, pause_quit too now that quit_guard is", cfg_book_marker &&
          cfg_continue && cfg_medallion && cfg_pause_quit);
    check("GOG: cpu32 guard, crash capture, keyboard filter, file trace stay on", cfg_cpu32_guard &&
          cfg_crash_dump && cfg_suppress && cfg_file_trace);

    // The exit wrapper's GetProcAddress hook (naked): other lookups pass through unchanged,
    // CorExitProcess comes back as the DLL's wrapper around the real export.
    {
        typedef FARPROC (WINAPI *Gpa_t)(HMODULE, LPCSTR);
        Gpa_t hook = (Gpa_t)(void*)hook_GetProcAddress;
        HMODULE k32 = GetModuleHandleA("kernel32.dll"), cor = LoadLibraryA("mscoree.dll");
        FARPROC realCor = cor ? GetProcAddress(cor, "CorExitProcess") : NULL;
        g_realGetProcAddress = GetProcAddress;
        check("exit: GetProcAddress hook passes names and ordinals through",
              hook(k32, "GetTickCount") == GetProcAddress(k32, "GetTickCount") &&
              hook(k32, (LPCSTR)(uintptr_t)1) == GetProcAddress(k32, (LPCSTR)(uintptr_t)1) &&
              !hook(k32, "NoSuchExport"));
        check("exit: CorExitProcess is handed out as the wrapper, the real one kept",
              realCor && hook(cor, "CorExitProcess") == (FARPROC)(void*)hook_CorExitProcess &&
              g_realCorExitProcess == (CorExitProcess_t)(void*)realCor && !hook(k32, "CorExitProcess"));
    }

    // exit_logf: with the log lock held by another thread it gives up after 250 ms and writes
    // the line straight to the file handle.
    {
        HANDLE th;
        DWORD t0, dt;
        g_lockHeld = CreateEventA(NULL, TRUE, FALSE, NULL);
        g_lockRelease = CreateEventA(NULL, TRUE, FALSE, NULL);
        th = CreateThread(NULL, 0, hold_log_lock, NULL, 0, NULL);
        WaitForSingleObject(g_lockHeld, 5000);
        t0 = GetTickCount();
        exit_logf("exit: probe line written raw past a held log lock");
        dt = GetTickCount() - t0;
        SetEvent(g_lockRelease);
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
        check("exit: exit_logf waits at most ~250 ms for a held log lock", dt >= 240 && dt < 1000);
    }

    // GOG previews: a .sav opened for writing with no preview of its own gets the late capture
    // next to it; one whose preview the game wrote itself gets nothing.
    {
        wchar_t dir[MAX_PATH], sav[MAX_PATH], bmp[MAX_PATH], sav2[MAX_PATH], bmp2[MAX_PATH];
        static const unsigned char fake[54] = { 'B', 'M' };
        unsigned char got[64];
        DWORD n = 0;
        HANDLE f;
        int wrote, skipped;
        g_build = BUILD_GOG;
        cfg_save_thumbs = 1;
        InitializeCriticalSection(&g_thumbLock);
        GetTempPathW(MAX_PATH, dir);
        swprintf_s(sav, MAX_PATH, L"%sw2ef_probe_%lu_ManualSave_0001.sav", dir, GetCurrentProcessId());
        thumb_name_w(sav, bmp, MAX_PATH);
        DeleteFileW(bmp);
        thumb_note_open_w(sav);
        thumb_capture(fake, sizeof fake);
        thumb_tick_local();
        f = CreateFileW(bmp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        wrote = f != INVALID_HANDLE_VALUE && ReadFile(f, got, sizeof got, &n, NULL) && n == sizeof fake &&
                !memcmp(got, fake, sizeof fake);
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileW(bmp);
        swprintf_s(sav2, MAX_PATH, L"%sw2ef_probe_%lu_QuickSave.sav", dir, GetCurrentProcessId());
        thumb_name_w(sav2, bmp2, MAX_PATH);
        DeleteFileW(bmp2);
        thumb_note_open_w(bmp2);   // the game wrote its own preview first
        thumb_note_open_w(sav2);
        thumb_capture(fake, sizeof fake);
        thumb_tick_local();
        skipped = GetFileAttributesW(bmp2) == INVALID_FILE_ATTRIBUTES && !g_saveNameW[0];
        check("thumbnail (GOG): the late capture is written next to the .sav", wrote);
        check("thumbnail (GOG): nothing is written when the game wrote its own preview", skipped);
        // A second QuickSave right after, this time without a preview of its own: the first one's
        // preview must not pair with it again.
        thumb_note_open_w(sav2);
        thumb_capture(fake, sizeof fake);
        thumb_tick_local();
        check("thumbnail (GOG): a second QuickSave gets its own late preview",
              GetFileAttributesW(bmp2) != INVALID_FILE_ATTRIBUTES);
        DeleteFileW(bmp2);
    }

    // Unknown: this probe itself
    build_read((uintptr_t)GetModuleHandleA(NULL));
    check("unknown exe: not taken for a game build", g_build == BUILD_UNKNOWN && !ga(VA_GAME));
    all_on();
    build_gate();
    check("unknown exe: script patches and every address feature off", !cfg_book_marker &&
          !cfg_medallion && !cfg_enforce && !cfg_wf_enabled && !cfg_quit_guard && !cfg_storage_leak &&
          !cfg_clean_quit && !cfg_path_guard);
    check("unknown exe: crash capture and keyboard filter stay on", cfg_crash_dump && cfg_suppress);

    printf("%s: %d failures\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
