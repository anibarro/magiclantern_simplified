# How Live Composite works (EOS M 2.0.2)

Live Composite emulates Olympus Live Composite: the camera exposes frame
after frame at a fixed shutter, and every frame is merged into a composite
that keeps the brightest value of each pixel. Static parts of the scene keep
the exposure of a single frame, while moving lights (car trails, light
painting, stars) accumulate. The result is one RAW DNG at full sensor
resolution.

Usage is in INSTALL.txt. This document explains the implementation. The
development history is in FULLRES.md, and the failed hardware-merge work in
HARDWARE-MERGE.md.

## 1. Overview

```
SET (Movie Live View, x1)
 |- checks: model/firmware fingerprints, camera state, fixed ISO, no other RAW user
 |- allocate 2 x 32.5 MB (capture buffer + accumulator); create DCIM/LIVE
 |- install 4 hooks (inert unless this session is active AND Canon is in x5)
 |- request x5 zoom  ->  Canon reprograms the sensor through the hooks:
 |                        full-sensor readout at 3 fps
 |- switch the frame timers to the chosen shutter at a vsync
 |- loop: arm RAW DMA -> frame done -> merge into accumulator -> LCD preview
 |        (the next exposure is already running while merging)
 |- SET / frame limit / low battery: finish the frame in flight
 |- back to 3 fps, leave x5, remove hooks
 '- fix focus pixels, write DCIM/LIVE/LVFRnnnn.DNG (+ LOG/CSV with Debug ON)
```

Files:

| File | Role |
| --- | --- |
| `livecomp.c` | module entry, camera guard, raw key adapter for EOS M's combined Q/SET |
| `livecomp-lv.c` | menu, start/stop logic, Global Draw handling, worker task |
| `fullres-lv.c` | the session: hooks, x5 switch, timers, RAW DMA, merge, preview, DNG, LOG |
| `fullres-regs.h` | sensor register values and the shutter (timer) table; pure, host-tested |
| `raw14-max.h` | merge kernel on packed 14-bit RAW |
| `fullres-fix.h` | focus-pixel fix and DNG white balance |
| `fullres-gamma.h` | preview gamma table |
| `lv-dng.h` | DNG header writer |
| `camera-profile.h` | runtime selection of camera-specific constants (EOS M 2.0.2 only) |

## 2. Full-sensor readout

The EOS M has no full-resolution Live View. The module uses the
`CROP_Full_Res` preset from Bilal Fakhouri's crop_rec (EOS M,
`CROP_PRESET_1X1`, "5208x3478 @ 3 FPS"): Canon's x5 zoom mode, with the
sensor, ADTG and timing-generator settings changed so that the whole sensor
is read out.

Canon applies sensor settings through four routines. While a session runs,
they are hooked with ML's `patch_hook_function`:

| Hook | Address | What is replaced |
| --- | --- | --- |
| `cmos_write` (RAM copy) | 0x2998C | CMOS[5]=0, CMOS[7]=0x3A0 |
| `adtg_write` (RAM copy) | 0x2986C | readout and power-save timing, shutter blanking, scaled to timer B |
| `engio_write` (ROM) | 0xFF2C19AC | RAW window 0xC0F06804 = (0xDB2<<16)\|0x538, auxiliary H/V registers, timers A/B, preview window |
| `EngDrvOut` (ROM) | 0xFF2C1694 | the same values when Canon writes them one at a time (optional hook) |

The hooks act only when the session is active **and** Canon's PathDriveMode
(RAM 0x892E8) reports x5. Each hooked instruction is checked against the
expected original before patching, and the camera is refused on any
mismatch. The values come from crop_rec; the parts of the preset that write
Canon-owned memory (EDMAC #24 redirect, EDMAC #9 patches, HIV hook) are
deliberately left out, because RAW capture does not need them.

Result: a 5280 x 3506 raster (5208 x 3478 active), packed 14-bit, pitch
9240 bytes, 32,395,440 bytes per frame.

## 3. Shutter = frame period

The frame period is set by the sensor timing generator:

```
period = (timer_A + 1) * (timer_B + 1) / 32 MHz
```

Every row integrates for the whole period (the shutter blanking is set to the
minimum), so each frame is a 100 % exposure and consecutive frames have no gap.
Timer B is 16 bits wide, so long shutters also stretch timer A in multiples of
the reference value 1388 (factor k). The readout then lasts about
150 ms x k at the end of each period. The table in `fullres-regs.h` has 15
shutters from 2.5 s (k=1) to 60 s (k=22).

**Fast start.** Entering x5 directly with a long timer would make Canon's
first, transitional frame last a whole shutter time, and the first usable
frame would arrive after two shutter times (up to two minutes at 60 s). The
session therefore enters x5 at the reference 3 fps (333 ms). Once the
sensor window is verified, it switches to the chosen timers at a vsync: the
vsync callback adopts the new pair, and the timer-reapply code writes
0xC0F06008/0xC0F06014/0xC0F06000, the same as ML's FPS override does. The camera
latches the new timers one frame late, so the first frame after the switch
may still be a short 3 fps frame. It is detected from the vsync intervals and
skipped. On stop, the timers go back to 3 fps once the last frame is in
flight, so leaving x5 does not wait for another long frame.

## 4. Capture

- The module does not use the core's LV RAW buffer, which on the EOS M
  cannot hold a full-sensor frame. It starts its own RAW DMA: EDMAC channel
  0x12, source 0, with the RAW type register 0xC0F37014 = 0x10, the same
  steps as the core's `edmac_raw_slurp`. The target is its own 32.5 MB buffer
  with 64-byte guard areas before and after the frame.
- Each transfer is armed from the worker task at least 40 ms after a vsync
  (never inside the vsync, where the previous readout may still be ending).
  It captures the next readout, which happens at the end of the next period.
- At the following vsync the frame is checked: DMA idle, end pointer exactly
  at the frame end, guards intact.
  - A frame that arrived while the previous merge was still running is
    skipped and counted.
  - A partial frame (Canon cuts one readout short around the 5th frame) is
    merged; the missing rows hold the previous frame, which is harmless for
    a maximum.
  - If the DMA does not finish, it is aborted with Canon's `AbortEDmac`. If
    even that fails, buffer ownership is uncertain: the buffers stay
    allocated and the status asks for a reboot.
- Mode loss (Canon leaves x5 or resets the sensor window) is detected from
  0xC0F06804 and the zoom state. The frame in flight is aborted and the
  composite so far is saved.

## 5. Merge

`lc_raw14_max_sel` (raw14-max.h) works directly on the packed data: seven
16-bit words hold eight 14-bit samples, and both buffers share the layout.
For every field it compares the two values in place after one shift, builds a
mask and selects the brighter bits. There is no unpacking or repacking, and
the result is identical to unpack -> max -> pack, which the host tests check.
The frame is merged in 8 bands with a 10 ms sleep between bands, so the
camera's GUI task (SET, display) keeps running. The first frame is copied.
One frame takes 1.5-1.9 s, mostly memory traffic, and overlaps the next
exposure.

Canon's hardware max engine (~0.12 s per frame) cannot be used while the
sensor runs; see HARDWARE-MERGE.md.

## 6. Preview and display

- After every merge, a 360 x 240 view of the accumulator (one RGGB quad per
  output pixel, white-balance gains from the DNG neutral, gamma table) is
  rendered into a 720 x 480 UYVY image. At each vsync it is
  copied over Canon's three Live View YUV buffers, so the LCD shows the
  composite instead of Canon's x5 image.
- If a full preview does not fit before the next readout, one quarter strip
  is rendered per frame instead.
- Canon's x5 zoom box and arrows are hidden with
  `canon_gui_disable_front_buffer()` for the session. Messages are drawn
  directly on the BMP overlay. NotifyBox is not used during a session,
  because its timeout calls `redraw()`, which brings Canon's overlay back.
- Global Draw would draw over the preview, so the controller switches it off
  for the session and restores the user's setting afterwards.

## 7. Keys

On the EOS M, Q and SET share one button. The core consumes the idle press
before module key callbacks run, but forwards the release. A small
`CBR_KEYPRESS_RAW` adapter therefore starts a session on the release of a
short press, and suppresses the fake and physical releases of long presses
and of presses that began in a menu. While a session runs, every SET/Q press
and release is consumed (Canon must never act on them in the modified x5
mode). SET stops the session, and all other mapped keys are held.

## 8. Output

- Focus pixels: the EOS M's PDAF pixels form an exact 24 x 30 tile in the
  full raster (200,942 positions, the same set as MLV App's
  `80000331_5280x3506.fpm`). Each is replaced by the mean of its four
  same-colour neighbours (x +- 2, y +- 2), none of which is a focus pixel.
- White balance: the DNG AsShotNeutral is computed from Canon's WB setting
  (custom WB: Canon's gains; otherwise the preset's or the set colour
  temperature, through a 2500-10000 K table computed from the EOS M colour
  matrix).
- DNG: uncompressed, the whole 5280-pixel raster with the active area marked.
  ISO, aperture, focal length and the shutter (= frame period) are written
  as EXIF.
- Files go to `DCIM/LIVE/LVFRnnnn.DNG`; `DCIM` and `LIVE` are created if
  missing. With Debug ON, `LVFRnnnn.LOG` (session summary, registers, hook
  counters, timings, keys, vsync intervals) and `.CSV` (one line per frame)
  are written next to the DNG; the LOG is rewritten at each stage, so an
  interrupted session still leaves evidence. With Debug OFF no log file is
  written.

## 9. Safety rules

- Only EOS M 2.0.2: the model and firmware are checked first. Every hooked
  instruction and the hook sites are fingerprinted, and nothing is patched on
  a mismatch.
- The session refuses to start with Auto ISO, the FPS override on, another
  RAW user (raw video, silent pictures, crop_rec), Global Draw still on, the
  menu open, or not in Movie mode.
- The hooks are inert outside the session and outside x5, and they are
  removed at the end.
- Battery: when the indicator reaches its last (red) level, the session
  stops like SET and saves while there is still power for the card write.
- Two full-size allocations are the camera's measured capacity (LCM9-13); no
  Canon-owned memory is used.

## 10. Limits

- Shortest shutter 2.5 s. This is set by the CPU merge plus the time the GUI
  task needs. Olympus goes down to 1/2 s.
- Shutters tested on the camera: 2.5, 3, 4, 5 and 10 s. The others use the
  same scheme and are marked "not yet tested" in the menu.
- ISO and aperture are Canon's Movie-mode settings, fixed for the session.
- The DNG is written after the session (about 32 MB); the composite is not
  saved incrementally.
