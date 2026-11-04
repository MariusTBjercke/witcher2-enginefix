# Borderless window and the in-game Display mode row

Status: done 2026-11-02. The Display mode row and every live switch (full screen, borderless,
windowed) passed the owner's in-game test with DXVK and with Windows' own d3d9 (fourth build).
Target: Steam Enhanced Edition 3.5.0.26.

## Why

Witcher 2 has no borderless mode. Windowed mode gives the window a caption and a border and
sizes the client area to the configured resolution, centered on the desktop, so at the desktop
resolution the title bar is off-screen and the bottom of the picture is cut. Full screen is
exclusive: every alt-tab recreates the swapchain (and the engine's GUI textures), which is the
known alt-tab lag. The in-game Options > Graphics panel only has Brightness and Gamma; the
mode, resolution and VSync live in the launcher's Configurator, which writes
`Documents\Witcher 2\Config\User.ini`.

The feature is the same idea as the Witcher 1 engine fix's QoL 3: in windowed mode the window
becomes a borderless popup at the configured resolution, while Direct3D stays windowed, so an
alt-tab never recreates the device. The in-game Options > Graphics panel also gets a "Display
mode" row (full screen, borderless window, windowed), and every choice applies at once, like the
W1 options menu.

## The window and the mode switch (Steam 3.5.0.26)

`FUN_009d44e1` (`0x009D44E1`) registers class **`W2ViewportClass`** and creates the top-level
window:

| Mode | Style | ExStyle | Position | Site |
|---|---|---|---|---|
| windowed | `0x00C80000` (caption + system menu) | `0x40000` (WS_EX_APPWINDOW) | centered, client = configured resolution | `mov [ebp+0x1c],0x40000` @ `0x009D45C3`; **`mov edi,0x00C80000` @ `0x009D45CA`** |
| full screen | `0x80000000` (WS_POPUP) | `0x8` (WS_EX_TOPMOST) | 0,0 | `0x009D4562` / `0x009D4569` |
| child viewport | `0x40000000` (WS_CHILD) | `0x20` | 0,0 | `0x009D4543` / `0x009D454A` |

`AdjustWindowRect(..., edi, FALSE)` at `0x009D45E1`, then `CreateWindowExW` receives
`edi | 0x2000000` at `0x009D4681`/`0x009D4695` (IAT slot `0x00CAD338`).

`[Rendering] Fullscreen` (User.ini) is read by the settings loader `FUN_00b07714` into the
settings object at **`+0x10`** (store at `0x00B07A49`; `VSync` follows at `+0x11`). That value

* picks the window branch above (`FUN_009d44e1` argument `[ebp+0x14]`), and
* becomes `D3DPRESENT_PARAMETERS.Windowed` in `FUN_0052ccb0` (`local_a20 = (param_4 == 0)`).

The only paths into device and window creation are the viewport-init vtable method
`FUN_009e124a` (slot `0x00EC6114`) -> renderer init `FUN_009de288` -> `FUN_0052ccb0`. There is no
runtime "change the video mode" call (the in-game options never had one). The device mode can
still change live, through the engine's own lost-device path (next section).

### The lost-device path, and why it switches full screen

`FUN_0052ccb0` keeps the `D3DPRESENT_PARAMETERS` it created the device with at **`0x016267A0`**
(14 dwords; `hDeviceWindow` `+0x1C` = `0x016267BC`, **`Windowed` `+0x20` = `0x016267C0`**; the
device pointer is `0x01626788`). The only Reset, `FUN_0052d3c0` ("Resetting device..." via
`FUN_009ddbd3`), copies that block (`mov ecx,0xE; mov esi,0x16267A0` at `0x0052D4FE`) and passes it
to `IDirect3DDevice9::Reset` (vtable `+0x40`), after releasing the engine's own back buffer and
render targets.

The viewport's per-frame tick `FUN_009E146E` (viewport vtable slot **`0x00EC60F8`**, same vtable as
the init method at `0x00EC6114`; `__thiscall`, no stack arguments, returns AL) runs a state machine
at **viewport `+0xB0`**:

| State | Tick does | Next |
|---|---|---|
| 0 | nothing (device ok, render) | - |
| 1 | "Flushing all pending loading", then `FUN_009dd46b` "Releasing device..." (every device resource listener, GUI, textures) | 2 |
| 2 | `Sleep(200)` (lost, waiting until Reset is allowed) | - |
| 3 | `FUN_009ddbd3`: Reset with the stored parameters, recreate everything | 0 on success |

The state only moves on `TestCooperativeLevel` (`FUN_009dc9aa`): `DEVICENOTRESET` 0->1, 2->3;
`DEVICELOST` 0->1, 3->2. With `D3D_OK` the state is left alone. This is the alt-tab path of
exclusive full screen.

So a switch on a healthy device is: set `Windowed`, set state 1 (the engine releases), next frame
set state 3 (the engine resets into the new mode and rebuilds). `wf_vp_tick_hook` does exactly
that, inside the tick, on the game's thread; the D3D calls are all the engine's own.

The exe is SteamStub protected: `.text` is encrypted until the stub jumps to the real entry point
`0x0066F59D`, which happens *after* every static DLL's `DllMain`. A patch site must wait for its
fingerprint to appear; patching from `DllMain` would read encrypted bytes. The DLL does this from
a thread at 1 ms intervals (`wf_wait_site`).

## What the DLL does

`[window]` in `witcher2_enginefix.ini`:

| Key | Default | Meaning |
|---|---|---|
| `enabled` | 1 | 0 = no patches, no live switch, no menu channel |
| `borderless` | 0 | start borderless; only counts until the menu has saved the option |
| `dpi_aware` | 1 | make the process DPI-aware when borderless starts on |

The first builds had a `hotkey` (Ctrl+F11) for borderless; it was removed once the menu row
switched every mode live (owner's request, 2026-11-02).

Code patches (`src/window_patch.h`, installed by `window_init_thread` in `src/enginefix.c`):

* `0x009D45CA`: `mov edi,0x00C80000` -> `call wf_style_stub`. The stub loads the style from
  `wf_style` (`0x00C80000` or `0x80000000`) and changes only EDI, exactly like the instruction it
  replaces. It is in the windowed branch only, so exclusive full screen is untouched. A borderless
  start is therefore born as `WS_POPUP|WS_CLIPCHILDREN` at the configured client size, centered
  (at the desktop resolution: exactly the monitor rect).
* `0x0041F4B7` (`FUN_0041F4B7`, the config writer behind `theGame.WriteConfigParamFloat`):
  entry replaced by `jmp wf_write_hook`, with a trampoline that runs the six replaced prologue
  bytes (`push ebp; mov ebp,esp; and esp,-8`) and jumps back. The hook compares the wide-string
  category/section/key before returning the engine's own result; it never calls engine code.
  See the next section for why.
* `0x00EC60F8` (viewport vtable slot, `.rdata`): `0x009E146E` -> `wf_vp_tick_hook`, which calls
  the original through the old pointer. A data write, aligned and atomic (`InterlockedCompare
  Exchange` on the slot), so no barrier is needed. Installed only after the config writer, because
  only the menu asks for a switch. Fingerprints: the slot value, the tick's prologue and its
  `mov eax,[edi+0xB0]`, and the Reset's parameter copy at `0x0052D4FE`.

The two code patches are fingerprinted, installed under the shared peer-stop barrier
(`src/patch_barrier.h`), and fail closed (`window: ... skipped (bytes differ or out of range)`).
The DLL is pinned before patching, because the engine calls into it through those sites. During
the first in-game test (2026-11-02) the style patch was refused while the process was still
creating threads (the freeze-to-a-fixed-point pass kept finding new ones), so the install is now
retried every millisecond for up to 3 s; it stops as soon as the game window exists, because after
that the birth style no longer matters and the restyle path takes over (logged either way).

Full screen switch (`wf_vp_tick_hook`, game thread, once per frame):

* the menu's `Fullscreen` write sets `g_fsWant` (1/0);
* frame 1, state 0 and the mode differs: remember `Windowed`, write the new one, state 1 -> the
  engine releases (state 2);
* frame 2, state 2: for full screen the window first gets the game's own full screen style
  (`WS_POPUP`, `HWND_TOPMOST`, 0,0, screen size; synchronous when this thread owns the window),
  then state 3 -> the engine resets. State 0 afterwards = done; otherwise the old `Windowed` is
  put back and the engine's own retry resets in the old mode (OSD `could not switch`);
* with a wrapper `d3d9.dll` (DXVK), the window gets its class procedure (`FUN_009D237E`) back
  before every Reset and after a Reset into windowed mode (`wf_reset_wndproc`); see "The DXVK
  window-procedure loop" below;
* the worker never touches the window while a switch is pending; if the tick does not pick a
  request up within 3 s (the game not rendering), it is dropped and applies at the next start.

Window style (`wf_tick`, in the existing worker every ~120 ms):

* `FindWindowW(L"W2ViewportClass", NULL)`, verified to belong to this process;
* full screen = the engine's `Windowed` flag is 0 (falls back to `WS_POPUP` **and**
  `WS_EX_TOPMOST` before the device exists): the window is never touched;
* borderless = `WS_POPUP` without topmost; a window still topmost after leaving full screen is
  restyled (`HWND_NOTOPMOST`) even if its style already matches;
* a change sizes the client area to the swapchain's size, sets `WS_POPUP|WS_VISIBLE` or
  `0x00C80000|WS_VISIBLE` and `SetWindowPos(..., SWP_FRAMECHANGED|SWP_SHOWWINDOW|
  SWP_ASYNCWINDOWPOS|SWP_NOACTIVATE)` at (0,0) when that size equals the screen, otherwise
  centered. `WS_VISIBLE` is always added explicitly: the game's styles lack it, and an invisible
  window never takes the focus (the W1 black-screen incident).
* the swapchain's size is `[Viewport] Width/Height` from User.ini (the launcher's resolution),
  else the client area the window had when the worker first saw it (logged as
  `window: game window WxH, User.ini viewport WxH`). The *current* client area is not used: a
  framed window as large as the screen is clamped by `WM_GETMINMAXINFO`, so after
  borderless -> framed at the desktop resolution the client area is smaller than the screen, and
  the next framed -> borderless kept that smaller size (owner's test 2026-11-02: borderless again,
  but not filling the screen until a restart).
* the menu writes `EngineFixBorderless` before `Fullscreen`, so when a switch out of full screen
  completes, the wanted windowed style is already known; the worker then restyles at once.

DPI: `witcher2.exe` has no manifest and no `SetProcessDPIAware` call, so it is DPI-unaware. On a
display at anything but 100 % scaling Windows virtualizes the window (a 3840x2160 client becomes
5760x3240 physical and is clipped, and the picture is stretched by DWM). The DLL calls
`SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE)` from the same early thread,
with `SetProcessDPIAware` as the Windows 7 fallback, **before the game creates its window**.
System awareness (not PerMonitorV2) is deliberate: the window always lands on the primary monitor,
and no `WM_DPICHANGED` handling is needed. Turning borderless on at runtime on a scaled desktop
still restyles, but pixel-exact sizing needs the restart (logged).

## The in-game Display mode row

A script-patch feature, `[scriptpatches] window_mode` (default 1), edits
`game\gui\guimainmenu.ws` through the existing anchored patcher
(`src/scriptpatches/edits.txt` + `window_graphics.ws`, `window_input.ws`, `window_texts.ws`):

* `FillOptionsGraphics` (Options > Graphics, in the main menu and the pause menu) gets a local
  `var w2efModes : array< string >` (a `Body` edit: WitcherScript declares locals first) and,
  after the gamma slider, one `InsertMenuSelector` row **Display mode** (id `9201`, outside
  `EMenuButtons`) with the values Full screen / Borderless window / Windowed (index 0/1/2).
* `InputOptionsGraphics` handles it before the switch (`value` = the index, as for the game's own
  controller selector): `W2EF_SetWindowMode` writes `[Rendering] EngineFixBorderless` (1 only
  for borderless) and then `[Rendering] Fullscreen` (1 only for full screen).
* The texts follow the game's text language in all twelve languages ("Full screen" is the
  launcher's own word per language), using `StrChar` for non-ASCII like the book texts; every
  character, also upper-cased, is checked against the game's own texts with
  `tools\script-patches\check_texts.py`.

Why one selector instead of two on/off rows: the first build had **Full screen** and
**Borderless window** on/off rows, and they looked unevenly spaced (owner, 2026-11-02: Full
screen glued to the sliders, Borderless pushed down on its own). `ui_mainmenu.swf` (vanilla
`pack0.dzip`, and A Better UI's copy in `abetterui.dzip`) lays the list out with one pitch for
every row type: each row is the same `ListRowOptions` clip and `_y = index * m_iRegularOffset`,
where `m_iRegularOffset` is that clip's `_height` (DoInitAction 20523, offsets `0x9E1..0x9FE`).
A slider or selector fills its row with a second line under the label, an on/off row leaves that
space empty. Extra spacing or grouping would need a modified `ui_mainmenu.swf`, which collides
with UI mods that replace that file. A selector row has the same shape as the sliders, so the
panel reads evenly, and the three modes exclude each other by construction.

State and precedence:

* the menu reads the values with `theGame.ReadConfigParamFloat( "User", "Rendering", ... )`;
  Fullscreen wins, then EngineFixBorderless, else Windowed; a missing key counts as 0 (the
  engine's bool reader leaves the setting at 0 when the key is absent);
* the game stores them in `Documents\Witcher 2\Config\User.ini` (`[Rendering] Fullscreen`,
  `[Rendering] EngineFixBorderless`). At startup the DLL reads `EngineFixBorderless` from that
  file; only until it exists does `[window] borderless` decide (the W1 rule).

## Why the config-writer hook

The menu can only write through the engine's own config API, and the script native
`theGame.WriteConfigParamFloat` ends at `FUN_0041F4B7` (**`__stdcall`**, four arguments:
`const wchar_t* category, const wchar_t* section, const wchar_t* key, const float* value`; the
function ends in `RET 0x10`, so the callee removes the 16 argument bytes). The hook reacts to the
two keys on the game's main thread (state only) and the worker applies the restyle, so nothing
window-related runs inside a script call. Without the hook (installed too late, fingerprint
mismatch) the value still persists in User.ini and applies at the next start: no part of the
feature depends on the hook succeeding.

**The first in-game test crashed here (2026-11-02), and the cause was this calling convention.**
The hook and the trampoline pointer were declared `__cdecl`. The original's `RET 0x10` already
removed the arguments, and the cdecl hook then removed them again, so ESP ended 16 bytes high:
the hook's epilogue restored EBP from a stack slot holding the wrapper's return address
(`0x009B50EF`, visible in the crash context) and returned into the stack, where DEP then raised
`0xC0000005` executing `0x001ABBD4`. The crash dump and log are in
`docs/validation/borderless-first-test-2026-11-02.md`. Both are `__stdcall` now;
`tools\windowprobe` runs the same hook/trampoline against a synthetic `__stdcall` target with the
real prologue and `RET 0x10` and checks that the arguments arrive, the return value is passed and
ESP is unchanged. The same probe built against a `__cdecl` hook reproduces the crash
(`0xC0000005`), so the regression is pinned.

Why not poll the file: the writer marks the config dirty and the engine saves User.ini later
(typically at exit), so file polling would not be timely. Why not call the engine's read function
from the DLL thread: it allocates and is not documented as thread-safe.

## The DXVK window-procedure loop (third build, 2026-11-02)

Owner's test of the live switch, DXVK on NVIDIA, game started in full screen: full screen ->
borderless -> windowed worked; -> full screen again killed the process silently at 15:37:37. The
DLL's crash filter logged nothing (it runs on the overflowed stack and faulted in its own stack
probe); Windows kept `%LOCALAPPDATA%\CrashDumps\witcher2.exe.42364.dmp`. Offline `cdb`: exception
`c00000fd` (stack overflow) on the main thread, the stack filled by ~10 700 frames in DXVK's
`d3d9.dll` window procedure alternating with ~8 500 `opengl32!wglWndProc` (NVIDIA's Vulkan
driver) through `CallWindowProcW`. `witcher2_d3d9.log` ends at the second
`Setting display mode: 3840x2160@0`, i.e. inside the Reset into full screen.

DXVK (`src/d3d9/d3d9_window.cpp`): `EnterFullscreenMode` calls `HookWindowProc`, which stores
whatever `SetWindowLongPtr(GWLP_WNDPROC, D3D9WindowProc)` returns as the original;
`LeaveFullscreenMode` calls `ResetWindowProc`, which restores the original **only if
`D3D9WindowProc` is still the top procedure**, and erases its record either way; a
`D3D9WindowProc` without a record passes everything to `DefWindowProc`. With a full screen start
DXVK hooks first and NVIDIA subclasses on top (wgl -> DXVK -> game). Leaving full screen therefore
leaves DXVK's procedure orphaned in the chain (and the game's own procedure cut off), and entering
full screen again stores wgl as DXVK's original while wgl's original is DXVK: a loop on the first
message.

Fix: `wf_reset_wndproc` puts the class procedure back (`SetWindowLongW(GWL_WNDPROC)`, only when
`GetClassLongW(GCL_WNDPROC)` is the game's `0x009D237E` and the loaded `d3d9.dll` is not from the
system directory): before every Reset, so DXVK always hooks a clean chain and unhooks without
leaving anything behind, and after a Reset into windowed mode. After a Reset into full screen
DXVK's hook is left in place. NVIDIA's subclass is dropped at a switch; nothing in the game needs
it.

## GOG 3.5.0.26g (0.10.0)

The same code, compiled again; proven statically on 2026-11-04 (the `k_gog` rows in
`src/enginefix.c`):

| | Steam | GOG |
|---|---|---|
| window creation (`L"W2ViewportClass"`) | `009D44E1` | `00A257D7`, the same function instruction for instruction, `RET 0x18` |
| windowed style site `bf 00 00 c8 00` | `009D45CA` | `00A258C0` |
| class procedure | `009D237E` | `00A2362D` (stored at `00A2592D`) |
| render interface (RTTI `CRenderInterface`), tick slot +0x0C | `00EC60EC` -> `009E146E` | `00F68B2C` -> `00A32999` (pushes "Flushing all pending loading") |
| Reset copy `mov ecx,0Eh; mov esi,WF_PP` | `0052D4FE` | `004CE64E`, the operand relocated |
| device / stored present parameters | `01626788` / `016267A0` | `01690818` / `01690830`, stored in device creation `004CDE00` |
| config writer (`WriteConfigParamFloat`) | `0041F4B7`, `__stdcall` | `00420D54`, **`__thiscall`** |
| config manager | `01608650` (the writer reads it) | `01679440` (callers load it into ECX) |

The lost-device state at +0xB0 means the same on GOG (0 render, 1 flush and release, 2 Sleep(200),
3 Reset; TestCooperativeLevel moves it the same way). The one difference that matters is the
writer: it keeps its bytes (`55 8b ec 83 e4 f8`, the trampoline's prologue) but takes the config
manager in ECX, which Steam's writer reads from the global itself. A `__stdcall` hook would lose
ECX before the trampoline runs, so GOG gets `wf_write_hook_gog` (`__fastcall`: ECX passed on, 16
bytes of stack arguments, `RET 0x10`), and the Draw distance sync calls GOG's trampoline with
`ECX = [01679440]`. `tools/windowprobe` runs both detours against synthetic targets; the GOG one
checks that ECX survives the hook and the trampoline. The Reset copy is checked as
`b9 0e 00 00 00 be` plus `ga(WF_PP)`, since the operand is relocated under ASLR.

## Behaviour and limits

* Borderless only makes sense with **Full screen off**; the Display mode row sets both keys. A
  start in full screen with `[window] borderless=1` (and no menu value yet) stays full screen.
* The live full screen switch costs what an alt-tab out of full screen costs (the engine flushes
  pending loading, releases and recreates its device resources): a short pause, not a restart.
* Switching between full screen and windowed over a loaded level, with the menu open, shows the
  characters behind the menu in their bind pose (T-pose) until the menu closes (owner,
  2026-11-02). The menu pauses the world, so animation does not run and the skinning data that the
  device reset dropped is only rebuilt when the game resumes. Nothing is unloaded and nothing
  persists. This should be the same as a vanilla alt-tab out of full screen in the menu; that
  comparison is not done. A fix would need the engine's skinning update forced once while paused,
  which has not been located; left as is.
* If the menu channel or the viewport tick is not hooked (fingerprint mismatch), Full screen
  applies at the next start and the OSD says so.
* At the desktop resolution the client area equals the monitor, so the popup covers it exactly.
  At a lower resolution it is a centered, frameless window at that size (and cannot be moved);
  the README says to pick the desktop resolution.
* The window is placed on the primary monitor (the game's own `GetSystemMetrics` centering);
  borderless does not add multi-monitor selection.
* Windowed mode inherits the game's windowed behaviour: the mouse modes (`FUN_009d24f8`, which
  clips the cursor to the window rect) already work there. The background-pause behaviour has not
  been measured in windowed mode; if it differs from full screen it is a separate follow-up.
* Brightness/Gamma (device gamma ramp) may not apply in windowed mode, and DXVK ignores it; the
  sliders are unchanged. Measure before promising anything.
* The launcher's Configurator may rewrite User.ini when the user presses OK there and can drop
  `EngineFixBorderless`; `[window] borderless` is then the fallback again.
* Borderless and windowed stay in D3D9 windowed presentation (`Windowed=1`), which DXVK and
  native d3d9 both support. Full screen goes through `Reset` with `Windowed=0`, the same call the
  game makes after an alt-tab; with DXVK that is DXVK's full screen emulation, as at a full screen
  start.

## Verification

Offline (2026-11-02, this machine):

* `cmd /c build.bat` -> `Built build\dinput8.dll`, no warnings from the new code.
* `cmd /c tools\patchtest\build.bat`; `build\sptest.exe test --game "<game>"` -> 23 tests,
  0 failures, `script_real` markers now `2 1 9 6 2` (the WindowMode rows add 3 blocks to
  `guimainmenu.ws`).
* `build\sptest.exe build "<game>" <scratch>` -> `window_mode=on`, 4 redirects; the patched
  `guimainmenu.ws` has both rows inside `FillOptionsGraphics`, the handler as the first body lines
  of `InputOptionsGraphics`, and the helpers/texts at the end.
* `python tools\script-patches\check_texts.py "<game>\CookedPC"` -> every window label, also
  upper-cased, exists in that language's own texts (including JP katakana, ZH traditional and
  Turkish).
* `cmd /c tools\distanceprobe\build-and-run.bat` -> 28 native distance checks, 9 hotkey checks
  and 21 preset checks, 0 failures (the shared barrier was extracted to
  `src/patch_barrier.h` for this feature; the probes prove the refactor).
* `cmd /c tools\windowprobe\build-and-run.bat` -> 15 checks, 0 failures: the synthetic target's
  prologue equals the real one, the detour installs, the trampoline delivers the arguments, the
  configured key/float reaches the DLL, other keys do not, the stack balances across every call
  and the style stub returns the wanted style with only EDI changed. Built with a `__cdecl` hook
  (the first-test bug) the same probe dies with `0xC0000005`, so it is a real regression test.

In game (owner), first attempt 2026-11-02: the two Options rows appeared and selecting Borderless
crashed the game; root cause and fix above, evidence in
[the validation note](../validation/borderless-first-test-2026-11-02.md). Re-test pending with the
`__stdcall` hook.

In game (owner), second build 2026-11-02 (`__stdcall` hook, two rows): started borderless at
3840x2160 (`window: game window 3840x2160, User.ini viewport 3840x2160`), borderless off/on
repeatedly from the menu, every switch back to borderless filled the screen. Full screen still
said "restart". The owner then asked for the live full screen switch, one grouped control and no
hotkey: this build.

Offline for this build: `build.bat` clean; `sptest test` 0 failures (markers `2 1 10 6 2`, 18
edits, 4 WindowMode); `sptest build` puts the `var` line first in `FillOptionsGraphics`, the
selector after gamma and the handler first in `InputOptionsGraphics`; `check_texts.py` ok for the
four window texts in all twelve languages; `windowprobe` 15/15 and `distanceprobe` 0 failures.
The viewport tick hook has no offline probe (it calls the engine's own tick at its fixed address).

In game (owner), with the DLL installed:

1. Log: `window: menu channel hooked (...)`, then `window: viewport tick hooked (Full screen
   switches at once)`.
2. Options > Graphics: one Display mode row after Gamma, in the game's language, evenly spaced
   like the sliders.
3. Borderless -> Windowed -> Borderless: framed, then filling the screen again
   (`Display mode: windowed / borderless window`).
4. -> Full screen: a short pause, then exclusive full screen (`Display mode: full screen`, log
   `window: full screen on`); alt-tab out and back still works.
5. Full screen -> Borderless and -> Windowed: back without a restart, not topmost, right size.
6. Restart: the game starts in the last chosen mode (User.ini `Fullscreen`,
   `EngineFixBorderless`).
7. Optionally without DXVK (`bin\d3d9.dll` renamed while the game is closed).

Fourth build in game (owner, 2026-11-02, DXVK + NVIDIA, 4K): the crash is gone; every switch
between the three modes works, repeatedly, and Options shows the choice without an OSD line. The
same test with DXVK disabled (`bin\d3d9.dll` renamed, Windows' own d3d9) also passed. One
cosmetic effect was seen: the bind pose behind the menu (Behaviour and limits).
