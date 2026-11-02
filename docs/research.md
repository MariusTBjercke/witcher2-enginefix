# The Witcher 2 — engine research log

Living document. Newest findings at the bottom of each section.

## 1. Target binary

| Property | Value |
|---|---|
| Game | The Witcher 2: Assassins of Kings — Enhanced Edition (Steam appid 20920) |
| Version | 3.5.0.26 (buildid 13542023) |
| File | `The Witcher 2/bin/witcher2.exe` |
| Size | 14.96 MB (15 685 632 bytes) |
| SHA-256 | `CB9FBC9C1DC7AE0AC73C5768E55724CC8E7B78999299F0A2E6E5AB7D74A55FE8` |
| Image base | `0x00400000` |
| Arch | x86 PE32, MSVC, 7 sections |
| LAA | yes (`IMAGE_FILE_LARGE_ADDRESS_AWARE` set — >2 GB address space already available) |

### Packing

The shipped exe **is** Steam DRM protected:

```
.text      VA 0x00001000  VSize 0x008A94AD  (code — encrypted by the stub)
PSFD00     VA 0x008AB000  VSize 0x00001E6A
.rdata     VA 0x008AD000  VSize 0x002CB1B4  (plaintext — RTTI + settings keys)
.data      VA 0x00B79000  VSize 0x0183CA64  (mostly BSS, raw 1.6 MB)
.rsrc      VA 0x023B6000  VSize 0x0003C370
.reloc     VA 0x023F3000  VSize 0x00132392
.bind      VA 0x02526000  VSize 0x0008A000  <-- SteamStub payload
```

`.bind` is the SteamStub marker, so **the exe must be unpacked before analysis**:
Ghidra's auto-analysis of the packed binary yields only 411 functions (the stub).

Unpacked with **Steamless v3.1.0.5** (`tools/steamless/Steamless.CLI.exe`):

* detected as **SteamStub Variant 2.1**
* ships a `SteamDRMP.dll` payload (offsets are pulled out of it)
* `.bind` removed, `.text` decrypted and linked as main code section
* output: `witcher2.exe.unpacked.exe`, 14.42 MB, same image base
* real entry point: **`0x0066F59D`** (the packed file's entry points at the stub)
* memory size 38 936 164 bytes (~37.1 MB image)

Never overwrite the game's exe — unpack a copy. Steam keeps working with the
original file untouched.

## 2. Config system

The engine registers its ini keys as **UTF-16LE (wide) strings in `.rdata`**, next
to the section names. Scanning the packed exe for ASCII only finds nothing; a
wide-string scan finds them immediately. This matters for any future string work:
`User.ini`, `Rendering.ini`, `base_scripts.dzip` and `.dzip` are all wide strings,
while `d3d9.dll`, `fmodex.dll`, `kernel32.dll` are ASCII.

Sections confirmed present as wide strings: `Rendering`, `Engine`, `FileSystem`,
`Viewport`, `TextureStreaming`, `TextureGroups`.

### Rendering / LOD keys found in the binary

| Key | Notes |
|---|---|
| `MeshDistanceScale` | Localised in `bin/config/lang/EN.ini` as **"LOD Distance"** — the launcher slider. Written to `[Rendering]`. |
| `FoliageDistanceScale` | **No launcher slider.** Only present in the quality presets. This is the hidden vegetation-draw-distance knob. |
| `CharacterLodGameplayDownscale` | 0 = no downscale of character LOD during gameplay. |
| `TextureMemoryBudget`, `MaxTextureSize`, `MaxAtlasTextureSize`, `MaxResidentMipMap`, `TextureDownscale`, `AtlasTextureDownscale`, `DetailTextureDownscale` | Texture streaming; `MaxResidentMipMap` lives in `Rendering.ini`, the rest in `User.ini`. |
| `UberSampling`, `AllowSharpen`, `DanglesLimiter`, `PerformancePlatform`, `CustomRenderingSettings` | Misc. |

### Preset values (`bin/config/profiles/*.ini`, `[Rendering]`)

| Preset | `MeshDistanceScale` | `FoliageDistanceScale` | `CharacterLodGameplayDownscale` |
|---|---|---|---|
| PCLow | 0.7 | 0.5 | 1 |
| PCMedium | 0.8 | 0.6 | 1 |
| PCHigh | 0.9 | 1.0 | 0 |
| PCUber | 1.0 | 1.0 | 0 |
| `bin/config/User.ini` (default template) | 0.5 | *(absent)* | *(absent)* |

The most interesting fact for pop-in work: **`FoliageDistanceScale` never exceeds
1.0 in any shipped preset**, and the launcher has no control for it at all, so a
value >1.0 is unexplored territory — including whether the engine clamps it.

## 3. Entity / mesh level distance properties

Wide strings in the binary that name serialised object properties (these are read
from `.w2ent` / `.w2mesh` / `.w2l` data, not from `User.ini`):

* `CFoliageDefinition`, `CFoliageContainer`, `CFoliageEntity`, `OCFoliageContainer`,
  `MC_FoliageInstances`, `MC_FoliageTree`, `MEVT_FoliageColor`, `SHOW_Foliage`,
  `m_canUseOnFoliageMeshes`
* `CMeshLOD`, `CMeshLODLevelBaseMesh`, `CMeshLODLevelExplicitMesh`,
  `CMeshLODLevelCustomMesh`, `CMeshLODLevelHide`, `ChangeForcedLOD`,
  `CRenderCommand_ChangeSceneForcedLOD`
* distance properties: `m_maxRenderDistance`, `m_lodDegenerationStartDistance`,
  `m_autoHideDistance`, `m_autoHideDistScale`, `m_autoCloseDistance`,
  `m_autoDisableDistance`, `m_billboardHideDistance`, `m_billboardShowDistance`,
  `m_billboardSourceLOD`, `m_startDistanceFactor`, `m_occlusionDistance`,
  `m_shadowRenderingDistance`, `m_overrideMeshShadowRenderingDistance`,
  `m_shadowDistanceScale`, `m_forceLODLevel`, `m_forceBehaviorLod`
* `CRenderVisibilityQuery` / `IRenderVisibilityQuery`, `EVisibilityTest`,
  `EFarPlaneDistance`, `ENearPlaneDistance`, `EShadowRenderingDistance`

So there are three independent places pop-in can come from:

1. the global LOD multipliers (`MeshDistanceScale`, `FoliageDistanceScale`);
2. per-mesh/per-entity render distances baked into the data files — this is what
   the Nexus mod *Extreme Vegetation Rendering Distance* attacks by replacing 296
   `.w2mesh` files;
3. the visibility/culling path (`CRenderVisibilityQuery`, `EVisibilityTest`), which
   would need a code patch rather than data.

## 4. Open questions

* Is `FoliageDistanceScale` / `MeshDistanceScale` clamped, and to what range?
  (Decides whether ini values >1.0 do anything, or whether a code patch is needed.)
* Which code path applies `FoliageDistanceScale` — the foliage container build, or
  the per-instance visibility test?
* Is the real pop-in source the culling distance for foliage *instances*, or the
  per-mesh LOD switch distance? Compare by raising one knob at a time.
* Does the engine read additional undocumented keys from `[Rendering]` that are
  not present in any shipped ini? Enumerate the config-registration function.

## 5. Where the settings are registered (analysis 2026-10-28, analysis complete: 39 339 functions)

**Gotcha first:** the section headers store `VirtualAddress` *relative to the image
base*. When mapping a raw file offset to a VA by hand, the formula is
`VA = ImageBase (0x00400000) + SectionVA + (fileOffset - RawPtr)`. Forgetting the
image base makes every computed address off by exactly `0x400000` — and the wrong
addresses still "work" as pointers into `.text`, producing plausible nonsense.

Corrected wide-string VAs (verified against Ghidra's own string search):

| Key | VA |
|---|---|
| `MeshDistanceScale` | `0x00E44A60` |
| `FoliageDistanceScale` | `0x00E44A20` |
| `CharacterLodGameplayDownscale` | `0x00E44BC8` |
| `MeshLodGameplayDownscale` | `0x00E44B80` |
| `TextureMemoryBudget` | `0x00E44978` |
| `MaxResidentMipMap` | `0x00E44788` |
| `AllowBillboards` | `0x00E449EC` |

Each key string has exactly **one** pointer reference in the whole image, and every
one of them sits inside a single function: **`FUN_00b07714`**. It is not a data
table — it is straight-line code that registers settings one by one:

```
push  "FoliageDistanceScale"      ; 0x00B07F76
lea   edi, [ebp-0x1c]             ; CName temp, zeroed
call  0x004020E3                  ; CName::CName(const wchar_t*)
push  "Rendering"                 ; 0x00E44A4C
...
mov   esi, [ebp+8]                ; this  (settings object)
lea   eax, [esi+0x68]             ; &this->field_0x68  <-- where the value lives
push  eax
push  key CName
push  section CName
push  [ebp-0xc]
call  0x0041D0FB                  ; register helper A (float-ish)
```

Same shape for `MeshLodGameplayDownscale` at `0x00B08171`, except it stores into
`this+0x34` and calls helper **`0x004024E3`** (helper B) instead. Two different
helpers for two different value types is the useful part: it means
`MeshLodGameplayDownscale` is not the same kind of setting as `FoliageDistanceScale`.

Confirmed: both are registered under section `Rendering` (`0x00E44A4C` and
`0x00E44BB4` both read "Rendering").

### Where to go next

1. Decompile `0x0041D0FB` and `0x004024E3` — the register helpers should reveal the
   value type, whether a default/min/max is attached, and whether they clamp.
2. `FUN_00b07714` is the constructor/initialiser of the settings object; its `this`
   is the runtime object holding `+0x68` (foliage scale) and `+0x34` (mesh gameplay
   downscale). A runtime patch can set those floats directly, independent of the ini.
3. Check whether the values are validated when the ini is parsed, or only when used.

## 6. The settings object

The whole rendering settings block is one heap object, held in a singleton:

| Item | Address |
|---|---|
| Singleton pointer (`DAT_02580e28`) | `0x02580E28` (.data) |
| Getter (lazy init) | `FUN_00402254` |
| Constructor | `FUN_0040227c` |
| Config loader (reads every key into the object) | `FUN_00b07714` (`0x00B07714`–`0x00B08831`) |

Field map, all confirmed from the loader's code:

| Offset | Setting | Type | Section |
|---|---|---|---|
| `+0x34` | `MeshLodGameplayDownscale` | bool | Rendering |
| `+0x38` | `CharacterLodGameplayDownscale` | bool | Rendering |
| `+0x68` | `FoliageDistanceScale` | float | Rendering |
| `+0x6c` | `MeshDistanceScale` | float | Rendering |

The loader uses two register helpers — `0x0041D0FB` (float path, via `FUN_0040861e`
→ `FUN_00ac23b9`) and `0x004024E3` (bool path, via `FUN_00402600` → `FUN_00ac247d`).
**Neither helper clamps or validates.** They are generic "get this key's value as
float/bool" accessors. So there is no evidence of a range check between the ini and
the in-memory field: whatever the ini says is what the renderer gets.

What is still missing is the *consumer* side: which renderer function multiplies
`[settings+0x6c]` into a mesh LOD distance, and whether the character path reads the
same field. That is the next RE target.

## 6. Pitfall that cost a launch: COM signatures and CFG

The first version of the keyboard filter hooked `IDirectInput8::CreateDevice` with this
signature:

```c
typedef HRESULT (WINAPI *CD_t)(void* self, const GUID* rguid, void* outer, const IID* riid, void** out);
```

That is five parameters. The real method has **three plus `this`**:

```c
HRESULT CreateDevice(REFGUID rguid, LPDIRECTINPUTDEVICE8 *lplpDevice, LPUNKNOWN pUnkOuter);
```

The extra `riid` shifted every argument by one, so `ppvOut` was read from the caller's
`pUnkOuter` slot (NULL or garbage) and the hook wrote a patched "vtable pointer" into
arbitrary memory. The game then died within the same millisecond the hook was
installed, with **no WER report and no Event Log entry** - consistent with a silent
fast-fail, which is also what a Control Flow Guard violation looks like.

Symptom to remember: a process that vanishes at a precise log line, leaving no crash
record, is usually either CFG or a memory write to a bad pointer.

`tools/inputprobe/` is the answer to that class of bug: a console program built with
`/guard:cf` that loads the proxy DLL and drives DirectInput exactly as the game does
(`DirectInput8Create` → `CreateDevice` → `SetDataFormat` → `Acquire` →
`GetDeviceState` in a loop). Against the fixed DLL it runs clean, which is how the fix
was verified without launching the game. Note the probe cannot verify the *pass-through*
case: DirectInput only reports keys for the focused window, so key injection from a
background console reads all-zero - the filter branch is exercised, the baseline is not.

## 7. Method notes

* Analysis copy lives in `tools/ghidra-inputs/`, never in the game folder.
* Do not trust a Ghidra "auto analyzed: true" reply — check the function count.
  A packed binary shows ~400 stub functions; the unpacked one reaches tens of
  thousands.

## 8. Experimental renderer distance controls (2026-10-28)

`FUN_009bade3` has separate mesh visibility and shadow-caster distance gates.
An opt-in experiment now scales their squared thresholds at VA 009BB09F and
009BB465. It does not alter LOD tables or prove that either gate causes the tent
NPC's disappearance. Scope is all mesh proxies, not verified character-only.
Exact bytes, native thread barrier, 27-check probe, new Ctrl+Shift+F9/F10 controls
and rollback are in [render-distance-experiment](fixes/render-distance-experiment.md).

## 9. Game data formats used by the script patches (2026-10-30)

* **DZIP** (`*.dzip`): Gibbed.RED layout. 32-byte header (`DZIP`, version 2, count, `gbbd`,
  entry table offset, entry table hash), entries of 64 KiB LZF blocks behind an int32 block
  offset table, entry table at the end. The header hash is FNV-1a 64 prime with seed
  `0xFFFFFFFF` over each entry's name bytes, name length, FILETIME, sizes and offset; it matches
  vanilla and Mersey `base_scripts.dzip` and `elf_flotsam.dzip`. Rewriting one entry and keeping
  the others' compressed bytes is enough for the game (`tools/script-patches/ScriptPatches.ps1`, git tag `v0.5.1`).
* **Scripts are compiled from `CookedPC\base_scripts.dzip` at every start.**
  `compiledscripts.w2scripts` dates from the install and lacks Mersey's identifiers, yet Mersey
  works; our patched archives compiled in game. A compile error stops the game with
  "Script compilation error ... guishop.ws[375]: parse error ...".
* **WitcherScript keywords** (UTF-16 strings next to `virtual_parent`, unpacked file offset
  `0xA781EC`): abstract array bool break brix byte case class cleanup const continue def default
  defaults delete do editable else enter entry enum event exec extends final float for function
  if import in inlined int latent local name new NULL optional out parent private public quest
  return saved savepoint state storyscene string struct super switch this timer var
  virtual_parent void while, plus theGame theHud thePlayer theCamera theServer theSound. Never
  name a variable `entry` or `state`.
* **`StrChar( i : int )`** (native `008E5A0A`) returns a one-character string from a UTF-16
  unit (clamped to >= 0x20): the safe way to put non-ASCII text in script.
* **`.w2strings`** (version 201): uint32 version, uint16 key hi, key table (plain names in
  lower case, e.g. `menucontinue`, uint32 index ^ magic), uint16 key lo, uint32 hash ^ magic,
  then texts: uint32 index ^ magic, length-prefixed UTF-16 XORed per unit with
  `((len + 1) * key) & 0xFFFF`, key = `(magic >> 8) & 0xFFFF` rotated left by one per unit.
  Magic per file key from Gibbed.RED's table; `tr0` is not in it (its 16-bit text key can be
  found by trying all). Decoder: `tools/script-patches/check_texts.py`. Languages with full
  texts here: EN PL DE FR ES IT RU CZ HU TR JP ZH (Traditional); `br_user`/`kr_user` hold English
  placeholders. `theGame.GetGameLanguageName( audio, text )` returns these codes.

## 10. Window, video mode and the in-game options (2026-11-02)

Verified for the borderless feature (`docs/fixes/borderless-window.md`).

* **The window** is created in `FUN_009d44e1` (`0x009D44E1`), class **`W2ViewportClass`**, which
  is also how the mod finds it (`FindWindowW`). Windowed: style `0x00C80000` (caption + system
  menu), exstyle `0x40000` (WS_EX_APPWINDOW), centered, client area = the configured resolution
  (`0x009D45C3` / **`0x009D45CA`**, then `AdjustWindowRect` at `0x009D45E1`). Full screen:
  `WS_POPUP` (`0x80000000`), exstyle `0x8` (WS_EX_TOPMOST), at 0,0 (`0x009D4562` /
  `0x009D4569`). `CreateWindowExW` adds `WS_CLIPCHILDREN` (`0x009D4681`, IAT slot `0x00CAD338`).
* **The mode setting**: `[Rendering] Fullscreen` (User.ini) is read by `FUN_00b07714` into the
  settings object at **`+0x10`** (store `0x00B07A49`; `VSync` `+0x11`). It selects the window
  branch above and `D3DPRESENT_PARAMETERS.Windowed` in `FUN_0052ccb0`
  (`local_a20 = (param_4 == 0)`). There is **no runtime mode switch**: the only path is the
  viewport-init vtable method `FUN_009e124a` (slot `0x00EC6114`) -> `FUN_009de288` ->
  `FUN_0052ccb0`. A live change therefore needs a restart; only the window *style* is live
  (restyle with `SetWindowLongW`/`SetWindowPos`).
* **SteamStub and code patches**: `.text` is encrypted until the stub jumps to the OEP
  `0x0066F59D`, which happens after every static DLL's `DllMain`. A `.text` patch installed from
  `DllMain` reads encrypted bytes; poll for the site's fingerprint instead (the IAT, by contrast,
  is already filled at `DllMain`, see section 6).
* **DPI**: `witcher2.exe` has no manifest (`<assembly`/`dpiAware` absent in both the packed and
  the unpacked file) and no `SetProcessDPIAware` code, so the process is DPI-unaware. On a scaled
  desktop a windowed client is virtualized (4K at 150 % becomes 5760x3240 physical). A DLL can
  still set awareness from an early thread, before the game creates its window
  (`SetProcessDpiAwarenessContext` / `SetProcessDPIAware`).
* **The in-game options are scripted**: `game\gui\guimainmenu.ws` has `FillOptionsGraphics` /
  `InputOptionsGraphics` (brightness and gamma only; mode, resolution and VSync are launcher
  settings, labelled in `bin\config\lang\EN.ini`). `InsertMenuOnOff( items, id, label, state )`
  and ids outside `EMenuButtons` (the mod uses `9001` for Continue) are the pattern for a new
  toggle.
* **Script settings**: `theGame.ReadConfigParamFloat(category, section, key, out value)` and
  `WriteConfigParamFloat(category, section, key, value)`; the writer native `FUN_009B503A` calls
  `FUN_0041F4B7( const wchar_t* category, const wchar_t* section, const wchar_t* key,
  const float* value )` - **`__stdcall`**: it ends in `RET 0x10` and the caller (`0x009B50EA`,
  continuing at `0x009B50EF`) does not clean the arguments. A detour must use `__stdcall`; a
  `__cdecl` hook removes the arguments a second time and returns into the stack (crash of the
  first 0.7.0 test, 2026-11-02, see `docs/validation/borderless-first-test-2026-11-02.md`).
  Prologue `55 8B EC 83 E4 F8 A1 50 86 60 01`. That entry is the live channel from the patched
  menu to the DLL. The writer marks the config dirty; the engine writes User.ini later, so polling
  the file is not a substitute.
* **Peer-stop barrier at startup** (`src/patch_barrier.h`, shared): the freeze-to-a-fixed-point
  pass can fail while the process is still creating threads (new thread in every pass, or a thread
  exiting during enumeration). It succeeded 3 ms later in the 2026-11-02 log, so retry the install
  briefly instead of giving up.
* `FUN_009d24f8` is the mouse-mode function (0 = free, 1 = `ClipCursor` to the window rect,
  2/3 = capture/exclusive DirectInput); it is not tied to full screen, so cursor clipping already
  works in windowed/borderless mode.

