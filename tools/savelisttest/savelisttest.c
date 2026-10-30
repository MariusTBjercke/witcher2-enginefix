// Save-list trace and leak-fix test: tools/savelisttest/build-and-run.bat
// Includes the DLL source, maps a fake list builder and both call sites at their real
// addresses (009A49B9, 009A5175, 009A524F), redirects the calls and runs them. Fake
// registry Find (004A4A03), ImageKey (00B30E9F) and stream destructor (0052AFAA, vtable
// 00F02C2C) check when the leaked preview stream is freed.
#include "../../src/enginefix.c"

static int fails;
static void expect(const char* what, int ok) { printf("%-52s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) fails++; }

static void put(uintptr_t at, const char* hex)
{
    for (const char* p = hex; *p; ) {
        unsigned v;
        sscanf_s(p, "%2x", &v);
        *(unsigned char*)at++ = (unsigned char)v;
        p += 2;
        while (*p == ' ') p++;
    }
}

static void put_call_site(uintptr_t call)
{
    // push dword [esp+4]; call 009A49B9; ret 4  - a stdcall(panel) thunk around the site
    put(call - 4, "ff 74 24 04");
    *(unsigned char*)call = 0xE8;
    *(LONG*)(call + 1) = (LONG)(VA_SAVELIST - (call + 5));
    put(call + 5, "c2 04 00");
}

// The exe is linked at 0x00400000 with a large zero-filled arena, so every game
// address used below (004A4A03 .. 00F02C2C) lies inside our own image: no other
// mapping can take them, unlike a VirtualAlloc at run time.
static unsigned char g_arena[0xD00000];

static int map_game_range(void)
{
    DWORD old;
    uintptr_t lo = 0x004A0000, hi = 0x00F10000;
    if ((uintptr_t)g_arena > lo || (uintptr_t)g_arena + sizeof g_arena < hi) {
        printf("arena %p+%X does not cover %08X..%08X\n", (void*)g_arena, (unsigned)sizeof g_arena,
               (unsigned)lo, (unsigned)hi);
        return 0;
    }
    return VirtualProtect((void*)lo, hi - lo, PAGE_EXECUTE_READWRITE, &old) != 0;
}

static int add_calls;
static void __fastcall fake_add(void* self, void* edx, void* name, void* stream)
{
    (void)self; (void)edx; (void)name; (void)stream;
    add_calls++;
}
typedef struct fake_name { const wchar_t* text; unsigned key; } fake_name;
typedef struct fake_stream { void* vtbl; int destroyed; char pad[0x18]; void* inner; } fake_stream;

int main(void)
{
    typedef void (__stdcall *thunk_t)(void*);
    InitializeCriticalSection(&g_thumbLock);
    g_log = stdout; cfg_log = 1; cfg_savelist_trace = 1;
    if (!map_game_range()) return 2;

    // Find: real prologue, then out = map (EAX), found = (*key == 0x77), ret 8
    put(VA_MAP_FIND, "55 8b ec 83 ec 0c 56 ff 75 08 8b f0 59 5e 8b 55 0c 89 02"
                     "81 39 77 00 00 00 0f 94 c0 c9 c2 08 00");
    // ImageKey(name): key = name->key, ret 4
    put(VA_IMAGE_KEY, "8b 44 24 04 8b 40 04 c2 04 00");
    // stream destructor: real prologue, then this->destroyed++, ret 4
    put(VA_STREAM_DTOR, "55 8b ec 56 8b f1 8b 4e 20 c7 06 2c 2c f0 00 ff 46 04 5e 5d c2 04 00");
    *(void**)(uintptr_t)VA_STEAM_STREAM = (void*)(uintptr_t)VA_STREAM_DTOR;
    g_origAddImage = fake_add; cfg_list_leak = 1;

    // builder: real prologue, then mov esp,ebp; pop ebp; ret 4
    put(VA_SAVELIST, "55 8b ec 83 e4 f8 81 ec ec 01 00 00 8b e5 5d c2 04 00");
    put_call_site(VA_SAVELIST_CALL_A);
    put_call_site(VA_SAVELIST_CALL_B);

    savelist_install();
    expect("leak fix recognised its targets", g_leakFixOn == 1);
    expect("startup call redirected", *(LONG*)(VA_SAVELIST_CALL_A + 1) == (LONG)((uintptr_t)savelist_startup - (VA_SAVELIST_CALL_A + 5)));
    expect("menu call redirected", *(LONG*)(VA_SAVELIST_CALL_B + 1) == (LONG)((uintptr_t)savelist_menu - (VA_SAVELIST_CALL_B + 5)));
    expect("second install is a no-op", patch_call(VA_SAVELIST_CALL_A, (void*)savelist_startup));
    expect("foreign call target refused", !patch_call(VA_SAVELIST_CALL_B, (void*)savelist_startup));

    // stdcall stack balance: run both thunks many times; a wrong cleanup crashes or drifts ESP
    for (int i = 0; i < 1000; i++) {
        ((thunk_t)(VA_SAVELIST_CALL_A - 4))((void*)0x1234);
        if (i == 0) g_log = NULL;   // keep the output short after the first build
    }
    g_log = stdout;
    ((thunk_t)(VA_SAVELIST_CALL_B - 4))((void*)0x1234);
    expect("builder ran through both sites, depth back to 0", g_listDepth == 0);

    g_listDepth = 1; g_seenCount = 0; g_listImages = g_listNewImages = 0;
    savelist_note_image(L"ManualSave_0011_640x360.bmp");
    savelist_note_image(L"manualsave_0011_640X360.BMP");
    savelist_note_image(L"QuickSave_640x360.bmp");
    g_listDepth = 0;
    expect("previews counted, repeats not new (case-folded)", g_listImages == 3 && g_listNewImages == 2 && g_seenCount == 2);

    {
        unsigned char self[16] = { 0 };
        fake_name held = { L"ManualSave_0011_640x360.bmp", 0x77 }, fresh = { L"New_640x360.bmp", 0x10 };
        fake_stream st = { (void*)(uintptr_t)VA_STEAM_STREAM, 0 }, other = { (void*)0x1111, 0 };
        g_listDepth = 0; add_calls = 0;
        hook_AddImage(self, NULL, &held, &st);
        expect("outside the list builder: untouched", add_calls == 1 && st.destroyed == 0);
        g_listDepth = 1; add_calls = 0;
        hook_AddImage(self, NULL, &held, &st);
        expect("held name in list: stream freed, original skipped", add_calls == 0 && st.destroyed == 1 && g_listFreed == 1);
        expect("lookup: key reaches Find, held vs new", registry_holds(self, &held) && !registry_holds(self, &fresh));
        st.vtbl = (void*)(uintptr_t)VA_STEAM_STREAM; st.destroyed = 0; add_calls = 0;
        hook_AddImage(self, NULL, &fresh, &st);
        expect("new name in list: original takes the stream", add_calls == 1 && st.destroyed == 0);
        add_calls = 0;
        hook_AddImage(self, NULL, &held, &other);
        expect("held name, unknown stream type: original", add_calls == 1 && other.destroyed == 0);
        add_calls = 0;
        hook_AddImage(self, NULL, &held, NULL);
        expect("held name, no stream: original", add_calls == 1);
        g_listDepth = 0;
    }

    mem_view m;
    mem_snapshot(&m);
    expect("memory walk sees committed and free space", m.commit_mb > 0 && m.free_mb > 0 && m.largest_free_mb > 0 && m.largest_free_mb <= m.free_mb);
    log_mem("test");

    printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
