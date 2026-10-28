# livecomp speed benchmark (LCB1-LCB3, not built)

`speed-bench.h` was included at the end of `modules/livecomp/fullres-lv.c`
(camera build only) with a "Speed benchmark" menu item on the branch
`livecomp-speed`. With Live View running (x1) and the session's two 32 MB
buffers it measured memory reads/writes, the C and ARM select-max kernels on
four data sets, the Live Bulb add and the preview copy (memcpy / EDMAC), into
`DCIM/LIVE/LVBENCH.LOG`. Results: FULLRES.md (LCM64) and
`tests/livecomp/LCB1-camera` .. `LCB3-camera`.

To use it again: copy it next to `fullres-lv.c`, include it there under
`#ifdef __ARM__`, declare `unsigned livecomp_bench_run(struct card_info *card,
const char **status);` in `fullres-lv.h` and restore the menu item and task
from commit f40d63b34 (`livecomp-lv.c`).
