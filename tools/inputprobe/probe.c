// Input probe: exercises the proxy DLL's DirectInput hook exactly the way the game
// does, and verifies that F9/F10 are filtered out of the keyboard state only while
// Ctrl or Alt is held.
//
// Build: cl /nologo /O2 /MT /guard:cf probe.c dinput8.lib /Fe:probe.exe
// Run with the working directory set to the game's bin folder (so "dinput8.dll"
// resolves to the proxy) and suppress_mod_keys=1 in witcher2_enginefix.ini.
//
// /guard:cf is deliberate: the game's indirect calls are CFG-checked, so a hook that
// is not in the CFG bitmap kills the process instantly and silently. Building the
// probe the same way reproduces that.

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>

typedef HRESULT (WINAPI *PFN_DIC)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

#define DIK_F9_CODE  0x43
#define DIK_F10_CODE 0x44
#define DOWN 0
#define UP   KEYEVENTF_KEYUP

static int fails;

static void check(const char* what, int ok, int got)
{
    printf("  %-46s %s (0x%02X)\n", what, ok ? "PASS" : "FAIL", got & 0xFF);
    if (!ok) fails++;
}

int main(void)
{
    HMODULE h = LoadLibraryA("dinput8.dll");
    if (!h) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    PFN_DIC create = (PFN_DIC)GetProcAddress(h, "DirectInput8Create");
    if (!create) { printf("no DirectInput8Create export\n"); return 1; }

    IDirectInput8A* di = NULL;
    HRESULT hr = create(GetModuleHandle(NULL), DIRECTINPUT_VERSION, &IID_IDirectInput8A, (void**)&di, NULL);
    printf("DirectInput8Create 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !di) return 1;

    IDirectInputDevice8A* kb = NULL;
    hr = di->lpVtbl->CreateDevice(di, &GUID_SysKeyboard, (LPDIRECTINPUTDEVICE8A*)&kb, NULL);
    printf("CreateDevice        0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr) || !kb) return 1;

    hr = kb->lpVtbl->SetDataFormat(kb, &c_dfDIKeyboard);
    hr = kb->lpVtbl->SetCooperativeLevel(kb, GetConsoleWindow(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    hr = kb->lpVtbl->Acquire(kb);
    printf("format/coop/acquire done\n\n");

    // DirectInput only reports keys for the focused window, so take focus first.
    ShowWindow(GetConsoleWindow(), SW_SHOW);
    SetForegroundWindow(GetConsoleWindow());
    Sleep(400);

    unsigned char st[256];

    // A: F9 alone must reach the game untouched.
    keybd_event(VK_F9, 0, DOWN, 0); Sleep(60);
    hr = kb->lpVtbl->GetDeviceState(kb, sizeof st, st);
    check("F9 alone passes through", st[DIK_F9_CODE] != 0, st[DIK_F9_CODE]);

    // B: Ctrl+F9 must be filtered out.
    keybd_event(VK_CONTROL, 0, DOWN, 0); Sleep(60);
    hr = kb->lpVtbl->GetDeviceState(kb, sizeof st, st);
    check("Ctrl+F9 filtered", st[DIK_F9_CODE] == 0, st[DIK_F9_CODE]);

    // C: F10 under Ctrl as well.
    keybd_event(VK_F10, 0, DOWN, 0); Sleep(60);
    hr = kb->lpVtbl->GetDeviceState(kb, sizeof st, st);
    check("Ctrl+F10 filtered", st[DIK_F10_CODE] == 0, st[DIK_F10_CODE]);

    // D: release Ctrl, F9 still held -> visible again.
    keybd_event(VK_CONTROL, 0, UP, 0); Sleep(60);
    hr = kb->lpVtbl->GetDeviceState(kb, sizeof st, st);
    check("F9 visible again after Ctrl release", st[DIK_F9_CODE] != 0, st[DIK_F9_CODE]);

    // E: unrelated keys must not be touched while Ctrl is held.
    keybd_event(VK_CONTROL, 0, DOWN, 0); Sleep(60);
    hr = kb->lpVtbl->GetDeviceState(kb, sizeof st, st);
    check("Ctrl alone leaves F9 clear", st[DIK_F9_CODE] == 0, st[DIK_F9_CODE]);
    keybd_event(VK_CONTROL, 0, UP, 0);
    keybd_event(VK_F9, 0, UP, 0);
    keybd_event(VK_F10, 0, UP, 0);

    kb->lpVtbl->Unacquire(kb);
    kb->lpVtbl->Release(kb);
    di->lpVtbl->Release(di);

    printf("\n%s (%d failure%s)\n", fails ? "PROBE FAILED" : "PROBE OK",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
