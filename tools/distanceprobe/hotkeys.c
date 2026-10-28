// Exercise the actual DLL hotkey handlers with deterministic key-state input.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static int test_mods, test_f9, test_f10;
static SHORT WINAPI probe_key_state(int key) {
    if (key == VK_CONTROL) return test_mods & 1 ? (-32767-1) : 0;
    if (key == VK_SHIFT) return test_mods & 4 ? (-32767-1) : 0;
    if (key == VK_MENU) return test_mods & 2 ? (-32767-1) : 0;
    if (key == VK_F9) return test_f9 ? (-32767-1) : 0;
    if (key == VK_F10) return test_f10 ? (-32767-1) : 0;
    return 0;
}
#define GetAsyncKeyState probe_key_state
#include "../../src/enginefix.c"
#undef GetAsyncKeyState
static int failures;
static void check(const char* label, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) failures++;
}
static void keys(int mods, int f9, int f10) {
    test_mods=mods; test_f9=f9; test_f10=f10;
    poll_hotkeys(); poll_distance_hotkeys();
}
int main(void) {
    cfg_osd=0; cfg_log=0;
    cfg_mesh_steps[0]=1; cfg_mesh_steps[1]=2; cfg_mesh_steps[2]=4; cfg_mesh_steps[3]=8;
    cfg_foliage_steps[0]=1; cfg_foliage_steps[1]=2; cfg_foliage_steps[2]=3; cfg_foliage_steps[3]=5;
    cfg_mesh_step_n=cfg_foliage_step_n=4;
    cfg_mesh=4; cfg_foliage=2; g_step_mesh=2; g_step_foliage=1;
    keys(0,0,0);
    keys(5,1,0);
    check("Ctrl+Shift+F9 changes only visibility", cfg_visibility==1.5f && cfg_mesh==4);
    keys(1,1,0);
    check("releasing Shift while holding F9 cannot change LOD", cfg_mesh==4);
    keys(5,1,0);
    check("adding Shift back while holding F9 cannot retrigger visibility", cfg_visibility==1.5f);
    keys(0,0,0); keys(1,1,0);
    check("Ctrl+F9 retains mesh control", cfg_mesh==8 && cfg_visibility==1.5f);
    keys(5,1,0);
    check("adding Shift to held Ctrl+F9 cannot trigger visibility", cfg_visibility==1.5f);
    keys(0,0,0); keys(5,0,1);
    check("Ctrl+Shift+F10 changes only shadow range", cfg_shadow_range==1.5f && cfg_foliage==2);
    keys(1,0,1);
    check("releasing Shift while holding F10 cannot change foliage", cfg_foliage==2);
    keys(0,0,0); keys(1,0,1);
    check("Ctrl+F10 retains foliage control", cfg_foliage==3 && cfg_shadow_range==1.5f);
    keys(0,0,0); keys(5,1,0);
    check("physical release rearms distance key", cfg_visibility==2.0f && ef_range_sq==4.0f);
    printf("%s: %d failures\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
