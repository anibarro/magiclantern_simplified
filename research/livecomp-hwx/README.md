# Live Composite hardware-merge experiments (not built)

These are the research sources behind `modules/livecomp/HARDWARE-MERGE.md`.
They are **not** part of the Live Composite module and are not compiled by
any makefile. They call undocumented Canon ROM routines of the EOS M 2.0.2,
fingerprinted before use. Running them can leave a DMA channel stuck (reboot)
or, in the variants that touched Live View's channel 5 (removed), crash the
camera. They are kept for reference and for anyone who continues the
investigation.

| File | Content |
| --- | --- |
| `native-max.c`, `native-max.h` | Wrapper for Canon's image max job (setup FF429E68, start/stop ENGIO tables, non-waiting resource lock, DMA ownership checks, channel-5 borrow from LCM29). |
| `resource-snapshot.h` | Bounded read-only walk of Canon's resource registry (manager 0x4C3F8). |
| `hwx-res.h` | Registry walk plus non-blocking hold/release of resource sets. |
| `hwx-step1.h` | LCX1: pause Live View in full-res x5 and test whether sensor frames still arrive (T0-T4). Log `LVHWXD.LOG`. |
| `hwx-engine.h` | LCX2: one engine job while Live View runs at 3 fps, with millisecond tracing of the DMA pointers. Log `LVHWXE.LOG`. |
| `lv-pause-regs.h` | The 189 image-pipeline registers whose value changes when Live View pauses (from the LCM32 camera log). |
| `lcx2-integration.diff` | Changes to `fullres-lv.c`, `fullres-lv.h`, `livecomp-lv.c`, `livecomp-config.h` and the `Makefile` that build the LCX2 test module from LCM52. |

To rebuild LCX2: start from the LCM52 sources, copy these files into
`modules/livecomp/`, apply `lcx2-integration.diff` (`git apply -p1` from the
repository root), and build as usual. The menu gets "HW test: OFF / Paused
readout / Engine in LV". Camera logs of both runs are in
`tests/livecomp/LCX1-camera` and `tests/livecomp/LCX2-camera`.
