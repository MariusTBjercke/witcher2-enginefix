# Steam regression run of c0b0652, 2026-11-04

Steam 3.5.0.26 with DXVK 3.1.1 (`dxvk.enableDescriptorHeap = False`), the owner's own ini, 4K. The
DLL built from `c0b0652` ("GOG: saves, door lighting and the hidden spinner"). That commit
touched these Steam paths:
- `light_install`'s fingerprints now start at -9, and `patch_rel32` does the patching;
- memory doors defer their occluders;
- the DLL's occluder switch hides the spinner;
- a preview pairs with one save only;
- `lazy_prepare` is shared with GOG.

The owner loaded a save in Flotsam and went through the inn and a couple of houses. They made a
manual save and a quicksave, opened the Load menu and quit through the pause menu. Owner: it
seemed to work fine. No new dump.

```text
build: Steam 3.5.0.26 (TimeDateStamp 518B881C, .text 008A94AD), delta 0x00000000 from 0x00400000; import slots 27/27 found by name
thumbnail: menu image registry hooked (QuickSave preview refresh)
savelist: startup call hooked, menu call hooked; leak fix=1 fast_list=1 trace=0
saves: ISteamRemoteStorage 13000BE8 FileWrite hooked; thumbnail fix=1
doors: environment activation hooked, door Stabilize hooked - door_light_blend on (1.00 s)
doors: loading spinner command hooked - no spinner on instant doors
partition: keep_outdoors kept 70 of 77 layers, unloaded 7; loaded 3 (0 interiors added), skipped 1 (fake rooms, low-poly), occluders wait until Geralt is through the door
thumbnail: menu reloads QuickSave_640x360.bmp
thumbnail: menu reloads ManualSave_0170_640x360.bmp
exit: ExitProcess(0) from witcher2.EXE+0x272388 on thread 6364 (the main thread)
unloaded
```

There were 3 doors without a fade, 6 lighting-blend lines, no fades and no memory doors (with DXVK
there was enough address space).
