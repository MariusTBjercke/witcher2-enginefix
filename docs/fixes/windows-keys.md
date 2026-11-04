# Windows keys and the Snipping Tool in game

Status: done 2026-11-02 (second attempt): the owner's start test passed and the Snipping Tool
works in game, with DXVK and with Windows' own d3d9. Target: Steam Enhanced Edition 3.5.0.26.

## Why

While the game is in front, Win+Shift+S, Print Screen (when Windows opens the Snipping Tool with
it) and every other Windows key combination do nothing. Witcher 1 had the same symptom from
`DISCL_NOWINKEY` on its DirectInput keyboard (its engine fix's QoL 6).

## How the game reads the keyboard (Steam 3.5.0.26)

The window object (`FUN_009d44e1`, vtable `0x00EC6560`) has an input mode at `+0x834`, set by
`FUN_009d24f8`:

* modes 0-2 (menus, cursor): keys arrive as `WM_KEYDOWN`/`WM_KEYUP` in the object's procedure
  `FUN_009D4BD9` (vtable slot 0, `__thiscall (msg, wparam, lparam)`, `RET 0xC`), which passes the
  mapped key to the engine through `FUN_009d243e`;
* mode 3 (play, hidden cursor): `FUN_009d24f8` creates a DirectInput keyboard
  (`GUID_SysKeyboard` at `0x00D6E7A8`), sets a buffer size, and calls `SetCooperativeLevel(hwnd,
  5)` at `0x009D25EE`: **`DISCL_EXCLUSIVE | DISCL_FOREGROUND`**, not `DISCL_NOWINKEY`. The mouse
  follows with `4 | 1` or `4 | 2`. `FUN_009d2817` polls the keyboard every frame
  (`GetDeviceData`, then `GetDeviceState` 256 bytes).

An exclusive keyboard blocks the Windows keys and Print Screen just like `DISCL_NOWINKEY`. The
first attempt only removed `DISCL_NOWINKEY`, which the game never sets: the log showed the hook
installed but no rewrite, and the keys stayed dead (owner, 2026-11-02).

## What the DLL does

The DLL is the game's `dinput8.dll` and already hooks `IDirectInput8::CreateDevice` (slot 3) and
the keyboard's `GetDeviceState` (slot 9). For `[hotkeys] windows_keys=1`:

* `CreateDevice` remembers the keyboard object and hooks its `SetCooperativeLevel` (slot 13,
  `HRESULT (this, HWND, DWORD)`, `__stdcall`). The slot may be shared with the mouse's class, so
  only calls on that keyboard are changed;
* `SetCooperativeLevel` on the keyboard with `DISCL_EXCLUSIVE` or `DISCL_NOWINKEY` becomes
  `DISCL_NONEXCLUSIVE` without `DISCL_NOWINKEY` (`DISCL_FOREGROUND` stays), logged once as
  `input: keyboard cooperative level 0x05 -> 0x06`;
* a non-exclusive keyboard also lets Windows send the window its key messages, so in mode 3 the
  engine would get every key twice (DirectInput and `WM_KEYDOWN`), and Alt or F10 would open the
  framed window's system menu. Before the first change, the window object's procedure slot
  `0x00EC6560` is pointed at `wk_window_proc` (data write, atomic; fingerprints: slot value, the
  prologue `55 8b ec 83 e4 f8 83 ec 64`, `RET 0xC` at `+0x2F2`). In mode 3 it drops
  `WM_KEYFIRST..WM_KEYLAST`; everything else goes to the original. If the hook cannot be placed,
  the keyboard stays exclusive (logged);
* the first change starts `winkey_thread`: while the game is in front, a Windows key going down
  is followed at once by a press and release of the unassigned virtual key `0xE8` (AutoHotkey's
  method), so a Windows key tapped on its own does not open the Start menu.

This hooks the object's vtable, not the window's `GWLP_WNDPROC`, so the DXVK repair of the window
procedure chain (`borderless-window.md`) does not touch it. `[hotkeys] windows_keys=0` leaves the
game's flags alone.

## GOG 3.5.0.26g (0.10.0)

The window object is `CViewportWindow` by RTTI on both builds: GOG's vtable slot 0 (`00F68FA0`)
holds the procedure `00A25ED3` (Steam `00EC6560` -> `009D4BD9`), `__thiscall (msg, wparam,
lparam)`, `RET 0xC`, the same checks (`55 8b ec 83 e4 f8 83 ec 64`, `c2 0c 00` at +0x2F2). The
input mode switch is `00A237A7`, the mode at +0x834, and the keyboard's `SetCooperativeLevel(hwnd,
5)` at `00A238A4`, as on Steam.

## Verification

Offline (2026-11-02): `build.bat` clean. `tools\inputprobe\probe.exe` copied into `<game>\bin`:
`CreateDevice` and `SetCooperativeLevel` succeed through the hooks and acquire/poll run on. The
probe passes `DISCL_BACKGROUND | DISCL_NONEXCLUSIVE`, so neither the rewrite nor the window
procedure hook runs there; both are first seen in the game.

In game (owner):

1. Start test (vtable hooks changed): the game reaches the main menu and loads a save.
2. Log: `input: window procedure hooked (...)` and
   `input: keyboard cooperative level 0x05 -> 0x06 (...)` once you are in play.
3. In play: Win+Shift+S and Print Screen open the Snipping Tool; a tapped Windows key does not
   open the Start menu; Win+D shows the desktop.
4. Keys act once: open and close the inventory/journal with their keys, walk, quick-save (F5),
   and press Alt and F10 alone in windowed mode: no system menu, no stuck input.
