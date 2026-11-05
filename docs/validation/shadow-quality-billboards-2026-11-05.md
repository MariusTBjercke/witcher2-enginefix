# Shadow quality and plant billboards, 2026-11-05

Steam 3.5.0.26 with DXVK, 4K, the owner's own ini. The Nexus mod Extreme Vegetation Rendering Distance was
installed as loose files. The builds were the working tree just before the 0.11.0 version bump.

## Shadow quality

The owner stood at the forest spot where the line shows and switched the Options > Graphics row. Owner: on Vanilla the
line is back, on High and Ultra it is gone, and it generally got much better. Log of one session:

```
17:46:41.025 shadows: vanilla, zones 7 / 24 / 64 m, atlas 1024, filter 1.00 (options menu)
17:46:43.319 shadows: atlas switched to 1024 per cascade
17:47:14.206 shadows: ultra, zones 12 / 24 / 64 m, atlas 4096, filter 0.50 (options menu)
17:47:16.979 shadows: atlas switched to 4096 per cascade
17:47:31.698 shadows: vanilla, zones 7 / 24 / 64 m, atlas 1024, filter 1.00 (options menu)
17:47:34.350 shadows: atlas switched to 1024 per cascade
17:48:29.246 shadows: ultra, zones 12 / 24 / 64 m, atlas 4096, filter 0.50 (options menu)
17:48:31.900 shadows: atlas switched to 4096 per cascade
17:49:10.547 exit: ExitProcess(0) from witcher2.EXE+0x272388 on thread 26432 (the main thread)
17:49:10.607 unloaded
```

## Plant billboards

The owner compared the same plant on Extreme and Vanilla, loading a save after each switch. Owner: better on Extreme,
and a reload is needed to compare. Log of one session:

```
18:05:28.318 preset: extreme at startup
18:05:28.318 billboards: hook installed, factor 3.00x
18:06:08.864 billboards: 3101 new proxies, 3101 moved; last 10.0 m -> 30.0 m (3.00x)
18:08:06.101 preset: vanilla mesh=1.00 foliage=1.00 visibility=1.00x shadow=1.00x billboards=1.00x (options menu)
18:08:25.645 billboards: 189 new proxies, 0 moved; last 10.0 m -> 10.0 m (1.00x)
18:08:40.962 preset: extreme mesh=8.00 foliage=5.00 visibility=3.00x shadow=3.00x billboards=3.00x (options menu)
18:08:45.906 billboards: 2954 new proxies, 2954 moved; last 10.0 m -> 30.0 m (3.00x)
18:09:08.055 exit: ExitProcess(0) from witcher2.EXE+0x272388 on thread 48596 (the main thread)
18:09:08.115 unloaded
```

No new crash dump from either session.

## GOG

GOG 3.5.0.26g with DXVK, no loose meshes in `CookedPC`, an ini from before Shadow quality, the release DLL. `User.ini` is shared with Steam and held Ultra. Owner: seemed to work fine.

```
18:19:15.826 shadows: level ultra; atlas at start 4096, filter on, live atlas on
18:19:17.310 billboards: hook installed, factor 3.00x
18:19:17.310 shadows: zones patched at 006411CD, 006411FD and 0064120D
18:19:20.094 shadows: atlas 4096 per cascade, wanted 4096
18:19:32.498 billboards: 16359 new proxies, 16359 moved; last 8.0 m -> 24.0 m (3.00x)
18:20:15.439 shadows: vanilla, zones 7 / 24 / 64 m, atlas 1024, filter 1.00 (hotkey)
18:20:17.010 shadows: high, zones 12 / 24 / 64 m, atlas 2048, filter 0.60 (hotkey)
18:20:18.095 shadows: ultra, zones 12 / 24 / 64 m, atlas 4096, filter 0.50 (hotkey)
18:20:18.817 shadows: atlas switch refused, stays 1024 per cascade
18:20:18.938 shadows: vanilla, zones 7 / 24 / 64 m, atlas 1024, filter 1.00 (hotkey)
18:20:21.708 shadows: high, zones 12 / 24 / 64 m, atlas 2048, filter 0.60 (hotkey)
18:20:25.083 shadows: atlas switched to 2048 per cascade
18:20:26.406 shadows: ultra, zones 12 / 24 / 64 m, atlas 4096, filter 0.50 (hotkey)
18:20:29.778 shadows: atlas switched to 4096 per cascade
18:20:30.982 shadows: vanilla, zones 7 / 24 / 64 m, atlas 1024, filter 1.00 (hotkey)
18:20:34.356 shadows: atlas switched to 1024 per cascade
18:21:06.971 shadows: ultra, zones 12 / 24 / 64 m, atlas 4096, filter 0.50 (options menu)
18:21:10.460 shadows: atlas switched to 4096 per cascade
18:21:46.752 exit: CorExitProcess(0) from witcher2.EXE+0x343F48 on thread 22276 (the main thread)
```

The `refused` line is the defect in `docs/fixes/npc-streaming-sun-shadows.md`, fixed after this run: the hotkey was pressed while a switch was running.

## Not covered

- Frame cost: no measurement, and the owner did not report on it.
- The Shadow quality hotkey on Steam (seen on GOG).
- The fix for a level picked during a switch: `distanceprobe` only.
- Native Direct3D 9 with a larger shadow map.
- Which copy of the plant files the game loads with that mod installed. The hook logs only the last proxy.
