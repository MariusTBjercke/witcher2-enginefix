# Medallion on the move, 2026-11-03

Steam Enhanced Edition 3.5.0.26 with Project Mersey, build of 2026-11-03 before the 0.9.0 version
bump (same code, the log still says 0.8.1).

Owner's verdict after playing: works well.

From `witcher2_enginefix.log`:

```text
scriptpatches: book_marker=1 continue=1 pause_quit=1 window_mode=1 medallion=1
script overlay: medallion=on
script patches: book_marker=on continue=on pause_quit=on window_mode=on medallion=on cache=built 93 ms
```

The game started, so the patched scripts compiled.

Offline: `build\sptest.exe test --game <game>` 0 failures; `script_real` markers 2 1 10 6 2 1 1.

Not tested on vanilla scripts in game; there the anchors were only checked by search (1 match each).
