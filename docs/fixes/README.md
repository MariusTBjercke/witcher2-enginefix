# Fixes and research index

One entry per runtime change or investigation. "Shipped" means the feature is in a release or the
current version; the entry itself has the proof.

## Save games

| Entry | What it covers | State | Off switch |
|---|---|---|---|
| [save-previews.md](save-previews.md) | Missing Load menu screenshots, capture/save race | Shipped | `[saves] fix_thumbnails` |
| [save-bloat.md](save-bloat.md) | Save list leak, fast list, many saves | Shipped, research continues | `[saves] fix_list_leak`, `fast_list` |

## Loading and doors

| Entry | What it covers | State | Off switch |
|---|---|---|---|
| [interior-preload.md](interior-preload.md) | Seamless interiors, partitions, occluders | Shipped | `[loading] interiors` |
| [load-times.md](load-times.md) | Measured load times, faster door fades | Shipped | `[loading] fast_doors`, `door_fade` |
| [interior-maps.md](interior-maps.md) | Black map in interiors (data files) | Shipped | Delete the map files |

## Distance and graphics

| Entry | What it covers | State | Off switch |
|---|---|---|---|
| [graphics-presets.md](graphics-presets.md) | The four draw distance levels, Ctrl+F10, the menu row | Shipped | `[presets]` |
| [lod-distance.md](lod-distance.md) | First pop-in work, LOD multipliers | Background | |
| [render-distance-experiment.md](render-distance-experiment.md) | Visibility and mesh shadow hooks | Shipped inside the presets | `[preset.*]` |
| [npc-streaming-sun-shadows.md](npc-streaming-sun-shadows.md) | NPC disappearance, sun shadow quality | Open research | |
| [roadmap-shadows-characters-menus.md](roadmap-shadows-characters-menus.md) | Older roadmap for shadows and characters | Background | |
| [modern-hardware-options.md](modern-hardware-options.md) | Tuning candidates, shadow keys, frame pacing | Open research | |
| [quality-audit-2026-10-28.md](quality-audit-2026-10-28.md) | Audit of quality, characters and saves | Background | |

## Scripts, UI and input

| Entry | What it covers | State | Off switch |
|---|---|---|---|
| [dll-script-patches.md](dll-script-patches.md) | How the DLL patches scripts and UI movies, file hooks | Shipped | `[scriptpatches]` |
| [shop-book-read-marker.md](shop-book-read-marker.md) | Book read status, helpers in their own script | Shipped | `book_marker` |
| [menu-buttons.md](menu-buttons.md) | Continue and pause menu Quit | Shipped | `continue`, `pause_quit` |
| [medallion.md](medallion.md) | Medallion on the move | In 0.9.0 | `medallion` |
| [borderless-window.md](borderless-window.md) | Display mode row, borderless, video mode | Shipped | `window_mode`, `[window]` |
| [windows-keys.md](windows-keys.md) | Snipping Tool and Windows keys | Shipped | `[hotkeys] windows_keys` |
| [skip-intro.md](skip-intro.md) | Skipping the startup movies | Shipped, off by default | `[startup] skip_intro` |

## Stability and platforms

| Entry | What it covers | State | Off switch |
|---|---|---|---|
| [crash-capture.md](crash-capture.md) | Crash log and dump, silent exit | Shipped | `[diagnostics] crash_dump` |
| [crash-raycast-stale-body.md](crash-raycast-stale-body.md) | Crash leaving the Flotsam inn | Shipped | `[stability] raycast_guard` |
| [quit-crash.md](quit-crash.md) | Crash while quitting with an area loaded (`FactsAdd`) | In 0.9.0 | `[stability] quit_guard` |
| [cpu32-hang.md](cpu32-hang.md) | No window on 32 or more threads | In 0.9.0, not run on such a CPU | `[stability] cpu32_guard` |
| [gog-build.md](gog-build.md) | How GOG differs and what a port needs | Research | |
