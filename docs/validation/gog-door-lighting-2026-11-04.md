# Door lighting, the hidden spinner and the memory doors on GOG, 2026-11-04

GOG 3.5.0.26g, vanilla scripts plus A Better UI, 4K, the shipped ini with `skip_intro=1`. The
working build with the GOG port of `door_light_blend` and the hidden spinner (`gog-build.md`,
"Ported addresses"), on top of `e199dc0`. There were four runs by the owner in Flotsam: the houses
by the quay, the inn and its harbour door, the clerk's house and Einar Gausel's house.

## Run 1 (17:03-17:09, native D3D9): lighting and spinner work, a memory door shows see-through

```text
doors: environment activation hooked, door Stabilize hooked - door_light_blend on (1.00 s)
doors: loading spinner command hooked - no spinner on instant doors
doors: 'Outdoor\global\landing_site\outside\Flotsam' -> 'Outdoor\global\landing_site\outside\08_bar_craftsmen' is loaded - no fade (0 to load)
doors: lighting blends over 1.00 s instead of switching at once - 'taves_craftsmen' 15 at 0.00 (+1.00/s)
doors: 'Outdoor\global\landing_site\outside\Flotsam' -> 'Outdoor\global\landing_site\outside\inn_interior' is loaded - no fade (7 to load, spinner hidden)
doors: lighting blends over 1.00 s instead of switching at once - 'house_01_inn' 15 at 0.00 (+1.00/s)
doors: 'Outdoor\global\landing_site\outside\inn_interior' -> 'Outdoor\global\landing_site\outside\Flotsam' with only 930 MB of address space free - the game's door (the outdoors unloads), 0.25 s fades
doors: interior ready after 172 ms - picture back now (state 2)
```

Owner: good at the houses by the quay. At the inn's harbour exit the town in front of Geralt was
see-through until he was out; after that, the doors near the inn faded to black. The memory guard
had made that exit the game's own door. Its job loaded the town's occluders at once, and
`fast_doors` brought the picture back after 172 ms, while the camera was still inside the
inn's occluder block. That is the 0.8.1 bug again.

## Run 2 (17:16-17:19, native D3D9): the game's timing on memory doors

The first fix: a memory door's picture waits for the door animation's `fade_out` event. There was
no see-through, and no `picture back now` after any of the five memory doors (955-1011 MB free).
Owner: the fade was clearly longer than 0.25 s. They wanted the short fade wherever a fade is
needed.

## Run 3 (17:24-17:28, native D3D9): short fade on memory doors, occluders wait

The second fix: on a memory door the game's job still unloads the outdoors and runs its GC, but
its occluder layers wait as on a kept door. So the picture comes back as soon as the job is done:

```text
doors: 'Outdoor\global\landing_site\outside\Flotsam' -> 'Outdoor\global\landing_site\outside\inn_interior' with only 991 MB of address space free - the game's door (the outdoors unloads), 0.25 s fades
partition: memory door kept 0 of 114 layers, unloaded 114; loaded 12 (0 interiors added), skipped 0 (fake rooms, low-poly), occluders wait until Geralt is through the door
doors: interior ready after 16 ms - picture back now (state 2)
doors: Geralt is through the door - loading the occluders (3735 ms after the door)
doors: 'Outdoor\global\landing_site\outside\inn_interior' -> 'Outdoor\global\landing_site\outside\Flotsam' with only 1015 MB of address space free - the game's door (the outdoors unloads), 0.25 s fades
partition: memory door kept 0 of 19 layers, unloaded 19; loaded 56 (0 interiors added), skipped 0 (fake rooms, low-poly), occluders wait until Geralt is through the door
doors: interior ready after 0 ms - picture back now (state 2)
```

Owner: short fades, nothing see-through. They also saw the loading spinner now and then, going in
or out of houses away from the centre. The door's own spinner was hidden. The one they saw came
from the DLL's occluder switch, made once Geralt is 5 m from the door (3-4 s after it started),
which was past the 2 s window. That switch's spinner is now hidden too (`g_koOccTick`).

## Run 4 (17:33-17:36, DXVK 3.1.1 with `dxvk.enableDescriptorHeap = False`)

```text
preload: 62 of 62 preloaded layers loaded, 0 left for their first visit (free address space at the lowest 2334 MB, floor 1280 MB)
savelist: after the switch private=1078 MB commit=1481 reserve=280 free=2333 largest_free=2036 MB, previews held=0
```

| Doors | No fade | Memory doors | Lighting blend lines |
|---|---|---|---|
| 6 (the inn both ways, houses) | 6 | 0 | 12 |

Owner: "it went very well". There was no dump in any run. Each run ended with
`exit: CorExitProcess(0) from witcher2.EXE+0x343F48`.

With native D3D9 at 4K the free address space fell to about 930-1015 MB around the inn, so most
doors there used the memory path. With DXVK it stayed above 2 GB, and every door went without a
fade.
