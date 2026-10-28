// Pairing-logic test for the save thumbnail fix: tools/thumbtest/build-and-run.bat
// Includes the DLL source directly and replaces the Steam write with a recorder.
#include "../../src/enginefix.c"

static char  w_name[MAX_PATH]; static int w_size, w_calls;
static unsigned char __fastcall fake_write(void* self, void* edx, const char* name, const void* data, int size)
{
    (void)self; (void)edx; (void)data;
    lstrcpynA(w_name, name, sizeof w_name); w_size = size; w_calls++;
    return 1;
}
static int fails;
static void expect(const char* what, int ok) { printf("%-52s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) fails++; }
static unsigned char img[921656];

int main(void)
{
    InitializeCriticalSection(&g_thumbLock);
    g_log = stdout; cfg_log = 1; cfg_save_thumbs = 1;
    g_remoteStorage = (void*)0x1234; g_origFileWrite = fake_write;

    w_calls = 0; thumb_note_write("ManualSave_0011.sav"); thumb_tick();
    expect("race: nothing written before capture", w_calls == 0);
    thumb_capture(img, sizeof img); thumb_tick();
    expect("race: late BMP written", w_calls == 1 && !strcmp(w_name, "ManualSave_0011_640x360.bmp") && w_size == (int)sizeof img);
    thumb_tick();
    expect("race: written once", w_calls == 1);

    w_calls = 0; thumb_capture(img, sizeof img);
    thumb_note_write("ManualSave_0012_640x360.bmp"); thumb_note_write("ManualSave_0012.sav"); thumb_tick();
    expect("game wrote BMP itself: nothing added", w_calls == 0 && !g_saveName[0] && !g_thumbData);

    w_calls = 0; thumb_note_write("QuickSave.sav"); thumb_capture(img, 1000); thumb_tick();
    expect("quicksave name", w_calls == 1 && !strcmp(w_name, "QuickSave_640x360.bmp") && w_size == 1000);

    w_calls = 0; thumb_capture(img, sizeof img); g_thumbTick -= 5000;
    thumb_note_write("ManualSave_0013.sav"); thumb_tick();
    expect("stale capture (5 s before save) rejected", w_calls == 0 && g_saveName[0] && !g_thumbData);
    g_saveTick -= THUMB_WINDOW + 1; thumb_tick();   // time passes, no capture
    expect("save without capture times out", w_calls == 0 && !g_saveName[0]);

    w_calls = 0; thumb_note_write("ManualSave_0014.sav"); thumb_capture(img, THUMB_MAX + 1); thumb_tick();
    expect("oversized capture ignored", w_calls == 0);

    w_calls = 0; thumb_note_write("ManualSave_0015_640x360.bmp"); thumb_note_write("ManualSave_0016.sav");
    thumb_capture(img, sizeof img); thumb_tick();
    expect("BMP of another save does not satisfy .sav", w_calls == 1 && !strcmp(w_name, "ManualSave_0016_640x360.bmp"));

    // stale marks for the menu registry: our late writes and the game's own BMP writes
    memset(g_stale, 0, sizeof g_stale);
    thumb_mark_stale("QuickSave_640x360.bmp"); thumb_mark_stale("QuickSave_640x360.bmp");
    { int n = 0; for (int i = 0; i < STALE_MAX; i++) n += g_stale[i][0] != 0;
      expect("stale mark deduplicated", n == 1); }
    thumb_note_write("AutoSave_0001_640x360.bmp");
    { int found = 0; for (int i = 0; i < STALE_MAX; i++) found |= !wcscmp(g_stale[i], L"AutoSave_0001_640x360.bmp");
      expect("game's own BMP write marked stale", found); }
    memset(g_stale, 0, sizeof g_stale);
    thumb_note_write("QuickSave.sav"); thumb_capture(img, sizeof img); thumb_tick();
    { int found = 0; for (int i = 0; i < STALE_MAX; i++) found |= !wcscmp(g_stale[i], L"QuickSave_640x360.bmp");
      expect("late QuickSave write marked stale", found); }
    for (int i = 0; i < STALE_MAX + 3; i++) { char b[64]; sprintf_s(b, sizeof b, "ManualSave_%04d_640x360.bmp", i); thumb_mark_stale(b); }
    { int found = 0; for (int i = 0; i < STALE_MAX; i++) found |= !wcscmp(g_stale[i], L"ManualSave_0034_640x360.bmp");
      expect("ring keeps the newest mark when full", found); }

    printf("%s\n", fails ? "FAILED" : "ALL PASS");
    return fails;
}
