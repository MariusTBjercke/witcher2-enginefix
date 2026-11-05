// Executes the real window-mode config-writer detour against a synthetic target that has the
// exact prologue and epilogue of FUN_0041F4B7 (55 8B EC 83 E4 F8 ... RET 0x10): the function is
// __stdcall, the callee removes its four arguments. This pins the 2026-11-02 crash, where a
// __cdecl hook let the original clean the arguments and then removed them a second time, so the
// stack shifted by 16 bytes and the hook returned into the stack.
//
// Also covers the synthetic counterpart of the style stub: the patch replaces a 5-byte
// "mov edi,imm32" with a call to wf_style_stub, which must leave every register but EDI alone.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if !__has_include("../../src/window_patch.h")
int main(void) { puts("FAIL: window patch implementation is absent"); return 1; }
#else
#include "../../src/window_patch.h"

static int failures;
static void check(const char* what, int pass)
{
    printf("%s %s\n", pass ? "PASS" : "FAIL", what);
    if (!pass) failures++;
}

// The hook calls this (declared in window_patch.h). The harness records the notification.
static int notified;
static const wchar_t* notified_key;
static float notified_value;
static void wf_on_config_write(const wchar_t* section, const wchar_t* key, float value)
{
    (void)section;
    notified++;
    notified_key = key;
    notified_value = value;
}

// The synthetic original: the real prologue, record the four stack arguments, return 1 with
// "ret 16" (stdcall). Only EAX and ESP/EBP are touched; no callee-saved register is clobbered.
static const wchar_t* seen_cat;
static const wchar_t* seen_sec;
static const wchar_t* seen_key;
static float seen_value;
static int seen_calls;

#pragma warning(push)
#pragma warning(disable: 4100) // named parameters are referenced by stack slot in naked assembly
__declspec(naked) static char __stdcall fake_writer(const wchar_t* cat, const wchar_t* sec,
                                                    const wchar_t* key, const float* value)
{
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        mov eax, dword ptr [ebp + 8]
        mov dword ptr [seen_cat], eax
        mov eax, dword ptr [ebp + 0Ch]
        mov dword ptr [seen_sec], eax
        mov eax, dword ptr [ebp + 10h]
        mov dword ptr [seen_key], eax
        mov eax, dword ptr [ebp + 14h]
        mov eax, dword ptr [eax]
        mov dword ptr [seen_value], eax
        inc dword ptr [seen_calls]
        mov al, 1
        mov esp, ebp
        pop ebp
        ret 10h
    }
}
#pragma warning(pop)

// GOG 3.5.0.26g: the same writer as __thiscall - ECX is the config manager, then the same four
// stack arguments, the same prologue and RET 0x10.
static void* seen_this;

#pragma warning(push)
#pragma warning(disable: 4100)
__declspec(naked) static void fake_writer_gog(void)
{
    __asm {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        mov dword ptr [seen_this], ecx
        mov eax, dword ptr [ebp + 8]
        mov dword ptr [seen_cat], eax
        mov eax, dword ptr [ebp + 0Ch]
        mov dword ptr [seen_sec], eax
        mov eax, dword ptr [ebp + 10h]
        mov dword ptr [seen_key], eax
        mov eax, dword ptr [ebp + 14h]
        mov eax, dword ptr [eax]
        mov dword ptr [seen_value], eax
        inc dword ptr [seen_calls]
        mov al, 1
        mov esp, ebp
        pop ebp
        ret 10h
    }
}
#pragma warning(pop)

// One call through the (possibly patched) entry from a naked frame, so ESP can be compared on
// both sides. stdcall: the callee removes the four arguments.
typedef char (__stdcall *fake_fn)(const wchar_t*, const wchar_t*, const wchar_t*, const float*);
static const wchar_t* call_cat;
static const wchar_t* call_sec;
static const wchar_t* call_key;
static float call_value;
static DWORD esp_before, esp_after;
static char call_result;

__declspec(naked) static void call_fake(void)
{
    __asm {
        mov dword ptr [esp_before], esp
        lea eax, dword ptr [call_value]
        push eax
        push dword ptr [call_key]
        push dword ptr [call_sec]
        push dword ptr [call_cat]
        call fake_writer
        mov dword ptr [esp_after], esp
        mov dword ptr [call_result], eax
        ret
    }
}

// GOG's caller loads ECX with the config manager before the call.
static void* call_this;
__declspec(naked) static void call_fake_gog(void)
{
    __asm {
        mov dword ptr [esp_before], esp
        lea eax, dword ptr [call_value]
        push eax
        push dword ptr [call_key]
        push dword ptr [call_sec]
        push dword ptr [call_cat]
        mov ecx, dword ptr [call_this]
        mov edx, 0BADF00Dh
        call fake_writer_gog
        mov dword ptr [esp_after], esp
        mov dword ptr [call_result], eax
        ret
    }
}

// Install the same detour enginefix.c installs: E9 <hook> + NOP, trampoline with the 6 prologue
// bytes built by the shared helper.
static int install_hook_at(uintptr_t site_addr, void* hook, void** tramp)
{
    unsigned char* site = (unsigned char*)site_addr;
    int64_t delta = (int64_t)(uintptr_t)hook - (int64_t)(site_addr + 5);
    unsigned char code[6];
    DWORD old = 0, tmp = 0;

    if (memcmp(site, wf_writer_orig, sizeof wf_writer_orig) != 0) return 0;
    if (delta < INT32_MIN || delta > INT32_MAX) return 0;
    *tramp = wf_make_trampoline(site, wf_writer_orig, sizeof wf_writer_orig);
    if (!*tramp) return 0;
    code[0] = 0xE9;
    *(uint32_t*)(code + 1) = (uint32_t)(int32_t)delta;
    code[5] = 0x90;
    if (!VirtualProtect(site, sizeof code, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(site, code, sizeof code);
    VirtualProtect(site, sizeof code, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof code);
    return 1;
}

static int install_hook(void)
{
    void* tramp = NULL;
    int ok = install_hook_at((uintptr_t)fake_writer, (void*)wf_write_hook, &tramp);
    wf_real_write = (wf_write_fn)tramp;
    return ok;
}

static int install_hook_gog(void)
{
    void* tramp = NULL;
    int ok = install_hook_at((uintptr_t)fake_writer_gog, (void*)wf_write_hook_gog, &tramp);
    wf_real_write_gog = (wf_write_gog_fn)tramp;
    return ok;
}

// The style stub: call it from a naked frame with known register values and check that only EDI
// changes and that the return goes back where the call put it. EDI is the caller's
// (callee-saved), so this frame saves it around the stub.
static DWORD stub_ebx, stub_esi, stub_ebp, stub_edi, stub_esp;
__declspec(naked) static void call_style_stub(void)
{
    __asm {
        push ebx
        push esi
        push ebp
        push edi
        mov ebx, 11111111h
        mov esi, 22222222h
        mov ebp, 33333333h
        call wf_style_stub
        mov dword ptr [stub_ebx], ebx
        mov dword ptr [stub_esi], esi
        mov dword ptr [stub_ebp], ebp
        mov dword ptr [stub_edi], edi
        mov dword ptr [stub_esp], esp
        pop edi
        pop ebp
        pop esi
        pop ebx
        ret
    }
}

static const wchar_t k_user[] = L"User";
static const wchar_t k_rendering[] = L"Rendering";
static const wchar_t k_borderless[] = L"EngineFixBorderless";
static const wchar_t k_other[] = L"MeshDistanceScale";

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   // a crash must not swallow the lines before it
    // The synthetic target must be byte-identical to the real prologue this the detour expects.
    check("synthetic prologue matches the real one",
          memcmp((const void*)(uintptr_t)fake_writer, wf_writer_orig, sizeof wf_writer_orig) == 0);

    // Unpatched baseline: the four arguments arrive in the right slots.
    call_cat = k_user; call_sec = k_rendering; call_key = k_borderless; call_value = 3.5f;
    call_fake();
    check("unpatched call reaches the body", seen_calls == 1 && call_result == 1);
    check("unpatched arguments are in order",
          seen_cat == k_user && seen_sec == k_rendering && seen_key == k_borderless &&
          seen_value == 3.5f);
    check("unpatched call balances the stack", esp_before == esp_after);

    check("detour installs", install_hook());

    // Patched: the hook runs, the trampoline still delivers the arguments, the stack balances.
    call_value = 1.0f;
    notified = 0;
    call_fake();
    check("patched call reaches the body once", seen_calls == 2 && call_result == 1);
    check("patched arguments are in order",
          seen_cat == k_user && seen_sec == k_rendering && seen_key == k_borderless &&
          seen_value == 1.0f);
    check("patched call balances the stack (the 2026-11-02 crash)",
          esp_before == esp_after);
    check("our key is reported to enginefix.c",
          notified == 1 && notified_key == k_borderless && notified_value == 1.0f);

    // Another key goes through the trampoline but is not reported.
    call_key = k_other;
    call_value = 2.0f;
    notified = 0;
    call_fake();
    check("other keys still reach the engine", seen_calls == 3 && seen_key == k_other &&
          seen_value == 2.0f);
    check("other keys are not reported", notified == 0);
    {
        // Every options row's key has to pass the filter, or its row silently does nothing.
        static const wchar_t* const rows[] = {
            L"EngineFixBorderless", L"Fullscreen", L"EngineFixDrawDistance", L"EngineFixShadowQuality"
        };
        float level = 1.0f;
        unsigned r;
        for (r = 0; r < sizeof rows / sizeof rows[0]; r++) {
            notified = 0;
            notified_key = NULL;
            wf_note_write(L"User", L"Rendering", rows[r], &level);
            check("an options row's key is reported", notified == 1 && notified_key == rows[r]);
        }
        notified = 0;
        wf_note_write(L"User", L"Other", rows[3], &level);
        check("the same key in another section is not reported", notified == 0);
    }
    check("third call balances the stack", esp_before == esp_after);

    // GOG: the thiscall writer keeps its config manager in ECX through the hook and trampoline.
    check("GOG synthetic prologue matches", memcmp((const void*)(uintptr_t)fake_writer_gog,
          wf_writer_orig, sizeof wf_writer_orig) == 0);
    call_this = (void*)(uintptr_t)0x12345678u;
    call_cat = k_user; call_sec = k_rendering; call_key = k_borderless; call_value = 0.0f;
    seen_this = NULL; seen_calls = 0;
    call_fake_gog();
    check("GOG unpatched call gets ECX and the arguments", seen_calls == 1 && seen_this == call_this &&
          seen_key == k_borderless && seen_value == 0.0f && esp_before == esp_after);
    check("GOG detour installs", install_hook_gog());
    notified = 0; seen_this = NULL; call_value = 1.0f;
    call_fake_gog();
    check("GOG patched call keeps the config manager in ECX", seen_calls == 2 && seen_this == call_this);
    check("GOG patched arguments are in order", seen_cat == k_user && seen_sec == k_rendering &&
          seen_key == k_borderless && seen_value == 1.0f && call_result == 1);
    check("GOG patched call balances the stack", esp_before == esp_after);
    check("GOG: our key is reported", notified == 1 && notified_key == k_borderless && notified_value == 1.0f);
    call_key = k_other; notified = 0;
    call_fake_gog();
    check("GOG: other keys reach the engine, not reported", seen_calls == 3 && seen_key == k_other &&
          notified == 0 && esp_before == esp_after);

    // The style stub changes only EDI.
    wf_style = (LONG)WF_STYLE_FRAMED;
    call_style_stub();
    check("style stub returns with the framed style", stub_edi == (DWORD)WF_STYLE_FRAMED);
    check("style stub keeps the other registers",
          stub_ebx == 0x11111111u && stub_esi == 0x22222222u && stub_ebp == 0x33333333u);
    wf_style = (LONG)WF_STYLE_POPUP;
    call_style_stub();
    check("style stub follows the wanted style", stub_edi == (DWORD)WF_STYLE_POPUP);

    printf("%s: %d failure%s\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
#endif
