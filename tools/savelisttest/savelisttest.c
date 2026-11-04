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

// The exe is linked at 0x00010000 with a large zero-filled arena, so every game
// address used below (004A4A03 .. 00F02C2C) lies inside our own image: no other
// mapping can take them, unlike a VirtualAlloc at run time. (At 0x00400000 the DLL's own
// code grew past 004A0000 and the arena no longer reached down to the game range.)
static unsigned char g_arena[0xF00000];

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

// fast_list: a fake mode-0 provider stream over `dz` (a whole .sav), attached lazily. Reads
// what 009B76FD reads (0x480 header, the four sections it seeks to, the end magic), then
// every byte, and compares with a full unpack. Returns 1 when all of it matches.
static int lazy_check(const sp_buf* dz, const char* label, int verbose)
{
    void* inner[8] = { (void*)(uintptr_t)VA_MEM_STREAM };
    void* outer[16] = { (void*)(uintptr_t)VA_STEAM_STREAM };
    sp_dzip z;
    sp_buf full = { 0 };
    const unsigned char* r;
    const char* keys[4] = { "timeInfo", "saveInfo", "worldInfo", "timeManger" };
    unsigned char tmp[0x480];
    LONG header_blocks;
    int ok = 1;
    if (!sp_dzip_open(dz, &z) || !sp_dzip_extract(&z, 0, &full)) { printf("%s: not a DZIP save\n", label); return 0; }
    sp_dzip_close(&z);
    outer[0x10 / 4] = dz->p; outer[0x14 / 4] = (void*)(uintptr_t)dz->n; outer[0x20 / 4] = inner;
    inner[0x10 / 4] = &outer[0x10 / 4];
    if (!lazy_attach(outer) || inner[0x10 / 4] != (void*)g_lazy.arr || (size_t)g_lazy.arr[1] != full.n) {
        printf("%s: attach failed\n", label); sp_free(&full); lazy_release(); return 0;
    }
    r = (const unsigned char*)g_lazy.arr[0];
    memcpy(tmp, r, sizeof tmp);
    ok &= !memcmp(tmp, full.p, sizeof tmp);
    ok &= !memcmp(r + full.n - 4, full.p + full.n - 4, 4);
    // SAVY directory: 32-byte name + u32 offset per entry from +12
    for (int k = 0; k < 4; k++)
        for (size_t at = 12; at + 36 <= sizeof tmp; at += 36)
            if (!strcmp((const char*)tmp + at, keys[k])) {
                unsigned off = *(const unsigned*)(tmp + at + 32);
                if (off + 256 <= full.n) ok &= !memcmp(r + off, full.p + off, 256);
            }
    header_blocks = g_lazy.blocks;
    ok &= !memcmp(r, full.p, full.n) && g_lazy.bad == 0;
    if (verbose || !ok)
        printf("%-26s %8zu bytes, header reads unpacked %ld of %ld blocks, all bytes %s\n", label, full.n,
               header_blocks, (long)(g_lazy.reserved / LAZY_BLOCK), ok ? "equal" : "DIFFER");
    lazy_release();
    sp_free(&full);
    return ok;
}

static int lazy_saves(const char* dir)
{
    char pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, bad = 0;
    snprintf(pat, sizeof pat, "%s\\*.sav", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no saves in %s\n", dir); return -1; }
    do {
        FILE* f;
        sp_buf b = { 0 };
        snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName);
        if (fopen_s(&f, path, "rb") || !f) continue;
        b.n = fd.nFileSizeLow;
        b.p = (unsigned char*)malloc(b.n);
        if (b.p && fread(b.p, 1, b.n, f) == b.n) { if (!lazy_check(&b, fd.cFileName, n < 3)) bad++; n++; }
        fclose(f);
        free(b.p);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    printf("%d real saves checked, %d differ\n", n, bad);
    return bad;
}

int main(int argc, char** argv)
{
    typedef void (__stdcall *thunk_t)(void*);
    setvbuf(stdout, NULL, _IONBF, 0);   // a crash must not swallow the lines before it
    InitializeCriticalSection(&g_thumbLock);
    InitializeCriticalSection(&g_logLock);
    g_log = stdout; cfg_log = 1; cfg_savelist_trace = 1;
    g_build = BUILD_STEAM; g_base = IMAGE_BASE; g_delta = 0;   // the Steam addresses, as in DllMain
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
    expect("second install is a no-op", patch_call(VA_SAVELIST_CALL_A, VA_SAVELIST, (void*)savelist_startup));
    expect("foreign call target refused", !patch_call(VA_SAVELIST_CALL_B, VA_SAVELIST, (void*)savelist_startup));

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

    {
        // Synthetic save: SAVY header naming the four sections, 300 KB of mixed data.
        static unsigned char savy[300000];
        const char* names[1] = { "C:\\x\\Test.sav.tmp" };
        unsigned char empty[32] = { 'D', 'Z', 'I', 'P', 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0,
                                    0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };
        sp_buf e = { empty, sizeof empty }, data = { savy, sizeof savy }, out = { 0 }, junk = { savy, 4096 };
        sp_dzip z;
        void* inner[8] = { (void*)(uintptr_t)VA_MEM_STREAM };
        void* outer[16] = { (void*)0x1111 };
        unsigned offs[4] = { 0x48C, 0x4CD, 150000, 0x579 };
        const char* keys[4] = { "timeInfo", "saveInfo", "worldInfo", "timeManger" };
        memcpy(savy, "SAVY\2\0\0\0\1\0\0\0", 12);
        for (int k = 0; k < 4; k++) { strcpy_s((char*)savy + 12 + 36 * k, 32, keys[k]); memcpy(savy + 44 + 36 * k, &offs[k], 4); }
        for (size_t i = 0x480; i < sizeof savy; i++) savy[i] = (unsigned char)((i / 7) ^ (i % 251 < 40 ? i : 0));
        memcpy(savy + sizeof savy - 4, "KCUP", 4);
        expect("lazy: fault handler installed", AddVectoredExceptionHandler(1, lazy_fault) != NULL);
        expect("lazy: synthetic DZIP written", sp_dzip_open(&e, &z) && sp_dzip_write(&z, 1, names, &data, 0, &out));
        sp_dzip_close(&z);
        expect("lazy: synthetic save reads equal, few blocks", lazy_check(&out, "synthetic", 1));
        expect("lazy: foreign stream type refused", !lazy_attach(outer) && !g_lazy.region);
        outer[0] = (void*)(uintptr_t)VA_STEAM_STREAM;
        outer[0x10 / 4] = junk.p; outer[0x14 / 4] = (void*)(uintptr_t)junk.n; outer[0x20 / 4] = inner;
        inner[0x10 / 4] = &outer[0x10 / 4];
        expect("lazy: non-DZIP data refused, stream untouched", !lazy_attach(outer) && inner[0x10 / 4] == &outer[0x10 / 4]);
        sp_free(&out);
        if (argc > 1) expect("lazy: every real save equal to a full unpack", lazy_saves(argv[1]) == 0);
    }

    mem_view m;
    mem_snapshot(&m);
    expect("memory walk sees committed and free space", m.commit_mb > 0 && m.free_mb > 0 && m.largest_free_mb > 0 && m.largest_free_mb <= m.free_mb);
    log_mem("test");

    printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
