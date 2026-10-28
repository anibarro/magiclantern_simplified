# Hardware merge on the EOS M: what was tried and why it does not work

This document records the investigation into using Canon's own image
"maximum" engine to merge full-sensor frames instead of the CPU. It covers
module builds LCM9-LCM40 (October 2026) and the experimental builds LCX1 to
LCX7. The product does **not** use the engine; it merges on the CPU (see
HOW-IT-WORKS.md). The engine is fast and exact, but on this camera it cannot
run at full speed while the sensor is producing frames, and (LCX5-LCX7) it
loses its accumulator input while a factory capture is in progress.

Chronological notes, camera logs and every intermediate hypothesis are in
FULLRES.md. Camera evidence is in `tests/livecomp/LCMnn-camera` and
`tests/livecomp/LCX1-camera`, `LCX2-camera` (LCX3-LCX7: `LCX5-camera` to
`LCX7-camera`). The experimental source code is kept, unbuilt, in
`research/livecomp-hwx/` (see the README there); LCX3-LCX7 are on the local
branch `livecomp-lcx3`.

## 1. Why a hardware merge was wanted

- A full-sensor frame is 5280 x 3506 pixels of packed 14-bit RAW: 32,395,440
  bytes. Merging means reading the frame and the composite and writing the
  composite back: ~97 MB of memory traffic per frame.
- The CPU merge (ARM926, select-max on packed words) takes 1.5-1.9 s per frame.
  It runs while the next exposure is already in progress, so a shutter of
  2.5 s or more has no gap between frames. 2 s worked on paper but left the
  camera's GUI task no time to run, so the shortest offered shutter is 2.5 s.
- Olympus Live Composite allows 1/2 s to 60 s. Shutters below 2.5 s need a
  merge that is roughly ten times faster.
- Canon's firmware contains an image processing job that computes the
  per-pixel maximum of two RAW images. Measured with Live View paused it takes
  **~116 ms per full frame and is bit-exact** (LCM27-LCM40, 9 runs).

## 2. The engine

All addresses are EOS M firmware 2.0.2.

| Item | Detail |
| --- | --- |
| Job sequence | The same steps as Canon's FF42A3B4: setup FF429E68 (job descriptor), start (ENGIO table RAM 0x55180), wait on an event, stop (ENGIO table 0x551B0). |
| Configuration | Setup writes 43 registers in 0xC0F08000-0xC0F082A4 and 0xC0F0D000-0xC0F0D0AC (job list at RAM 0x55020). |
| Job descriptor | `{a, b, 10, 10, 10, 10, 0x8888dddd, 3, 2047, out}`; a/b/out are band pixel pointers (uncached). |
| Inputs | Read DMA channels 40 and 41, each starting one row before the band (4 halo rows needed above and below). |
| Output | Engine output is DMA source 1. Setup connects the output channel (state +0x28) to source 1. |
| Band size | Up to 1930 rows of 5280 pixels (pitch 9240); a frame is two bands (1754 + 1752 rows). |
| Context | Native state block at RAM 0x54FC8 (geometry pointer +20, DMA channels/connections +24..+40, output channel at +40). |
| Resources | Ten Canon resources: write channel (5), 0x1000C, 0x1000D, 0x50002, 0x50003, 0x50006, 0x80000, 0x20001, 0x30000, 0x30001. Locked atomically with the non-waiting try-lock FF2C6228 (created by FF2C5848, unlocked FF2C5EB4, deleted FF2C5A60). |
| Cache | The first job needs Canon's correction table (RAM 0x551E0, 0x188 bytes) written back from the data cache: a hook at FF42A01C cleans it. |

The wrapper (`research/livecomp-hwx/native-max.c`) fingerprints every ROM
routine it calls, never waits for a lock, checks that no channel is active
before configuring it, verifies the programmed DMA addresses before and after
the job, aborts only a transfer that still points at its own buffer, and
otherwise quarantines (keeps buffers and locks, asks for a reboot).

## 3. Results, in order

### 3.1 Inside Live View, idle window of a slow frame (LCM9-LCM23)

- Canon's write resource 5 (the engine's native output channel) is held by
  Live View permanently, in normal and in full-res x5 mode (LCM10, LCM20).
- With a private resource set and output channel 6: the inputs were consumed
  completely, but channel 6 was reprogrammed by Canon to another address
  (LCM12). Channel 6 was abandoned.
- Output channel 3: inputs consumed, **0 bytes** written in the idle window
  of a 2-4 s full-res frame (LCM22, LCM23). No other channel listened on
  source 1, so there was no competing listener.
- One exception: LCM13, in normal (not full-res) Live View at continuous
  3 fps, channel 3 wrote 422,112 bytes in 5 s and then timed out. This was
  only explained by LCX2 (section 3.6).

### 3.2 Live View paused (LCM27-LCM28)

- With `PauseLiveView()`, resource 5 is free and the job on channel 5 is
  **exact, 57-58 ms per band, ~116 ms per frame**, verified byte for byte
  against the CPU composite.
- The engine output reaches only channel 5. Channels 0, 3 and 4 receive
  nothing even when paused; a paused channel-3 attempt could not be aborted
  and required a reboot (LCM28).

### 3.3 Borrowing channel 5 from Live View (LCM29-LCM36)

- Live View keeps channel 5 armed and idle (source 2, its display image)
  between readouts. The module saved its programmed address, geometry,
  control word and Canon's per-channel record (RAM table at *(0x1C404)),
  aborted it, ran the job, and restored and verified the channel.
- Result: the job ran, **0 bytes on channel 5**, channel returned verified
  (LCM29, LCM31, LCM33, LCM36). On the 5th frame of a session the borrow
  abort did not idle the channel (quarantine).
- After two sessions that borrowed channel 5 the camera crashed later:
  `KerQueue.c:555` (LCM31) and `EvfState.c:504`, both in interrupt 0x6A,
  Live View's per-frame VD interrupt (LCM36). Borrowing leaves Live View's
  state machine inconsistent. Disabled permanently.

### 3.4 Reproducing Live View's state while paused (LCM30-LCM40)

The idea: find which part of the "Live View running" state blocks the
engine, by applying it while paused, where the engine works.

| Build | Applied while paused | Engine |
| --- | --- | --- |
| LCM30-32 | (diff only) Canon's ENGIO shadow over 0xC0F00000-0xC0F3FFFC: 227 registers differ, none in the job list | - |
| LCM34 | Each group alone: routing table 0xC0F03xxx (102), start/stop flags (75), 0xC0F01xxx clocks (12) | exact each |
| LCM35 | All 189 at once, plus EDMAC routing differences | exact |
| LCM38 | Timing-generator enables 0xC0F06004/0xC0F07000; then all 15 differing 0xC0F06/07xxx | exact |
| LCM39 | (diff only) resource registry: Live View holds 64 locks, paused 0 | - |
| LCM40 | Live View's locks of each of 12 resource types, one type at a time (powers that type's domain) | exact each |

Also tried in Live View (LCM31, LCM33, LCM36): Live View's 9 engine-area
block flags set to their paused values, the routing table cleared, and the
display turned off, each with channel 5 borrowed. All gave 0 bytes.

**Conclusion after LCM40:** no register value visible in Canon's ENGIO shadow
and no resource/power state reproduces the blockage. Whatever blocks the
engine is something Live View does while running.

### 3.5 LCX1: is there sensor data while Live View is paused?

If the sensor kept delivering frames while paused, the engine could merge
them. After one normal full-res frame (still in x5, 3 fps) the test armed
the RAW slurp (EDMAC 0x12, source 0) for up to 3 s per try, cumulatively:

| Try | State | Result |
| --- | --- | --- |
| L | Live View running (control) | data moved (Canon cut this transfer after 18 rows because it was armed right after a vsync; data flow confirmed) |
| T0 | paused | nothing |
| T1 | + the 47 resources Live View had locked, held by us (all lockable) | nothing |
| T2 | + the 6 differing timing-generator words (0xC0F06004, 0xC0F07000, 0xC0F07010, 0xC0F0704C, 0xC0F07138, 0xC0F0714C) back to LV values, then the apply write 0xC0F06000=1 | nothing |
| T3 | + Canon's 9 recorded CMOS/ADTG lists replayed through Canon's own writers | nothing |
| T4 | + all 189 differing pipeline registers back to LV values | nothing |

In every paused try: no vsync interrupt, no CMOS/ADTG/ENGIO activity, the DMA
pointer never moved; every armed transfer was aborted cleanly and Live View
resumed normally. **Pausing stops the readout at a level the module cannot
restart** (sensor power or Canon's Evf state machine).

### 3.6 LCX2: the engine with Live View running, traced in time

LCM9-40 always ran the in-Live-View job in the idle part of a slow frame.
LCX2 ran it with full-res x5 at 3 fps (a frame every 333 ms) on the top band
(16.2 MB expected) with output on channel 3 (never used by Live View), and a
helper task recorded every change of the input and output DMA pointers with
its time and vsync number.

- Inputs 40/41: consumed completely in 34 ms.
- Output channel 3, per frame:
  - an **8,192-byte burst exactly at each vsync**;
  - after ~185 ms, about **3 KB in steps of 128-512 bytes** spread over
    ~150 ms, which is the sensor readout window of a 3 fps frame;
  - nothing in between.
- 87,520 bytes in 3 s (9 frames, ~11 KB per frame). The marker rows
  (proof that real max values were written) were never reached.

| Time after job start (ms) | Vsync # | Output offset (bytes) |
| --- | --- | --- |
| 573-610 | 10 | 32 ... 352 |
| 621 | 11 | 8,800 |
| 807-940 | 11 | 9,056 ... 11,744 |
| 960 | 12 | 19,936 |
| 1141-1281 | 12 | 20,192 ... 22,880 |
| 1293 | 13 | 30,944 |
| ... | ... | ... |
| 2961 | 18 | 87,520 |

This explains LCM13 (continuous 3 fps: a trickle of output) and LCM21-36 (idle
window of a slow frame: no frame boundary, no readout, no output).

**In Live View the engine's output stage is synchronised to the running sensor
pipeline** (one burst per frame boundary plus a trickle during readout).
At 3 fps a full 32 MB frame would take ~16 minutes; even at 30 fps ~100 s.
With Live View paused the output stage free-runs (116 ms per frame), but the
sensor delivers nothing (LCX1).

A second LCX2 job with a 6 s wait returned "done" after 51 ms with nothing
written, and channel 3 could not be aborted: the session ended in the DMA
quarantine (reboot required, no damage). Do not run back-to-back jobs on a
stalled output channel.

## 4. Conclusion

| Sensor | Engine output | Usable |
| --- | --- | --- |
| Running (Live View) | paced by frames: ~11 KB per frame | no |
| Stopped (Live View paused) | free-running: 116 ms per frame, exact | no frames to merge |

No register value, resource lock, power domain, channel or routing that the
module can reach changes either row. Pausing Live View for every merge costs
0.2 s (pause) + 0.7-1.0 s (resume) + re-entering the full-res mode per frame,
i.e. more lost exposure than the CPU merge, which overlaps the next exposure
with no gap. The product therefore uses the CPU merge (2 s minimum shutter
since LCM64, with a hand-written ARM merge and the preview copied by EDMAC).
Sections 5a-5c add a path without Live View (factory captures): the engine
is exact there between captures but not during one, which leaves a gap of
~0.43-0.56 s per frame; for Live Composite the gapless Live View path stays.

## 5. What was not tried

These are open, high-effort directions. None is known to work.

- Find the register (outside Canon's ENGIO shadow, or written by another
  processor/RPC) that selects the sync source of the engine's output stage,
  so that it free-runs while the sensor is running. Canon's ROM has
  `RPCS_MASTER_TG_STATE` and the Evf/LVCDEV state machines
  (`./Evf/EvfState.c`, `lvcdevStartDev`) as starting points.
- Restart the sensor readout after `PauseLiveView` through Canon's own Live
  View start path (EvfState / LVCDEV), not through registers.
- Use a different hardware block that Live View does not share.

## 5a. LCX3: frames without Live View (branch livecomp-lcx3)

Idea: do not use Live View at all. ML's full-resolution silent pictures take
a sensor frame with Live View paused through Canon's factory test-image
path (`FA_CreateTestImage`, `FA_CaptureTestImage`, `FA_GetCrawBuf`,
`FA_DeleteTestImage`): an electronic exposure at the photo-mode M shutter,
the RAW in a Canon buffer. Paused is exactly the state in which the engine
is exact and fast (3.2), so a chain of such captures could be merged by the
engine, with the preview drawn into the display buffer, which Canon does not
refresh while Live View is paused.

The LCX3 build (`hwx-fa.h`, menu "LCX3 FA engine test", photo mode M, RAW,
shutter 15 s or shorter like silent.mo) does, with Live View paused once:

1. 5 x: new FA job, capture, `FA_GetCrawBuf`; frame 1 is the accumulator.
2. Frames 2-5: CPU hash of max(accumulator, frame); the engine (`native-max`,
   output channel 5, rows 4..H-4 in two bands, out = frame in place); rows
   0-3 and H-4..H-1 on the CPU; hash of the result compared with the CPU
   one. If the engine refuses (reason logged), the CPU merges instead so the
   timing still completes. The frame job then becomes the accumulator and
   the old one is deleted (no ML allocation: Canon's factory memory mode
   lasts while any job exists, and each job owns a RAW buffer).
3. After every frame: `raw_preview_fast()` of the accumulator, display on.
4. Composite saved as `DCIM/LIVE/LVHWX3.DNG`; log `DCIM/LIVE/LVHWX3.LOG`:
   per frame the job/capture/merge/preview/delete times, buffer addresses,
   engine reason/lock/setup/wait/us per band, hash verdict; per cycle the
   start-to-start time and the capture overhead beyond the exposure.

What the log answers:

| Question | LOG field | Decides |
| --- | --- | --- |
| Gap between frames | `Cycle*_ms` minus the exposure; `capture_ms` minus exposure | whether short shutters are possible, and how dotted trails get |
| Engine usable in this state | `reason1/2` (0), `verify` 1, `us1+us2` (~116 ms expected) | hardware merge yes/no |
| Canon's buffers suitable | `buf`, `crawbuf`, geometry line (`engine_geometry` 1) | whether the engine accepts them (low RAM, pitch 9240) |
| Preview without Live View | `preview_ms`, what the screen showed | the display path |

Camera result (4 s, ISO 100): 5 frames, engine 4/4 bit-exact against the CPU
hash, 57.4 ms per band (~115 ms per frame); FA capture 4409 ms for a 4 s
exposure (409 ms overhead), job create ~20 ms, delete ~20 ms, preview 280 ms.
Raw 5280x3528, pitch 9240, black 2047; Canon's buffers in low RAM
(0x46000064/0x48000064) are accepted by the engine. The DNG is a valid
light-painting composite. Rows 0-3 of an FA frame read 16383 (outside the
active area).

## 5b. LCX4: loop timing (branch livecomp-lcx3)

`hwx-fa4.h`, menu "FA test mode" + "FA test run", 8 frames at 4 s, no hash
verification. Log `LVHWX4.LOG`.

| Mode | Result | Dead time per frame |
| --- | --- | --- |
| Fast loop (merge, then next capture) | 8 frames, engine every frame, valid DNG | 832 ms (779-850) |
| Parallel engine (merge during the next capture, 3 FA jobs) | 8 frames, engine every frame, no BUSY, 57-60 ms per band, valid DNG | 447 ms (427-529) |
| Job reuse | a second capture into the same job gives a new image in the same buffer | - |
| Long exposure | 4 s: valid. 20 s (20.7 s): `FA_CaptureTestImage` returned after 20036 ms, before the exposure ended, with no RAW buffer; the session cleaned up and wrote its log, then the camera crashed | - |

- The engine runs normally while an FA capture is exposing: the merge is
  hidden inside the exposure, and the remaining gap (~440 ms) is essentially
  Canon's FA capture overhead (capture_ms - exposure = 409 ms) plus ~35 ms of
  job handling. This is the floor of this approach.
- Canon's FA capture appears to give up after ~20 s (4 s took 4409 ms, 20 s
  returned at 20036 ms without an image). The readout most likely finished
  later into a job that had already been deleted, which would explain the
  crash. This is the reason for silent.mo's 15 s limit: the FA path is
  limited to 15 s frames (15.4 s capture, inside the ~20 s wait).
- In the fast loop the preview (274 ms) was not overlapped: its helper task
  had a higher priority than the main task and ran before the capture
  started. With a lower priority it would give ~560 ms.
- The parallel run was not hash-verified (the engine is the same as in LCX3,
  but it now runs alongside the capture DMA); a sampled check is needed
  before a product.
- Compared with the Live View path (gapless, 1.5-2.3 s CPU merge, shutter
  >= 2.5 s): ~0.45 s gap per frame (11% at 4 s, 2% at 20 s), visible as short
  breaks in moving lights, but no CPU merge limit, photo mode and the FA
  readout instead of the x5 Live View hooks.

## 5c. LCX5-LCX7: live loop and the engine during a capture

`hwx-fa4.h` mode "Live loop": three FA jobs reused, the engine merges frame k
while frame k+1 is captured by a helper task, preview drawn during the
exposure, stop with half-shutter (SET does not reach ML while Canon's
buttons are locked; the shutter is left unlocked as in silent.mo's save
loop). From LCX6 every merge is checked pixel by pixel on 8 blocks of 4
rows (168,960 pixels) against max(accumulator, copy of the frame).

| Run (4 s) | Result |
| --- | --- |
| LCX5 | 28 frames, stable, ~420 ms gap; hash check failed 27/27; SET never arrived (battery pull) |
| LCX6 | 6 frames, half-shutter stop, DNG saved. Exact when no capture runs (frame 2, merged before the next capture; last frame). Wrong on every merge started 50 ms into a capture: 22-70% of the sampled pixels |
| LCX7 | 14 frames, exact 2/13: the two idle merges. Merges started 300, 1000 or 2000 ms into the capture: all wrong (37-91% of the sampled pixels) |

Every wrong pixel equals the new frame where the accumulator was brighter:
while an FA capture is in progress (any phase, not only its start) the
engine loses its accumulator input. The cached and uncached views agree,
so it is not a cache effect. The LCX4 "parallel" DNGs looked plausible
only because the losses are mostly in the noise floor.

Also seen: in LCX7 Live View was no longer paused when the half-shutter
stop was detected (`lv_resumed` 1), so the half-shutter stop is not safe
for a product either.

Conclusion for the FA path:
- engine merge only while no capture runs (as in the fast loop): exact,
  gap ~560 ms (409 ms FA overhead + 120 ms engine + ~20 ms job handling);
- CPU merge during the exposure (1.5 s composite, 2.3 s bulb): exact, gap
  ~430 ms, shutter >= 2.5 s (3 s bulb);
- so the engine only helps frames shorter than ~2.5 s, where a 0.56 s gap
  is 20-40% of the time and shows as dashed trails.

## 6. Safety lessons

- Never reprogram a DMA channel that Live View owns (channel 5): the damage
  appears later, as an assert in Live View's VD interrupt.
- Abort only a transfer whose programmed address is still our own buffer; if
  the abort does not idle the channel, keep every buffer allocated and ask for
  a reboot. Two quarantines happened during these tests; no camera was harmed.
- Read EDMAC pointer/state registers only while the resources that power the
  channel are held.
- Every ROM routine is fingerprinted (FNV-1a hash of its code) before it is
  called; on any other firmware the experiments refuse to run.

## 7. Reproducing

The experiments live on the local branch `livecomp-hwx` (LCX1 f2e2a193e,
LCX2 23987f85c). The same sources are in `research/livecomp-hwx/`:

- `native-max.c/.h`, `resource-snapshot.h`: the engine wrapper and resource
  registry walk (from LCM9-40, plus `lc_native_job_active`).
- `hwx-res.h`: resource registry walk and non-blocking hold/release.
- `hwx-step1.h`: LCX1 paused-readout test (LOG LVHWXD.LOG).
- `hwx-engine.h`: LCX2 engine-in-Live-View test with DMA tracing (LVHWXE.LOG).
- `lv-pause-regs.h`: the 189 registers that change when Live View pauses.
- `lcx2-integration.diff`: how they hook into fullres-lv.c and the menu.

To build: apply the diff to `modules/livecomp`, copy the files into the module
directory, define `LC_HWX` (as livecomp-config.h in the diff does) and build
as usual. The menu then offers "HW test: OFF / Paused readout / Engine in LV".
Only on EOS M 2.0.2, only for research, and expect to pull the battery.
