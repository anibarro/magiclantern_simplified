# Hardware merge on the EOS M: what was tried and why it does not work

This document records the investigation into using Canon's own image
"maximum" engine to merge full-sensor frames instead of the CPU. It covers
module builds LCM9-LCM40 (October 2026) and the experimental builds LCX1 and
LCX2. The product does **not** use the engine; it merges on the CPU (see
HOW-IT-WORKS.md). The engine is fast and exact, but on this camera it cannot
run at full speed while the sensor is producing frames.

Chronological notes, camera logs and every intermediate hypothesis are in
FULLRES.md. Camera evidence is in `tests/livecomp/LCMnn-camera` and
`tests/livecomp/LCX1-camera`, `LCX2-camera`. The experimental source code is
kept, unbuilt, in `research/livecomp-hwx/` (see the README there).

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
with no gap. The product therefore uses the CPU merge with a 2.5 s minimum
shutter.

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
