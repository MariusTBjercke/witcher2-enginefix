// Borderless window support for Steam 3.5.0.26 - the machine-code pieces. The engine and the
// behaviour live in enginefix.c; see docs/fixes/borderless-window.md for the research.
//
// Two engine sites, both fingerprinted and fail-closed:
//
//  1. FUN_009D45CA  "mov edi,0x00C80000" - the style the game's *windowed* window is born with
//     (FUN_009d44e1, the W2ViewportClass window). Replaced by "call wf_style_stub", which loads
//     the style from wf_style: 0x00C80000 framed (the original) or 0x80000000 WS_POPUP for a
//     borderless window. Only EDI changes, exactly as the replaced instruction did. The call site
//     is the windowed branch only, so exclusive fullscreen is untouched.
//
//  2. FUN_0041F4B7 - the engine's config writer, __stdcall (category, section, key, float value;
//     RET 0x10, the callee removes the arguments). The script native "theGame.WriteConfigParamFloat"
//     ends here, so hooking the entry lets the in-game options menu
//     (docs/fixes/borderless-window.md) change borderless at once. The
//     6-byte prologue is replaced by a jump to a trampoline that runs it and jumps back.
#ifndef W2_WINDOW_PATCH_H
#define W2_WINDOW_PATCH_H

#include <wchar.h>
#include "patch_barrier.h"

#define WF_STYLE_SITE   0x009D45CAu   // mov edi, 0x00C80000 (windowed branch of FUN_009d44e1)
#define WF_WRITER_SITE  0x0041F4B7u   // config writer, __stdcall (RET 0x10)
// Live full screen switch (enginefix.c, wf_vp_tick_hook). The engine keeps one copy of the
// D3DPRESENT_PARAMETERS it created the device with (FUN_0052ccb0) and passes that copy to every
// Reset (FUN_0052d3c0, copy at WF_RESET_COPY); the viewport's per-frame tick drives the lost-device
// state machine at +0xB0 (docs/fixes/borderless-window.md).
#define WF_VP_TICK_SLOT 0x00EC60F8u   // viewport vtable slot -> FUN_009E146E
#define WF_VP_TICK      0x009E146Eu   // __thiscall, no stack arguments, returns AL
#define WF_VP_STATE     0xB0          // 0 ok, 1 release next, 2 released, 3 Reset next
#define WF_RESET_COPY   0x0052D4FEu   // mov ecx,0xE; mov esi,WF_PP (the Reset's parameter copy)
#define WF_D3D_DEVICE   0x01626788u   // IDirect3DDevice9*
#define WF_PP           0x016267A0u   // the stored D3DPRESENT_PARAMETERS
#define WF_PP_HWND      (WF_PP + 0x1C)
#define WF_PP_WINDOWED  (WF_PP + 0x20)
#define WF_GAME_WNDPROC 0x009D237Eu   // W2ViewportClass lpfnWndProc (FUN_009d44e1)
#define WF_STYLE_FRAMED 0x00C80000u   // caption + system menu, the game's windowed style
#define WF_STYLE_POPUP  0x80000000u   // WS_POPUP, the game's fullscreen style (without WS_EX_TOPMOST)

// Bytes checked before anything is written; the prologue length is what the writer trampoline
// reproduces (push ebp; mov ebp,esp; and esp,-8).
static const unsigned char wf_style_orig[5]  = { 0xBF, 0x00, 0x00, 0xC8, 0x00 };
static const unsigned char wf_writer_orig[6] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8 };

// The borderless state wanted now (0/1) and the style the window will be born with. Both are
// read by the game thread (the stub) and written by the window thread.
static volatile LONG wf_borderless;
static volatile LONG wf_style = WF_STYLE_FRAMED;

// enginefix.c: called on the game's main thread when the menu writes one of our keys.
static void wf_on_config_write(const wchar_t* section, const wchar_t* key, float value);

// Read by the engine instead of "mov edi,0x00C80000". No other register, flag or stack slot is
// touched, so the surrounding code is unchanged.
__declspec(naked) static void wf_style_stub(void)
{
    __asm {
        mov edi, dword ptr [wf_style]
        ret
    }
}

// FUN_0041F4B7 ends in "RET 0x10": it is __stdcall, the callee removes the four arguments. The
// hook and the pointer to the trampoline must use the same convention; a __cdecl hook would let
// the original clean the arguments and then remove them a second time (observed 2026-11-02: the
// stack shifted by 16 bytes and the hook returned into the stack).
// Builds the trampoline a detoured function is reached through: the replaced prologue bytes,
// then a jump back to the first byte after them. The prologue must end on an instruction
// boundary (6 bytes here: push ebp; mov ebp,esp; and esp,-8) and cover at least the 5 bytes of
// the jump. Shared with tools/windowprobe, which runs a synthetic __stdcall target.
static void* wf_make_trampoline(void* site, const unsigned char* prologue, unsigned len)
{
    unsigned char* t = (unsigned char*)VirtualAlloc(NULL, len + 8, MEM_COMMIT | MEM_RESERVE,
                                                    PAGE_EXECUTE_READWRITE);
    if (!t) return NULL;
    memcpy(t, prologue, len);
    t[len] = 0xE9;
    *(uint32_t*)(t + len + 1) =
        (uint32_t)(((uintptr_t)site + len) - ((uintptr_t)t + len + 5));
    return t;
}

// __thiscall with no stack arguments: ECX = the viewport, EDX unused, so __fastcall matches.
typedef char (__fastcall *wf_vp_tick_fn)(void* vp, void* edx);

typedef char (__stdcall *wf_write_fn)(const wchar_t* category, const wchar_t* section,
                                      const wchar_t* key, const float* value);
static wf_write_fn wf_real_write;   // the trampoline, set by the installer

// Entry replacement: observe the write (the engine stores it as usual) and tell enginefix.c.
// Strings are borrowed for the duration of this call, so they are only compared here.
static char __stdcall wf_write_hook(const wchar_t* category, const wchar_t* section,
                                    const wchar_t* key, const float* value)
{
    char r;

    if (!wf_real_write) return 0;
    r = wf_real_write(category, section, key, value);
    if (category && section && key && value && _wcsicmp(category, L"User") == 0 &&
        _wcsicmp(section, L"Rendering") == 0 &&
        (_wcsicmp(key, L"EngineFixBorderless") == 0 || _wcsicmp(key, L"Fullscreen") == 0 ||
         _wcsicmp(key, L"EngineFixDrawDistance") == 0))
        wf_on_config_write(section, key, *value);
    return r;
}

#endif


