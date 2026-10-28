# How Live Composite and Live Bulb work (EOS M 2.0.2)

Live Composite emulates Olympus Live Composite: the camera exposes frame
after frame at a fixed shutter, and every frame is merged into a composite
that keeps the brightest value of each pixel. Static parts of the scene keep
the exposure of a single frame, while moving lights (car trails, light
painting, stars) accumulate. The result is one RAW DNG at full sensor
resolution.

Live Bulb (menu Mode, LCM54) uses the same capture and replaces the merge
with a sum: the result is one long exposure of frames x shutter, shown on the
LCD as it builds up (section 5a). Live Bulb removes the column pattern the
sum builds up (section 5b), and both modes can subtract dark frames taken
with the lens covered at the end (section 5c).

Usage is in INSTALL.txt. This document explains the implementation. The
development history is in FULLRES.md, and the failed hardware-merge work in
HARDWARE-MERGE.md.

## 1. Overview

```
SET (Movie Live View, x1)
 |- checks: model/firmware fingerprints, camera state, fixed ISO, no other RAW user
 |- allocate 2 x 32.5 MB (capture buffer + accumulator); create DCIM/LIVE
 |- install 4 hooks (inert unless this session is active AND Canon is in x5)
 |  + a trace-only hook on AbortEDmac
 |- request x5 zoom  ->  Canon reprograms the sensor through the hooks:
 |                        full-sensor readout at 3 fps
 |- warm-up: 12 frames at 3 fps with throwaway RAW captures ("Tactical delay")
 |- switch the frame timers to the chosen shutter at a vsync
 |- loop: arm RAW DMA -> frame done -> merge into accumulator -> LCD preview
 |        (the next exposure is already running while merging)
 |- SET / frame limit / low battery: finish the frame in flight
 |- Dark frame ON: countdown, dark frames at the same shutter, subtracted
 |- back to 3 fps, wait until frames are short again, leave x5, remove hooks
 '- column correction (Live Bulb; composite after a dark frame), fix focus
    pixels, write DCIM/LIVE/LVFRnnnn.DNG / LVBLnnnn.DNG (+ LOG/CSV with Debug ON)
```

Files:

| File | Role |
| --- | --- |
| `livecomp.c` | module entry, camera guard, raw key adapter for EOS M's combined Q/SET |
| `livecomp-lv.c` | menu, start/stop logic, Global Draw handling, worker task |
| `fullres-lv.c` | the session: hooks, x5 switch, timers, RAW DMA, merge, preview, DNG, LOG |
| `fullres-regs.h` | sensor register values and the shutter (timer) table; pure, host-tested |
| `raw14-max.h` | merge kernel on packed 14-bit RAW (Live Composite) |
| `raw14-add.h` | sum kernel (Live Bulb) and subtract kernel (dark frames) on packed 14-bit RAW |
| `fullres-fix.h` | column correction, focus-pixel fix and DNG white balance |
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
shutters from 2 s (k=1) to 60 s (k=22).

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
flight.

**Warm-up (LCM62).** A few frames after x5, Canon's Live View aborts our
RAW transfer on channel 0x12 at the start of one readout (always the same
frame: the 5th captured one since LCM18; 6 rows arrive, abort callback).
LCM21 traced Canon's Evf VD handler running a "LastRamClear" around then. The
session therefore stays at 3 fps for 12 vsyncs after x5, arming a throwaway
capture on each, so that abort falls on a throwaway frame whether Canon
counts vsyncs or captures. The screen says "Tactical delay... first frame in
n s". Preview only has no warm-up (its single frame ends earlier). A
trace-only hook on `AbortEDmac` (RAM 0x1CA60, installed only if the entry is
an STMFD) logs every abort of the RAW channel: vsyncs since x5, caller LR,
time, own abort or not, transfer in flight (LOG `Abort_hook`, and
`Warmup_vds_slurps_done_ms_aborts`).

**Leaving x5 (LCM58/59).** Because the timers latch one frame late, one more
frame at the session shutter can follow the last one (or a stop during a long
frame), and Canon cannot leave x5 before it ends. LCM57 gave up after 18 s
(FE_RESTORE, camera half-restored). Now the session switches back to 3 fps
if needed and waits until a vsync interval is actually short again (up to two
shutters; "Ending the current frame: up to n s"), then leaves x5. Canon's
drawing is turned back on only once at x1 (LCM60: before, Canon could draw
its zoom box while the last long frame ended).

**SET before the first frame (LCM59).** SET during the short transitional
frame used to end the session with no frame. It now means "stop after the
first full frame", so a single-frame bulb is possible; the exit switch is
then made only inside a frame that has lasted 90 % of the shutter.

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
  - A partial frame (Canon's abort, see the warm-up above) is merged in
    Live Composite (the missing rows hold the previous frame) and left out
    in Live Bulb (LCM56: it used to be added and counted, so a "12 frames"
    bulb had 11 frames of light).
  - If the DMA does not finish, it is aborted with Canon's `AbortEDmac`. If
    even that fails, buffer ownership is uncertain: the buffers stay
    allocated and the status asks for a reboot.
- Mode loss (Canon leaves x5 or resets the sensor window) is detected from
  0xC0F06804 and the zoom state. The frame in flight is aborted and the
  composite so far is saved.

## 5. Merge

The select-max works directly on the packed data: seven 16-bit words hold
eight 14-bit samples, and both buffers share the layout. For every field it
compares the two values in place after one shift, builds a mask and selects
the brighter bits. There is no unpacking or repacking, and the result is
identical to unpack -> max -> pack.

- On the camera (LCM64) the kernel is `lc_raw14_max_ldm` in
  raw14-max-asm.h: hand-written ARM (ARMv5TE), generated by
  `build_tools/gen_raw14_max_asm.py`. Each field is compared with a CMP of
  the field shifted to the top of a word (fields that cross a word: the
  high part, then CMPEQ of the next word on a tie), the mask is built with
  conditional ORRs of a shifted 0x3FFF, words are loaded with LDM (4 + 3)
  and only changed words are written. No branches except the loop.
- The C kernel `lc_raw14_max_sel` (raw14-max.h) has the same result and is
  what the host simulator runs. `test_livecomp_raw14_asm.py` compiles the
  ARM kernels with the module toolchain and runs the machine code on a small
  ARM interpreter (`livecomp_armemu.py`): bit-exact on random, noise, tie
  and steady data.
- Camera (LCB3, 2.5 s sessions, 11 frames each): 1.46-1.52 s per frame
  (C kernel: 1.54-1.78 s). The ARM kernel does not count changed pairs: the
  CSV column `changed_pct` is 0.
- The benchmark that led to it (LCB1-LCB3, `research/livecomp-speed/`):
  reading both 32 MB buffers takes 0.92 s with LDM and ~1.25 s word by word;
  the merge is limited by instructions, not memory, and a whole-pair SWAR
  variant is only faster when nothing changes.

The frame is merged in 8 bands with a 10 ms sleep between bands, so the
camera's GUI task (SET, display) keeps running. The first frame is copied.
One frame overlaps the next exposure.

Canon's hardware max engine (~0.12 s per frame) cannot be used while the
sensor runs; see HARDWARE-MERGE.md.

## 5a. Live Bulb

With Mode = Live Bulb the frames are added instead (`lc_raw14_add`,
raw14-add.h). The first frame is copied as for the composite; it carries the
black level once. Every later frame adds only its signal:

```
acc = clamp(acc + frame - black, 0, white)      per sample
```

- `black` is the optical-black mean of the first frame (as for the
  composite). The subtraction is signed, so noise below black is added as
  it is and the dark parts do not drift upwards.
- `white` is the DNG white level. A sample that reaches it stays there, like
  a saturated photosite in a single long exposure.
- After every frame the optical-black columns of the accumulator are
  measured again (`acc_black`, rounded mean). The DNG black level and the
  preview use that value, so the integer rounding of the per-frame black
  cannot add up over many frames.
- Partial frames are not added at all (LCM56) and not counted: a gap of one
  sub-exposure for every row, the frame count and ExposureTime stay exact
  (LOG `bulb_partials_skipped`).
- A frame that arrives while a merge is still running is skipped as usual;
  the total exposure counts merged frames only, so the DNG ExposureTime
  (`merged x period`) stays true.
- Output: `DCIM/LIVE/LVBLnnnn.DNG` (own numbering), LOG/CSV the same as for
  the composite. The LOG line `Mode_total_exposure_ms_acc_black` records the
  mode, total exposure and accumulator black level; the merge-kernel line
  says `add`.

Every word changes, so there is no select-max shortcut. The word kernel
`lc_raw14_add_sel` (LCM55) takes 2.3 s per full frame on the camera, too
slow for 2.5 s frames. LCM65 runs `lc_raw14_add_ldm` (and `lc_raw14_sub_ldm`
for dark frames) on the camera instead: hand-written ARM in
`raw14-add-asm.h`, generated by `build_tools/gen_raw14_add_asm.py`, same
result bit for bit. Words come in by LDM, two at a time; an inner field is
one AND of the native word rotated, a field across two words one AND after
putting their halves together (no per-word rotation); the clamp is one
unsigned compare and a BICHI. Emulated ARM946: 199 instructions / ~216
cycles per 16 pixels vs 241 / ~257 for the C kernel. So Live Bulb allows
2.5 s frames (not yet run on the camera). If the next readout overtakes
the add, the session stops after the frame already armed ("use longer
frames").

## 5b. Column correction (Live Bulb; composite after a dark frame)

Every sensor column has a small offset of its own. A maximum keeps it at one
frame's size; a sum of N frames multiplies it by N while random noise grows
only as sqrt(N): vertical bars in the shadows (LCM55: ~23 DN at 12 frames).
Measured on the camera (LCM56-LCM60):

- Part of the pattern repeats every 8 columns (one packed block, "lanes"),
  and that part is not the same in the covered rows as in the image.
- One large component (P, ~12.7 DN per frame) switches from frame to frame:
  sums of 10-12 frames contain anything from 0 to 10 times it, and the
  covered rows do not track how much the image rows got.

So the offsets are estimated per session, at save time (`fullres-fix.h`):

1. `lc_colfix_measure`: trimmed mean of covered rows 16-26 per column, minus
   the mean of its CFA parity; `lc_colfix_split_lanes` removes its 8-column
   part (only the non-periodic part is trusted); `lc_colfix_ob_var` gives
   the noise variance of that estimate (9 samples: ~9 DN).
2. `lc_colfix_scan_row` on rows with y % 8 < 2: for every sample whose 9
   same-colour neighbours x-8..x+8 are all below black+1024, d = v -
   mean(v[x+-2,4,6,8]). A column offset passes this high-pass unchanged;
   flat scene and linear gradients cancel. Samples with |d| >= 200 are
   skipped, and (LCM60) samples whose left and right neighbour means differ
   by 2 sigma or more: a column offset looks the same from both sides, a
   vertical scene edge does not (LCM59 LVBL0021: an edge in the shadows got
   -31/+32 DN bands; with the check -6/+8).
3. `lc_colfix_combine`: the lanes come from the image (mean d per x % 8,
   zero mean per parity; fewer than 4000 samples per lane: the covered-row
   lanes are kept). Per column, the image estimate (mean d minus its lane)
   is weighted against the covered-row estimate, w = n*vob/(n*vob+vpix), so
   a bright scene falls back to the covered rows. One Jacobi step corrects
   for the neighbours' own error inside d.
4. `lc_colfix_apply` subtracts the offsets from every row; saturated samples
   stay.

Offline on LVBL0005/0009 (estimate from one half, scored on the other):
column error 9-11 DN with covered rows only (LCM56), 4.3-7.6 DN with the
image estimate. The pass costs ~9 s on the camera ("Applying denoise..."
once it takes more than 1 s).

## 5c. Dark frames (menu Dark frame, LCM59/LCM61)

After the last light frame the sensor keeps running at the session shutter
(no exit switch). A 10 s countdown asks to cover the lens ("SET=skip"); the
first dark frame is one whose every row started after the countdown (rolling
shutter: the first rows of the frame ending at vsync V+1 started up to one
readout before V). Then:

- **Live Bulb:** as many dark frames as light frames, same shutter (the dark
  lasts as long as the exposure). One dark frame x N does not work: P (5b)
  differs between frames, and LCM58's N x one dark frame added ~13 DN bars
  (LVBL0017/19); warm pixels too weak to see in one frame add up over N.
- **Live Composite:** one dark frame. A maximum takes each pixel from one
  frame, so its offsets are in it once, and max(L_k - D) = max(L_k) - D.

Only two 32 MB buffers exist (LCM9-13), so:

- Save unprocessed OFF: each dark frame is subtracted from the accumulator
  as it arrives, `acc = clamp(acc - dark + black, 0, white)`
  (`lc_raw14_sub_sel`, the exact inverse of the add, same speed; a sample at
  white stays). The next dark frame is armed before the subtraction when the
  subtraction ends before its readout, else every other frame.
- Save unprocessed ON: the light result is saved first (LVBR/LVCR); the dark
  frames are summed in the accumulator and saved (LVDK/LVCK); the light
  result is read back from the card into the capture buffer and the dark sum
  subtracted there.

The column correction (5b) then runs on the result (Live Bulb always; Live
Composite only after a dark frame), because the switching pattern P need not
cancel between the light and the dark frames. SET during the dark frames
stops after the current one ("dark partial"). The subtraction adds the dark
frames' own noise (shadows ~1.4 x noisier), like Canon's long exposure noise
reduction.

## 6. Preview and display

- After every merge, a 360 x 240 view of the accumulator (one RGGB quad per
  output pixel, white-balance gains from the DNG neutral, gamma table) is
  rendered into a 720 x 480 UYVY image. At each vsync it is
  copied over Canon's three Live View YUV buffers by EDMAC (`edmac_memcpy`,
  its resources held for the session; ~6 ms, LCM64; memcpy took ~110-150
  ms), so the LCD shows the
  composite instead of Canon's x5 image.
- If a full preview does not fit before the next readout, one quarter strip
  is rendered per frame instead.
- Canon's x5 zoom box and arrows are hidden with
  `canon_gui_disable_front_buffer()` for the session, and drawing is turned
  back on only after the camera is at x1 again. Messages are drawn directly
  on the BMP overlay. NotifyBox is not used during a session, because its
  timeout calls `redraw()`, which brings Canon's overlay back. After the
  session (x1) the save steps use NotifyBox with a 60 s timeout ("Saving
  the unprocessed DNG...", "Saving the dark frames...", "Subtracting the
  dark frames...", "Applying denoise...", "Saving the DNG..."), so the
  screen never goes blank while the camera works.
- Global Draw would draw over the preview, so the controller switches it off
  for the session and restores the user's setting afterwards.

## 7. Keys

On the EOS M, Q and SET share one button. The core consumes the idle press
before module key callbacks run, but forwards the release. A small
`CBR_KEYPRESS_RAW` adapter therefore starts a session on the release of a
short press, and suppresses the fake and physical releases of long presses
and of presses that began in a menu. While a session runs, every SET/Q press
and release is consumed (Canon must never act on them in the modified x5
mode). SET stops the session, and all other mapped keys are held. Canon's own
"button pressed" command (0x54) and touch events (0x6F-0x79) are swallowed
too (LCM56): the EOS M screen sits next to SET, and in x5 either one opened
Canon's zoom box and knocked the camera out of the mode. The stop request is
a counter, so a further SET is seen during the dark-frame countdown (skip)
and the dark frames (stop).

A session cannot start while one runs (re-entry guard), and the controller
no longer trusts `task_create()`'s return value (LCM56: it can be negative
as an int, so LCM55 sometimes reported "Worker failed" while the worker ran,
and a second SET started a second session): the worker reports itself, and
failure is declared only after 3 s without it.

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
- Files go to `DCIM/LIVE/LVFRnnnn.DNG` (Live Bulb `LVBLnnnn.DNG`); `DCIM`
  and `LIVE` are created if missing. Save unprocessed adds `LVBR`/`LVCR`
  (before any in-camera correction) and `LVDK`/`LVCK` (dark frames; Live
  Bulb: their sum), same number, same metadata. With Debug ON, `LVFRnnnn.LOG` (session summary, registers, hook
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
  removed at the end. The AbortEDmac hook only records (no change to the
  call) and is skipped if the function does not start with an STMFD.
- Battery: when the indicator reaches its last (red) level, the session
  stops like SET and saves while there is still power for the card write.
- Two full-size allocations are the camera's measured capacity (LCM9-13); no
  Canon-owned memory is used.

## 10. Limits

- Shortest shutter 2 s (Live Bulb 2.5 s, LCM65, not yet camera-tested).
  This is set by the CPU merge (1.5 s, Bulb 2.3 s with the C kernel) plus
  the preview, the ~150 ms readout and the time the GUI task needs.
  Olympus goes down to 1/2 s. A further idea (merge chasing the readout)
  is in FULLRES.md, LCM64.
- Shutters run on the camera: 2, 2.5, 3, 4, 5, 6, 10, 15 and 50 s. The menu
  still marks shutters other than 2-5 and 10 s "not yet tested".
- Start-up is about 4 s longer than LCM61 (warm-up). It moved Canon's
  recurring abort onto a throwaway frame (camera: abort at VD 7, inside the
  warm-up; no partial frame since).
- Dark frames are taken at the end (best temperature match) and need the
  lens covered by the user: the EOS M cannot close its shutter in Live View.
  Subtracting them adds their noise; there is no memory for averaging more
  dark frames in a composite.
- ISO and aperture are Canon's Movie-mode settings, fixed for the session.
- The DNG is written after the session (about 32 MB); the composite is not
  saved incrementally.
