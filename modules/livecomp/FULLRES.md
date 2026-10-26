# Full-sensor development

LCM13 is a feasibility gate, **not a full-sensor capture implementation**. The
working normal-LV composite remains the default. The new Task / Full-res check
tests memory capacity and native maximum blending without changing sensor mode.
One camera run should identify whether the engine is usable in this context,
whether its result is exact, and whether memory/throughput permit the next step.

## Why this gate exists

The reference is Bilal Fakhouri's [crop_rec_4k_mlv_snd source at
5f0cda8](https://github.com/bilalfakhouri/magiclantern_hg_02/blob/5f0cda843ffa732a1ae4811d22b11fb92c4fa158/modules/crop_rec/crop_rec.c).
Its EOS M `CROP_PRESET_1X1` / `CROP_Full_Res` claims 5208x3478 active pixels at
3 fps. This is a zoom-based sensor mode, not a larger normal-LV RAW allocation.
The earlier unchanged `crop_fr` port loaded after core additions but did not work
on camera; it supplied no sufficient geometry/ownership evidence to reuse here.

Dependencies identified in the reference:

| Component | EOS M settings / dependency |
| --- | --- |
| Sensor | CMOS 5 = 0, CMOS 7 = 0x3a0; zoom path activation |
| Readout and timers | RAW H/V 0x538/0xdb2, timer A/B 0x56b/0x1e03; matching horizontal/vertical auxiliary registers |
| ADTG | Matching exposure blanking, readout-end and power-save timing |
| Canon preview | EDMAC 9 vertical size increases; EDMAC 24 redirected to photo-mode memory; changed preview window |
| Memory | Full-sized RAW fallback plus exclusive capture/merge buffers |
| Exit | Undo all hooks, restore modified RAM geometry and rebuild normal Canon view |

The preview path hardcodes photo-memory address `0x1595b00` and a vertical
extension the source itself questions. Those are not module-owned allocations.
Do not transplant the redirect without proving destination extent and lifetime.
The new check does not install any of these sensor/preview patches.

The standard core **does support** allocating a larger RAW fallback via
`raw_lv_realloc_buffer`; it is not inherently limited to the default buffer.
However, `raw_lv_vsync` checks fallback capacity even when redirecting a frame
into a module buffer, so that allocation must be included in the budget. EOS M
also forces a height of 727 for ordinary unzoomed LV; raising the module's size
limit alone cannot enable this readout.

## Implemented in LCM9

- The legacy native maximum wrapper is now a generic module object with an
  EOS M 2.0.2 runtime guard, retained ROM fingerprints and core patch API imports.
  No platform includes, custom core, factory capture or external module.
- The engine is used only after explicit selection of Full-res check and SET.
  A RAM context snapshot precedes engine use. Missing context is reported without
  inventing Canon state. A nonwaiting native resource lock reports contention.
- Up to four large allocations are reserved simultaneously: two future capture
  buffers, one in-place accumulator, and the core fallback allowance. All remain
  owned through the test. Actual sensor capture and core reallocations are off.
- Synthetic 5280x3508 packed RAW14 data (not measured sensor dimensions) exercises
  the full RAW width, two 1754-row engine bands and all 14-bit input values. Each
  input has four halo rows at both ends; each allocation has outer guards.
- Every output sample is compared with the exact CPU maximum. Every unchanged
  second-source sample and both outer halos are also checked. Engine timing and
  Live View callback gaps are logged; these do not prove sensor exposure duty.
- Native failures stop the test. Uncertain DMA, event, unlock, patch-removal or
  buffer state retains allocations and blocks another run until reboot. Clean
  refusal/completion releases allocations and restores the shutter lock.
- DNG writer bounds now accept up to 5280x3528. Host tests save and independently
  decode every one of the 18,627,840 samples at that size, including short writes
  and injected write/allocation failures. Ordinary capture geometry is unchanged.

The native engine's hardware accuracy alongside LV remains **unverified** until
the camera check passes. Host tests replace hardware calls to exercise control
flow; they cannot establish Canon resource arbitration or DMA behavior.

## Camera check and interpretation

Install only `livecomp.mo`, then use Movie M, ordinary unzoomed LV, no other RAW
tools. Select Live Composite ON, Task / Full-res check. Close the menu and press/release
SET. Wait for the result, retain `LCFRnnnn.LOG`, and select Live composite to
return to ordinary use. No DNG is expected from this check.

The log is written before native execution so an interrupted job still leaves
context/memory evidence. After a clean return it includes both band results and
exact sample checks. Stage 6, zero native error/unsafe/mismatches, unchanged source
and guards, and successful results for both bands establish a successful check.
Four allocations establish capacity only in that camera state. Compare total
engine time with the proposed frame period; LCD work and capture contention need
additional headroom. A pass does not validate the unimplemented sensor mode.

## Next implementation gate

1. Use the check's context, lock, timing and memory result to choose native merge
   or a slower CPU path. If Canon has not initialised this engine in LV, identify
   a reversible initialisation path before calling its setup routine.
2. Own or safely lease the enlarged preview/capture destinations before activating
   the zoom readout. Validate exact RAW boundaries; do not treat active image
   dimensions or the synthetic benchmark dimensions as DMA geometry.
3. Enter/restore the EOS M mode using runtime-guarded module hooks and validated
   original instructions. Keep frame acquisition off until the state is coherent.
4. Use two leased capture slots and an in-place accumulator, with a small Bayer
   LCD snapshot. Account for the core fallback. Stop on any overrun; never skip
   sensor inputs to disguise insufficient throughput.
5. Integrate the validated merger and DNG writer, then perform one bounded real
   full-resolution capture with LCD preview, Stop, and mode restoration.

The archived LCM8 package is retained locally under
`platform/EOSM.202/releases/LCM8`; logs, images and build artifacts remain ignored.

## LCM9 camera result (2026-10-24)

The camera passed ROM/context validation and allocated two of the four requested
32,488,032-byte buffers. Native resource acquisition returned `0xD` (busy) on
its first attempt. No setup or blend ran, so neither accuracy nor merge speed
was measured; the 33 us timing is refusal overhead. Six LV callbacks continued
with a maximum 334 ms interval. Guards were intact and cleanup reported safe.

This rules out treating the four-buffer/native-engine design as ready. It does
not prove the engine is permanently unavailable or that only two buffers can
exist under all camera settings. Next investigate resource ownership and frame
phase availability, plus a smaller/segmented memory layout. The native context
already exists; factory initialization is not the missing prerequisite here.
The pinned crop_rec source also documents increased SRM capacity with Canon RAW
picture quality at boot; that setting was not captured in this diagnostic.

## LCM10: resource registry and short SET

Offline inspection of the camera's existing ROM dump confirms that FF2C6228
returns 0xD when FF2C5BF8 finds a requested resource with state 1 (locked). The
native path requests ten resources, not just the three visible DMA channels.
A single busy return therefore cannot identify the conflict or its duration.

LCM10 fingerprints the registry checker and its literal addresses, then reads
requested IDs and their busy bits from RAM under a brief interrupt lock. Walks
are bounded to 16 IDs and 64 nodes per list; malformed/unexpected structures
invalidate the snapshot. This observes occupancy only: it does not grant
ownership or replace the native nonwaiting lock. No peripheral register is read.
The full-res check takes 180 snapshots at 17 ms spacing before the existing
memory/native test. The log records valid/all-free counts, any/always-busy masks,
the ordered resource IDs, and Canon picture quality. Mask bit i refers to
Resource i. Even an all-free snapshot is not permission to bypass acquisition.

The two-buffer result was contiguous allocation capacity in that camera state,
not a measure of all RAM. A possible redesign is an accumulator split into
bands from Shoot memory plus small LCD snapshots. It still requires explicit
capture ownership and accounting for the core RAW fallback; no unowned SRM
chunk or hardcoded photo-memory destination may be reused to satisfy a budget.

The supplied button-handling report describes a conceptual interception scheme.
This core consumes the initial physical Q/SET event before module callbacks,
but its release survives. The new raw adapter uses physical release for a short
press and leaves long press available. It suppresses long press's fake release
and subsequent physical release, plus releases of presses begun in a menu.
Tests cover those sequences, Off, menu exit, half-shutter and callback ordering.
Camera confirmation of short-press delivery remains pending.

The ROM's initial ten-resource array at RAM 0x54ff8 contains three DMA IDs
(replaced during native-path initialization) followed by 0x50002, 0x50003,
0x50006, 0x80000, 0x20001, 0x30000, 0x30001. These include dependencies beyond
DMA channels; their occupancy must be observed before naming the conflicting
subsystem. Local disassembly and byte extracts are retained under ignored
research/livecomp-native-lock. Resource descriptions are deliberately not
invented for the undocumented classes 5 and 8.

## LCM10 camera result and LCM11 routing

LCFR0001.LOG from LCM10 recorded 180 valid samples, zero all-free samples,
and busy-any/busy-all masks of 1. Resource index 0 (ID 5) was busy in every
sample; the other nine were free in every sample. The core's resource mapping
identifies ID 5 as write DMA channel 5. This establishes the conflict in this
camera state, but does not identify its owner or prove perpetual occupation.
No native setup/blend ran. The 34 us value is refusal overhead, not blend speed.
Cleanup was safe; the two large allocations and all guards were retained intact
until release. Picture quality 0x3070000 is RAW + large fine JPEG, per
src/property.h, rather than RAW-only 0x4060000.

Offline tracing shows the native output route is configurable:

- FF42A038/044/094 load channel from state + 0x28 for callback registration,
  connection 1 and output DMA setup. FF42A468/46C use state + 0x28/0x2C for start.
- FF429D50 completion callback signals event bit 1 through state + 8; it does
  not embed channel 5. FF42A528 uses the context channel for callback removal.
- The init/start/stop ENGIO tables at RAM 0x55020, 0x55180 and 0x551B0 configure
  the processing blocks; the inspected start/stop tables contain no DMA-channel
  register address. Output connection selection is performed by setup above.
- FF2C5848 copies resource IDs into its own allocation. FF2C5A60 deletes that
  entry only when not locked. Both functions now have ROM fingerprints.

LCM11 creates a private ten-resource entry for each synthetic band, replacing
only write resource 5 with write resource 6. Channel/index 6 agree on EOS M;
the core's historical EOS M channel notes list it as a candidate. Availability
is never assumed: Canon's atomic nonwaiting lock must acquire all ten resources.
The original native resource list must match all ten expected IDs before use.
The original lock entry and busy channel 5 are never modified or unlocked.

After acquisition and a context recheck, the wrapper temporarily sets the native
output channel to 6 alongside the private geometry. Canon setup registers its
callback and connects this channel to output connection 1. Start, callback
removal and idle checks use 6. It restores output channel 5 and geometry before
unlocking the private entry, then deletes it. Resource creation/deletion and
channel routing are covered by host fault-injection tests; this is not evidence
that the hardware supports the combination correctly.

Correction to earlier terminology: 0x1CB3C unregisters a completion callback;
it does not stop/abort DMA. The wrapper issues Canon's processing stop table
and checks all three DMA channels. Any active channel or uncertain cleanup
keeps buffers/resources pinned and requires reboot. It does not forcibly abort
an occupied channel to make the check pass.

The next camera check should use Canon RAW-only picture quality, set before
restarting the camera, to test the reference source's memory-capacity claim at
the same time. This setting is not changed automatically. LCM11 still tests
synthetic full-width buffers, not sensor readout; ordinary composite is unchanged.
A busy refusal on replacement channel 6 is a valid result, not a reason to
remove that lock. Full-sensor acquisition remains pending even if blending passes.

## LCM11 camera result and LCM12 completion tracing

The installed LCM11 hash matched the build manifest. LCFR0002.LOG reports
RAW-only quality (0x4060000), but still only two large allocations. All 180
registry snapshots again found original write resource 5 occupied. The private
channel-6 resource set was successfully acquired, native setup returned zero,
and the correction-table hook ran once without error. The completion wait
returned 9 after 4,996,744 us. Offline tracing of RAM 0x1E7D4 confirms that
return 9 maps to the kernel timeout result, not a setup error.

The stop/cleanup path reported all three DMA channels idle, restored context,
deleted the event and private resource entry, and released resources. Guards
were intact and Live View delivered 21 callbacks with a maximum 334 ms interval.
No pixel verification ran because the wait failed. Idle states recorded AFTER
the stop sequence cannot prove whether DMA finished beforehand, made partial
progress, or never transferred data. This is not a successful blend benchmark.

LCM12 retains channel 6, the resource set, geometry and five-second timeout.
After successful setup it wraps the three exclusively owned DMA completion
callbacks with counters that immediately forward to Canon's original handlers.
Those original handlers still signal the same event bits. It records each
channel's state, flags, start address and current pointer before starting and
immediately after the wait, before stopping. All register reads are inside the
acquired resource lifetime. Busy refusal performs none of these peripheral reads.
DMA trace indices 0/1/2 correspond to input 40, input 41 and output 6.

This distinguishes missing completion notifications from transfer-progress
problems in one run. There is no guessed timeout extension, channel switch or
sensor-mode change. Host tests exercise forwarding, a missing output completion,
trace snapshots, busy refusal and cleanup. These are diagnostic changes, not
a claimed correction of the hardware timeout. Full-sensor acquisition and a
memory layout fitting the observed capacity remain unfinished.

## LCM12 camera result and LCM13 ownership fencing

LCFR0003.LOG and the installed module hash confirm LCM12. Both input callbacks
ran once. Inputs 40 and 41 advanced exactly 16,225,440 bytes (1756 rows at
9240 bytes), matching the 1754-row band plus Canon's two input halo rows.
Output channel 6 had no completion callback. Its programmed address changed
from 0x04009120 to 0x0D2DEC00; the current pointer was 0x0D2EB8C0. The latter
addresses are outside either owned synthetic buffer. This is evidence of DMA
reconfiguration despite successful registry locking, not merely a missing
completion notification. The actor is not identified by this log.

LCM12 reported safe cleanup because it checked only idle DMA state and context,
not destination identity. That result was too optimistic: idle does not prove
continued ownership. No image verification ran, and the test does not establish
that the output was correct or that all memory outside the guards was untouched.
Do not repeat the channel-6 experiment.

LCM13 switches only the synthetic output route to write channel/index 3, listed
as a candidate in src/edmac-memcpy.c's EOS M notes. Its availability remains a
hardware hypothesis, not a claim of exclusive ownership. The private ten-resource
try-lock is retained, and no occupied resource is forcibly acquired. The core's
copy channel 0x13 was considered but rejected: individual copy calls use a private
semaphore and do not themselves acquire the resource lock. A resource lock alone
would not safely coordinate those calls. RAW slurp channel 0x12 is also untouched.

Immediately after acquisition, active DMA causes quarantine without setup or
unlock (unlock itself resets the channel). After setup and before start, all
three DMA channels must be idle and have the
expected programmed addresses (input pixels minus one pitch, output pixels).
After the wait, those programmed addresses must still match. A mismatch sets
LC_NATIVE_OWNERSHIP (4096), stops our processing blocks, retains buffers, event,
hook and resources, and requires reboot. It does not unregister callbacks or
abort a channel that another path may have taken over. This detects the observed
failure; it cannot prove that no transient reconfiguration happened between
snapshots. The log retains the full LCM12 transfer/completion tracing.

Host regression tests reproduce the observed foreign output address and a
pre-start active channel, requiring quarantine with no event deletion, resource
unlock/deletion or callback unregister. Normal completion, a second band, busy
refusal and the previous failure cases remain covered. Hardware behavior and
full-sensor acquisition remain unverified.

## LCM13 camera result (LCFR0004.LOG, recorded 2026-10-26)

RAW-only quality (0x4060000); again only two of four 32,488,032-byte
allocations. Write resource 5 busy in all 180 samples. The private resource set
with output channel 3 was acquired and setup returned zero. Both input channels
completed: each advanced exactly 16,225,440 bytes (0x04006D08 -> 0x04F801A8 and
0x02006D08 -> 0x02F801A8). Output channel 3 kept its programmed address
0x04009120 (no foreign reconfiguration this time) but advanced only 422,112
bytes (pointer 0x04070200) and was still active (state 1) when the 5 s wait
timed out. Reason 96, no pixel verification. Cleanup reported safe.

Interpretation: the processing chain consumed both inputs but produced only a
small fraction of the output before stalling. Channel routing is no longer the
only suspect; the output connection, pipeline back-pressure or a shared
processing block used by Live View are all possible. Not resolved.

## Direction change (2026-10-26)

After five camera rounds (LCM9-LCM13) without one verified native blend, the
native-engine track is paused (code retained, LCM14 untested). The blocking
unknown is the sensor readout itself, which has never been attempted in this
module. Accepted constraints from the user: a slow full-res cadence (one frame
every ~2-4 s with near-continuous exposure, like Olympus base exposures) is fine.
At that cadence a CPU maximum (~2 s projected, to be optimised) is sufficient,
and the two allocations the camera actually provides are enough: one capture
buffer merged into one accumulator during the idle part of each frame period,
with overrun detection. See the LCM15 section for the readout gate.

## LCM15: full-sensor readout gate (Task: Full-res readout)

First attempt to put the sensor itself into full-resolution readout inside
this module. Implementation: `fullres-lv.c` (camera side) and
`fullres-regs.h` (pure register rewriting, host-tested).

**Why the earlier `crop_fr` port probably could not capture.** The stock core
has no `CONFIG_ALLOCATE_RAW_LV_BUFFER` for EOS M. At full size
`raw_lv_realloc_buffer` hits `ASSERT(0)`, the fallback stays at Canon's
24.5 MB default, and `raw_lv_vsync` refuses to slurp (even redirected frames)
because 9240 x ~3500 bytes do not fit. LCM15 does not use core LV RAW at all:
it arms `edmac_raw_slurp` itself from the module's CBR_VSYNC (which runs
before `raw_lv_vsync`) into its own 32.5 MB buffer, with the same RAW type
register write the core uses. Core LV RAW must stay unrequested (Global Draw
off); the gate aborts if anything requests it.

**Sensor mode.** Reference: Bilal's EOS M `CROP_PRESET_1X1` / `CROP_Full_Res`.
Hooks on the RAM `cmos_write` (0x2998C) and `adtg_write` (0x2986C), already
used by the stock crop_rec for EOS M, and the ROM `_engio_write` (0xFF2C19AC)
and `_EngDrvOut` (0xFF2C1694), both named in `platform/EOSM.202/stubs.S`.
All are fingerprinted (EngDrvOut only needs a plausible prologue and is
optional). They only act while the gate is active AND
`PathDriveMode->zoom == 5` (0x892E8+0x12). Values applied:
CMOS[5]=0, CMOS[7]=0x3A0; RAW window 0x0DB20538; timers A=0x56B,
B=0x1E03 (3 fps) or 23053 (1 fps) or 46107 (0.5 fps); HEAD3/HEAD4; ADTG
power-save/read-out timing; shutter blanking as 100/50/25/12% of the frame
(NRZI); C0F38024/383D4/383DC preview window.

Deliberately **omitted**: EDMAC#24 redirect to 0x1595b00, EDMAC#9 RAM
patches (0x5976C/0x5979C), C0F08184 and the HIV address hook. In the
reference these only recover Canon's preview height and write Canon-owned
memory; RAW capture does not depend on them. Expect a wrong/cropped LCD
during the test.

**Flow.** Movie LV x1 -> allocate 2 x 32,488,032 bytes (capture +
accumulator; the capacity LCM9-13 measured) -> install hooks -> `set_lv_zoom(5)`
-> verify C0F06804, compute geometry from C0F06800/04 exactly as
`raw_lv_get_resolution` -> measure cadence -> capture N single frames
(DMA idle, pointer, tail sentinel, guards) and merge them (memcpy, then exact
max) -> deactivate -> `set_lv_zoom(1)` -> unhook -> save LVFR DNG/LOG. Frames
that arrive while merging are not captured: this is a gate, not continuous
composite. The vsync callback re-applies timer B if Canon restores it outside
`engio_write` (counted in the log). The LOG is rewritten at every stage.

**What the first run should tell us.** Whether x5 engages with the hooks,
whether C0F06804 holds the full window, the real geometry and frame cadence,
whether a full-size RAW frame completes into our buffer, black level / band
means to place the active area, real full-res merge and memcpy times, and
whether normal LV is restored. Host simulation covers control flow, refusal
before any hook, hook-install failure, missing window, LV stall and
uncompleted DMA (quarantine). It proves nothing about the hardware.

**Next, if it passes:** continuous capture at 1 or 0.5 fps with one capture
buffer + accumulator, merging in the idle part of each frame period (overrun
detection, never silent skips), an LCD preview downscaled from the
accumulator, and a faster packed-RAW14 maximum.

## LCM15/16 camera results

LCM15 (LVFR0000-0003) and LCM16 (LVFR0004-0007): full-sensor readout works at
3, 1, 0.5 (timer B 0xB41B) and 0.33 fps (timer A x3); 5280x3506 raster,
5208x3478 active (left 72, top 28), RGGB, black 2047. The DMA trace shows the
readout (~150 ms x A/0x56B) happens at the END of each frame period and ends
just before the next vsync, so a slurp armed at a vsync completes at the next
one and the buffer is free for period - readout. Vsync jitter < 0.1 ms.
Full-res max merge: 2.2-2.5 s per frame (compute-bound; sparse store saves
little). memcpy 0.47 s. Earlier 0.5 fps failure: my 6 s timeout, not the
camera. Evidence: tests/livecomp/LCM15-camera, LCM16-camera.

## LCM17: continuous full-res composite (Task: Full-res composite)

Same entry/exit as the gate. One capture buffer + one accumulator. The slurp
is re-armed in the same vsync that completes a frame; the worker then has
period - readout to read the frame (first frame: memcpy; then exact max).
After each merge it checks that the next frame's DMA has not started writing
(pointer offset 0) and that no completion was missed; otherwise it stops and
saves ("merge too slow"), never skipping an exposure silently. Default 0.25 fps
(timer A x3, B 30739): 4 s frames, ~3.55 s window vs ~2.4 s merge.

LCD: x5 has no display filter (zebras/filters are off in zoom without core
LV RAW), so the module writes a 720x480 UYVY monitor render of the
accumulator (nearest RGGB quad, fixed gains, 1/2.2 LUT) into Canon's three
LV YUV buffers right after each vsync and after each merge. Canon's own
(garbage) frame can flash briefly. Per-frame CSV: merge time, changed blocks,
DMA offset after merge.

Next: faster merge (word-level SWAR prototype exists but gave only ~15% fewer
instructions; needs a better formulation or assembly) to allow 0.5 fps,
focus-pixel removal, white balance in DNG, JPEG output.

## LCM18 camera result

17 frames merged at 0.25 fps, user-stopped, DNG saved, "worked quite good".
Merge 2.08-2.58 s, preview 0.29 s, no overrun. One partial frame (6 rows) was
merged and logged: arming 40 ms after the vsync did not remove partials.

## LCM19: partial-frame diagnostics, focus pixels, white balance, merge

- **Partial frames.** The module now runs its own RAW slurp (same EDMAC calls as
  the core's `edmac_raw_slurp`) with its own complete/abort/pop callbacks.
  The CSV records, per frame: arm delay after the vsync, which callback fired,
  when (after arming) and the DMA offset at that moment, plus how many ENGIO
  and ADTG writes Canon made during the frame. A partial frame will show
  whether the transfer was aborted, completed early, or coincided with a
  Canon sensor reconfiguration.
- **Focus pixels.** `fullres-fix.h`: the EOS M full-res raster has PDAF pixels
  at (x%24, y%30) in {(0,4),(5,23),(6,18),(11,19),(12,16),(17,23),(18,18),(23,1)}
  for x 72..5279, y 31..3499 - exactly the 200,942 points of MLV App's
  `80000331_5280x3506.fpm`, and 89% of the bright dots measured in our own
  frames. All are R/B sites with clean same-colour neighbours; they are
  replaced by the mean of x+-2 / y+-2 before saving (menu: Focus pixels Fix/Keep).
- **White balance.** DNG AsShotNeutral from Canon's WB setting (Kelvin, presets
  as in mlv_dump, or custom gains) via a 2500-10000 K table computed from the
  EOS M ColorMatrix1; also used for the LCD preview.
- **Merge.** Compare-first SWAR-free variant: extracts the 16 samples of two
  blocks from 7 aligned words and only unpacks/packs when something got
  brighter. qemu ARM946 instruction counts: 81 vs 94 per 8 samples when
  nothing changes, 203 vs 120 when everything changes, so it is used only when
  the previous frame changed <30% of blocks. Expected gain ~10%: **2 s frames
  (0.5 fps) are not reachable with CPU merging** (memory + compute ~2.1 s per
  18 Mpixel frame). The realistic next step for faster cadence is double
  buffering with a third 32 MB buffer, if the camera can provide one.

## LCM19 camera result

8 frames at 0.25 fps, 5700 K, focus pixels fixed (200,942 in 1.13 s), WB and
DNG correct. Two findings:
- The partial frame (frame 5 again, 55,440 bytes = 6 rows, as in LCM18) fired
  the EDMAC **abort** callback 3.39 s after arming = right at readout start,
  with no extra Canon ENGIO/ADTG activity that frame. Something aborts channel
  0x12 once, ~16-20 s after entering x5. Not yet identified; costs one frame.
- Frame 8 (73% changed) took 3.5 s with the adaptive compare-first merge and the
  session stopped on overrun. Compare-first is removed again.

## LCM20

- Merge: sparse CPU max only. An overrun no longer stops the session: before
  each of 16 bands the DMA pointer of the next frame is checked; if the readout
  has already reached the band, the rest of that frame is skipped and counted
  (CSV lost_bands, LOG late_frames). Only a missed completion stops.
- Canon resource registry busy mask logged per frame (after the vsync and in
  the idle window).
- **Hardware max test** (menu HW merge test, default On): once, on frame 3+,
  if all ten engine resources are free, Canon's max engine runs on real data
  with its native output route (channel 5) in two bands (1754+1752 rows), inputs
  = accumulator (already containing the frame) and the captured frame, output
  in place into the capture buffer. The result must equal the accumulator
  byte for byte; mismatches, timing, lock/setup/wait results and DMA traces go
  to the LOG. The composite is never written by the engine. Buffers now keep 4
  halo rows before the pixels, as the engine requires.
- Fixed: per-frame CSV fields were not cleared between sessions.

## LCM20 camera result

16 frames merged (CPU), DNG saved, no late frames. HW test **not attempted**:
Canon resource 5 (native output write channel) was busy in every frame, both
right after the vsync and in the idle window (busy mask 0xa001: 10 resources,
bit0 = id 5). Live View keeps resource 5 permanently in x5 full-res too, as in
normal LV (LCM10). The 6-row aborted frame was frame 5 again (fourth session
in a row), ~32 s after the zoom.

## LCM21

- HW test falls back to the LCM13 replacement route (output on write channel 3,
  private resource set) when only resource 5 is busy. In LCM13 (continuous 3 fps
  normal LV) channel 3 wrote 422 KB then stalled; now the job runs in the idle
  ~1.5 s window of a 4 s full-res frame. Completion wait 600 ms (was 5 s).
- DMA activity map over one full frame period (frame 2 -> 3): state of all 16
  EDMAC write channels, plus channel 3 and 5 pointers and configuration, so the
  next step (e.g. borrowing channel 5 while Live View is not using it) is based
  on measured activity.

## LCM21 camera result

22 frames merged, DNG saved. HW test on channel 3 refused immediately with
LC_NATIVE_ARGUMENT: my plane construction used the allocation start as plane
storage, but the wrapper requires the band to start a whole number of rows
after storage (the halo + guard offset is not). Nothing ran on the engine.
DMA activity map: during the whole idle window write channels 0x01, 0x02, 0x05
and 0x11 are *armed* (state 1, pointer at start; channel 5 at 0x0D119C00,
length 0xE0020, connection 2) and only move when the next readout arrives, and
0x12 is our armed RAW slurp. So channel 5 is not free to borrow in the idle
window: Canon keeps it armed for the next frame. Channel 3 was idle (and
unconfigured) the whole time - consistent with using it for the engine.

## LCM22

Fixed plane layout (`fullres-hw.h`: storage = pixels - 4 halo rows, 4 halo
rows also kept after the band); the exact camera argument checks now live in
`native-max.h` and host tests build the planes from the real allocation
addresses and also assert that the LCM21 construction is rejected.

## LCM22 camera result (LVFR0005)

The planes passed all checks (plane_ok 7) and the engine really ran on frame 3
via channel 3: lock and setup returned 0, both input channels (40, 41)
completed, but output channel 3 completed nothing. After the 600 ms wait it was
still armed (state 1) at its programmed start 0x02009140 with the pointer
unmoved: **0 bytes written**. Reason 96 (WAIT|STOP) -> quarantine -> the session
stopped after 3 frames, and because DMA ownership was "uncertain" the composite
was not saved.

Same picture as LCM13 (inputs fully consumed, output 422 KB then stall), now
with zero output in the idle window. ROM reading: Canon's setup FF429E68 always
calls `ConnectWriteEDmac(state[10], 1)`, i.e. the engine output is source 1
whatever the output channel. The write router (RAM 0x1C6A8) keeps one source
register per *write channel* (0xC0F05000+4*ch, 0xC0F0501C, 0xC0F05200+...), so
several channels can name the same source. Hypothesis: a Live View channel
left armed on source 1 (0x01, 0x02 and 0x11 are armed through the idle window)
competes for the engine output; this would explain consumed inputs with a
missing output in both LCM13 and LCM22.

## LCM23

- Router evidence: the source of all 16 write channels in the idle window
  (`Write_route_idle`) and again after engine setup, plus every other channel
  on source 1 with its state, address and pointer before and after the job.
- Divert (on by default in the HW test): other listeners of source 1 are moved
  to an unused source (lowest >= 3 nobody names) with Canon's
  ConnectWriteEDmac just before StartEDmac, and restored right after the wait
  (~1 s before the next readout). Restore is verified; a failed restore
  quarantines (reboot).
- Stuck output recovery: if only channel 3 is still armed and still holds our
  own start address, it is aborted (AbortEDmac, RAM 0x1CA60) and the session
  continues instead of being quarantined. Channel 3 is never configured by
  Live View (LCM21/22 activity map).
- If the HW test does leave DMA uncertain, the accumulator (never an engine
  target) is still saved; only the buffers stay pinned until reboot.

## LCM23 camera result (LVFR0006-0009)

0006/0007: x5 zoom did not engage (24 fps vsync, PathDriveMode 1,4,6: camera
not in the expected photo LV mode); nothing hooked, nothing unsafe.
0008 (9 frames) and 0009 (8 frames): composites saved, focus pixels fixed.
HW test on frame 3, channel 3, identical in both runs:

- Write sources after setup: 0:2e 1:1b 2:4 **3:1** 5:2 6:12 0x10:28 0x11:f
  0x20:10 0x21:27 (all other channels 0). Idle window identical except ch3=0.
  **No other channel listens on source 1** - the competing-listener
  hypothesis is ruled out; nothing was diverted.
- Inputs 40/41 completed, output channel 3 armed on source 1 at its own
  address, 0 bytes written in 600 ms (reason 32 = WAIT).
- Stuck-output abort worked: aborted=1, channel idle, context restored,
  session continued safely (safe=1) and saved.
- Partial frame (EDMAC abort, 55,440 bytes) again once per session (frame 8
  in 0008, frame 5 in 0009), unrelated to the HW test timing.
- CPU merge 2.1-2.5 s per frame almost independent of the changed fraction
  (0-78 %): the cost is unpack/pack and memory traffic, not the writes.

Conclusion: in this Live View state the engine consumes both inputs but emits
nothing on source 1, whatever the output channel and with no competing
listener. Native-engine attempts LCM9-LCM13 and LCM21-LCM23 produced no
verified output.

## LCM24

- **Select-max CPU merge** (`lc_raw14_max_sel`, raw14-max.h): no unpack or
  repack. Both buffers share the packed layout, so the max is a per-field
  choice of bits; each 14-bit field is compared in place after one shift
  (ties are harmless), a mask selects the brighter bits, and a word is
  written only if it changed (unchanged cache lines stay clean). ARM946
  instruction count per 16 pixels (qemu): 156 all-changed / 114 unchanged,
  vs 239 / 194 for the LCM16-23 sparse kernel. Bit-exact against the
  reference on host and on emulated ARM946, including ties and edge values.
- Merge in 8 bands instead of 16 (the 10 ms scheduler ticks cost 160 ms).
- Preview is skipped on frames where rendering it (~420 ms) would push the
  next merge into the next readout (`Merge_kernel_bands_preview_skipped`).
  Goal: 0.5 fps (2 s, timer A 0x56B, readout ~150 ms).
- Engine experiment (HW test unchanged otherwise): every ENGIO register list
  written while the band-0 job runs is recorded with its time, address and
  first registers - Canon's own start/stop tables and anything Live View
  writes meanwhile - to see whether Live View reprograms the engine blocks
  (0xC0F08000/060/070, 0xC0F0D000/034) during the job.

## LCM24 camera result (LVFR0010 0.5 fps, LVFR0011 0.25 fps)

- **0.5 fps works**: 15 frames, 0 late, 0 lost bands, DNG saved. New merge
  1.57-1.77 s per frame (LCM23 sparse kernel: 2.1-2.5 s), first frame (copy)
  0.55 s. At 0.25 fps 1.47-1.87 s. The last band sometimes races the next
  readout (offset after merge up to 16.5 MB) but never loses data.
- **No live preview at 0.5 fps**: skipped on 14/15 frames (render 263 ms +
  copy 113 ms did not fit before the next readout), so the screen kept the
  first render.
- Engine (0011): ENGIO lists during the job are only Canon's own config
  (0x55020, 43 regs), start (0x55180) and stop (0x551b0) tables. **Live View
  writes nothing during the job.** Output channel 3 again 0 bytes. With no
  interference and no competing listener, the engine track stops here: HW
  test kept as an Off-by-default diagnostic.
- Partial frame (EDMAC abort at readout start) again on the **5th frame** of
  both sessions (0.5 fps: 10 s in; 0.25 fps: 20 s in) - tied to the frame
  count, not time.
- 0011 ended with FE_STALL after a long SET press (the first, short press
  was registered but its "Stopping" message was overwritten by the frame
  counter). Composite saved.

## LCM25

- Preview render rewritten: column table computed once (no division per
  pixel), only the 4 samples of each RGGB quad extracted (bit-exact vs the
  full unpack), one UI tick instead of eight.
- Preview deadline is the next frame's arrival, not the readout start: it
  reads only the accumulator, so overlapping the readout (which writes the
  capture buffer) is safe. Estimate uses the measured render+copy time.
- Stop feedback: Q/SET also checked during the merge; once stopping, the
  messages say so until the DNG is saved.
- New 0.4 fps (2.5 s, timer A 0x56B, B 57635) choice. HW merge test now
  defaults to Off.

## LCM25 camera result (LVFR0012-0015)

- **0.4 fps (2.5 s) works with live preview**: 23 frames, preview rendered
  every frame (0 skipped), merge 1.54-1.90 s, stopped cleanly by one short
  SET press. Render 124-133 ms (LCM24: 263 ms), copy ~110 ms.
- **0.5 fps (both variants) stopped after frame 2** (FE_DMA): the first max
  merge saw 99 % changed pixels and took 1.89 s; with the 110 ms preview
  copy at the frame start, the next frame completed during the merge and
  LCM20-25 treated that as fatal. (LCM24's 0.5 fps run had 62 % changed:
  1.77 s, fitted.)
- Partial frame again on the 5th frame of the 0.4 fps session.

## LCM26

- A frame completing during the merge is no longer fatal: it is merged
  next, without preview (`behind` counter). Frames that arrive while the
  worker is behind are skipped and counted (`missed`); the session stops
  only if more than 2 + limit/4 are skipped. Rows overwritten during the
  last band hold a mix of two real exposures - harmless for a maximum.
- The 40 ms pre-arm sleep is dropped when the preview copy (~110 ms)
  already separates the arm from the vsync.
- Default full-res FPS is now 0.4 fps (2.5 s).

## LCM26 camera result (LVFR0016, 0.5 fps)

18 frames, 0 behind, 0 skipped, merge 1.58-1.85 s, DNG saved. Preview
skipped on 17/18 frames: after the merge (~done + 1.7-1.97 s) a full render +
copy (~260 ms) never fits before the next frame. Partial frame at frame 8.

## LCM27

- Rolling preview: when a full preview does not fit, one quarter strip of
  the screen is rendered per frame (cycling, ~35 ms); the copy that hides
  Canon's frame at the next vsync puts it on screen. Counted as `strips`.
- Engine, in Live View: every write channel's state and pointer just before
  start and after the wait (`HW_band0_moved`), to see whether the output
  lands on any other channel.
- Engine, **Live View paused** (`HWP_*`): the only verified engine output
  (LCM1) ran with Live View stopped. After the session, with zoom and hooks
  restored and before the DNG save (accumulator unsaved but never an engine
  target), PauseLiveView, run the same two-band max on the real buffers
  (channel 5 if resource 5 is free, else 3), verify byte for byte against
  the CPU composite, ResumeLiveView. Runs only with "HW merge test" On.

## LCM27 camera result (LVFR0017, LVFR0018, 0.5 fps) - ENGINE VERIFIED

- **With Live View paused, Canon's max engine is exact and fast.** Busy mask
  0xA000 (resource 5 free), output on Canon's channel 5, both bands
  completed in 57.6 + 58.1 ms (0017) and 58.0 + 57.6 ms (0018): **~116 ms
  per full 5280x3506 frame, 0 mismatching bytes** against the CPU composite,
  both sessions. Live View resumed normally. CPU merge: 1.6-1.9 s.
- So the job, planes, geometry and verification are right; what blocks it
  in Live View is Live View's state (resource 5 held, channel 3 receives
  nothing).
- In-LV attempt did not run: at 0.5 fps the gating (elapsed + 800 <
  period - 500) never held.
- 0.5 fps sessions: 22 and 21 frames, 0 skipped, 1 behind; rolling preview
  drew 19 and 16 strips (2 and 4 skipped).
- Byte-wise verification took 3.7 s per frame (now word-wise).

## LCM28

Question: is it the output *channel* or Live View's *state*?
- Engine accepts output channels 0/3/4/5/6; stuck-output abort applies to
  any channel still holding our own programmed address.
- Attempt table (`HW_tries`): where (LV/paused), frame, channel, bands,
  busy mask, reason, time, output completions, output state and bytes
  written, abort, other channels that moved, mismatching words.
- Output area cleared before every attempt (no stale pass).
- In Live View (needs 0.25 fps for time): frames 3, 4, 5 try channels 4, 0,
  3 on the top half.
- Live View paused: channel 5 full frame (reference), then 3, 4, 0 top half.

## LCM28 camera result (LVFR0019, 0.25 fps)

Attempt table:
- LV ch4: no output, stuck, aborted (safe). LV ch0: resource busy (not run).
  LV ch3: no output, aborted (as always).
- Paused ch5 (2 bands): completed, 117 ms, wrote the full 32,395,440 bytes -
  but every word was 0. Cause: LCM28's new zero fill of the output area,
  which is also the engine's second input. With real data (LCM27) the
  output was exact; Canon's engine maps an all-zero input to 0 (values far
  below the 2047 black level are not plain integers to it).
- Paused ch3: no output and the abort did not idle it -> quarantine
  (composite saved, reboot requested).

Conclusion: **the engine output (source 1) reaches only write channel 5**,
with or without Live View. Channels 3/4 never receive it.

## LCM29: borrow channel 5 from Live View

From ROM (RAM 0x1C914/0x1CA60/0x1CB00/0x1CBE8): Canon keeps a 32-byte record
per EDMAC channel (table pointer at 0x1C404): +0 abort flag, +4 control word
(SetEDmac flags | StartEDmac mode<<6), +8/+12/+16 complete/abort/pop CBR,
+20/+24/+28 contexts; default CBR at *(0x1C830). Live View's channel 5:
control 0x20000040 = EDMAC_2_BYTES_PER_TRANSFER | mode 1, source 2, armed
and idle from ~0.3 s after each vsync until the next readout.

`lc_native_borrow5` (only if armed, idle, source 2, record consistent):
save address, geometry (edmac_get_info), control word, source and record;
point the abort CBR at the default; AbortEDmac(5); wait idle.
Job: `lc_native_engine_max_lock(..., channel 5, lock set of channel 3)`
(Live View keeps resource 5). `lc_native_return5`: ConnectWriteEDmac(5,
saved source), SetEDmac(saved address, geometry, control & ~0xC0), restore
the full record, StartEDmac(5, saved mode); verify state 1, address,
pointer at start, control word and source. Any failure quarantines.

Test (HW merge test On, 0.25 fps): borrow on frames 3, 4, 5 (full frame,
word-verified after channel 5 is returned); CSV records Live View's
channel 5 state/address at every frame to show it keeps working. Paused
test reduced to the channel-5 reference. Zero fill removed.

## LCM29 camera result (LVFR0000 after card cleanup, 0.25 fps)

- B0 (frame 3): refused correctly - channel 5 was not armed (state 0,
  pointer 0x1E0 past start). Live View double-buffers channel 5
  (0x0D116000 / 0x0D119C00) and is not always armed in the idle window.
- B1 (frame 4): borrow worked (abort idle in 1 us), job ran with output on
  channel 5 (source 1, lock set 3), inputs completed, **channel 5 wrote 0
  bytes**; stuck output aborted; channel 5 returned and verified identical
  (state 1, 0x0D119C00, control 0x20000040, source 2).
- B2 (frame 5): AbortEDmac did not idle channel 5 within 20 ms -> quarantine,
  composite saved, reboot.

Conclusion: the engine produces output only with **channel 5 and Live
View paused**. In Live View no channel (3, 4, 5) receives it. Borrowing
channel 5 is not enough and is risky (B2); it is disabled.

## LCM30: register-shadow diff, Live View running vs paused

Canon's ENGIO shadow (shamem_read = FF2C1730, RAM lookup of the last value
written through EngDrvOut/ENGIO, no peripheral access) over 0xC0F08000-
0xC0F08FFC, 0xC0F0D000-0xC0F0D0FC (engine blocks) and 0xC0F05000-0xC0F052FC
(EDMAC routing): snapshot on frame 3 (Live View running, no engine job) and
after PauseLiveView (before the channel-5 reference job). The engine job's
own config list (RAM 0x55020) is logged after the paused job. The log lists
every register that differs, flagged by whether the job list rewrites it:
differing registers the job does not rewrite are the candidates.
No engine job runs in Live View.

## LCM30 camera result (LVFR0001, 0.25 fps)

7 frames, saved. Paused channel-5 reference again exact (115 ms, 0
mismatches). Engine job config list (RAM 0x55020): 43 registers in
0xC0F08000-0x2A4 and 0xC0F0D000-0x0AC. Register-shadow diff, Live View vs
paused, over 1280 registers: **9 differ, none rewritten by the job**:

| register | Live View | paused |
|---|---|---|
| C0F08020 C0F08090 C0F08130 C0F08180 C0F08540 C0F08D68 | 1 | 0x80000000 |
| C0F083B0 C0F08598 C0F08D5C | 1 | 0 |

1 = block started; 0x80000000 = Canon's stop value (the job itself writes
it to 0xC0F08000/060/070 before starting them). Pausing Live View stops
these 9 Live View blocks in the engine's register area; with them running,
the engine output never reaches source 1.

## LCM31

Frames 3 and 4: borrow channel 5 (LCM29 procedure), set the 9 registers to
their paused values through Canon's EngDrvOut (FF2C1694, shadow kept),
run the full-frame job, restore Live View's 9 values (reverse order),
return channel 5, verify byte for byte. Logged: values saved/after-restore
per attempt and how often Live View writes each of the 9 through ENGIO
(if it does every frame, Live View re-establishes them itself).

## LCM31 camera result (LVFR0002 + CRASH00.LOG)

- Frame 3: borrow refused (channel 5 not armed), as in LCM29.
- Frame 4: channel 5 borrowed, the 9 Live View registers set to their paused
  values, engine job run: **still 0 bytes** on channel 5; registers restored
  (1 > 1) and channel 5 returned verified. Live View continued.
- Live View itself writes 7 of the 9 registers through ENGIO about every
  frame (10-15 times per session): they are per-frame settings, not the
  blocker.
- After the session, during the DNG save: **camera crash**, ASSERT in
  KernelDry/KerQueue.c:555 from interrupt 0x6A (message queue overflow in
  an ISR) - almost certainly a delayed effect of restarting Live View's
  blocks / handing channel 5 back. Black screen, battery pull. Log stops at
  frame 4; DNG incomplete (15 MB).

**Hardware engine conclusion (LCM9-LCM31):** exact and ~116 ms per full
frame, but only with Live View paused and output on channel 5. In Live View
no channel receives its output - other channels, a borrowed channel 5, and
stopping the 9 differing Live View blocks all fail. Pausing Live View per
merge would leave seconds of gap per sub-exposure, worse than the
overlapping CPU merge. Product path: CPU merge.

## LCM32: CPU product path + read-only diagnostic

- Runtime: all Live View engine experiments removed (borrow, block
  stop/restore, attempts). The native borrow/engdrv functions stay in
  native-max.c, unit-tested but unused.
- "HW diagnostic" (default Off) = "Register diff": the whole ENGIO shadow
  window (FF2C1694/FF2C1730 show it is one flat 256 KB RAM copy of
  0xC0F00000-0xC0F3FFFF, read without touching peripherals) is copied on
  frame 3 and compared after PauseLiveView, up to 2048 differences logged
  (flagged if the engine's job list rewrites them), then the paused
  channel-5 reference job runs as before (5 safe runs so far).

## LCM32 camera result (LVFR0003): whole-window register diff

8 frames, saved; paused channel-5 reference exact again (115 ms). Whole
ENGIO shadow window 0xC0F00000-0xC0F3FFFC, Live View running vs paused:
**227 registers differ**, none in the engine job list:
- 0xC0F03xxx (102): values like 0x80130000, 0x80160002, 0x801C0004 in Live
  View, 0 when paused (plus a few 0 -> 1). Looks like the image pipeline's
  routing table (enable | source block id << 16 | port): Live View's block
  connections, all cleared when paused. Prime suspect.
- start/stop flags (1 vs 0x80000000 or 0) in 0xC0F02/08/09/0B/10/11/1A/2C/
  35/37/38/3A/3B/3Dxxx; 0xC0F2F9xx (8 vs A).
- 0xC0F01000-054 (clock-like values, 0x2405B6E7 vs 0x8400).
- EDMAC (0xC0F04/26/30xxx) and sensor timing (0xC0F06/07xxx): expected.

## LCM33: escalating Live View test

`lv-pause-regs.h` (generated from LVFR0003): 189 registers with their
paused values, EDMAC and sensor timing excluded. Group A = the 102
routing-table registers, B = A + 75 start/stop flags, C = B + 12 0xC0F01xxx.
On frames where Live View's channel 5 is armed and idle: borrow channel 5,
save Live View's values, write the group's paused values (EngDrvOut), run
the full-frame job, restore Live View's values in reverse order and check
them, return channel 5, verify. Order A, then B, then C (a refused borrow
retries the same step); stop at the first exact result. lc_native_engdrv
now accepts 0xC0F01000-0xC0F3FFFC except EDMAC and sensor timing pages.

## LCM33 camera result (LVFR0004)

- Step A (frame 4): channel 5 borrowed, the 102 routing-table registers set
  to their paused values, job run: **0 bytes** on channel 5 again; values
  restored exactly (0 bad), channel 5 returned verified. The user saw
  repeating text characters at the top of the screen (display routing
  disturbed during the step).
- Step B (frame 5): borrow abort did not idle channel 5 (20 ms) ->
  quarantine, composite saved. Same as LCM29's frame-5 borrow: on the
  session's 5th frame (also the frame of the recurring partial-frame abort)
  Live View's channel 5 does not stop.
- ROM (FF2C5AC0 / FF2C4BF4): locking a resource enables its *type's* power
  module (reference-counted; table at FF8FFAB8), not the individual
  channel. Locking with resource 3 vs 5 is equivalent, so the lock set is
  not why borrowed channel 5 fails; channel 3 failing while paused is real.

## LCM34: reverse test with Live View paused

No Live View steps. After PauseLiveView and an exact channel-5 reference,
each register group gets its Live View value back (from the frame-3
snapshot) for one job, then its paused value: where 10 = routing table (A),
11 = start/stop flags (B), 12 = 0xC0F01xxx (C). A group that breaks the
working paused job is what blocks the engine in Live View; if none does,
the blocker is outside the ENGIO shadow (clock/power or other state).

## LCM34 camera result (LVFR0005)

Paused reference exact (120 ms). Reverse test, each group alone given its
Live View value: routing A (102) 116 ms exact, start/stop B (75) 119 ms
exact, 0xC0F01 C (12) 115 ms exact. **No single group breaks the engine.**

## LCM35

Same reverse test with all groups at once (where 13 = A+B+C) and with Live
View's EDMAC routing differences added (where 14, 0xC0F05xxx, written with
EngDrvOut directly and restored). Also timed: PauseLiveView and
ResumeLiveView (pause_ms, resume_ms), to size the "pause Live View per
merge" option if the blocker is not register-visible.

## LCM35 camera result (LVFR0007)

Paused reference exact (118 ms). All 189 registers given their Live View
values at once: exact (116 ms). Plus Live View's EDMAC routing: exact
(119 ms; no 0xC0F05xxx entries actually differed). **The register state of
Live View (everything Canon's ENGIO shadow records) does not stop the
engine.** PauseLiveView took 202 ms, ResumeLiveView 705 ms (to Live View
running, before full-res mode is re-entered): a pause per merge would cost
at least ~0.9 s of no exposure per frame.

(Deployment note: LCM35 first failed to reach the card because the copy
tool reused an earlier upload of the same staged file name; each build is
now staged under its own name and read back from the card.)

## LCM36: display off

What runs only while Live View is active is its activity, most visibly the
LCD refresh (display DMA keeps running between the slow full-res frames).
display_off() after frame 3; on frame 4 (or 6; never 5, where borrowing
fails) one borrowed channel-5 job with no register changes; display_on().
Paused reference after SET as before; reverse tests off.

## LCM36 camera result (LVFR0008 + CRASH01.LOG)

- Display turned off after frame 3 (1.9 s after frame done); frame 4:
  channel 5 borrowed (abort 1 us), job with no register changes: **0
  bytes** again; channel 5 returned verified. The LCD refresh is not the
  blocker.
- Crash right after SET: `ASSERT !IS_ERROR(TryPostEvent(..., EV_VD_INTERRUPT_EVF))`
  at Evf/EvfState.c:504 in interrupt 0x6A. Interrupt 0x6A is Live View's
  per-frame VD interrupt; the Evf task's queue was full, i.e. the Evf task
  was stuck. LCM31's crash (KerQueue.c:555, also INT-6Ah) is the same
  event. Both sessions borrowed channel 5 on frame 4 and reached the
  session end (mode change); LCM29/33 quarantined earlier and did not
  crash. Borrowing channel 5 leaves Live View's internal state wrong in a
  way none of our checks see. DNG incomplete.

## LCM37

All Live View engine work removed from every build: no borrow, no display
toggling, no register writes in Live View. "HW diagnostic" keeps only the
whole-window register snapshot and the paused channel-5 reference, both
without incident in every run.

## Hardware engine: summary (LCM9-LCM36)

Works, exactly and in ~116 ms per full frame, only with Live View paused
and output on channel 5. In Live View: no output on channels 3/4/5 (5
borrowed), with Live View's routing cleared, its blocks stopped, or the
display off. Live View's complete register state reproduced while paused
does not break the engine, so the blocker is Live View's running activity,
which is not visible in or controllable through the register shadow.
Pausing Live View costs 0.2 + 0.7 s plus re-entering full-res mode per
merge, i.e. ~1.5 s or more without exposure per frame. The CPU merge
already fits inside the frame period with no exposure gap at 0.4-0.5 fps.

## ROM study (LCM37-38, offline, ROM1 dump from the camera)

- Live View x5 runs several Canon "paths" (debug strings `[PATH] LVx5_*`):
  VramPath (display image; `(LVx5)VRAM[W]` - its buffers are Live View's
  channel-5 addresses 0x0D119C00/0x0D116000), YuvPath, FencingPath, and
  MEM1 (Canon's LV RAW dump: FF500180-FF5003A0 ConnectWriteEDmac(0x12, 0),
  SetEDmac(0x12, ..., 0x20000000), StartEDmac(0x12, 2) - the same channel
  and source ML's raw slurp uses). Per-frame statistics paths: HistLCG,
  Flicker, ObInteg, WbInteg (AE/WB), Af, and RamClear (run by the Evf VD
  handler around frame 4 - "evVdInterrupt LastRamClear Start", a likely
  cause of the recurring 5th-frame abort of channel 0x12).
- Every ConnectWriteEDmac call site in ROM decoded (channel, source):
  **only the engine setup (FF42A04C) ever uses source 1.** Live View paths:
  ch1 <- 27, ch2 <- 4, ch16 <- 40, ch0x12 <- 0 (per mode). No Live View
  path competes for the engine output.
- Resource locks enable per-type power modules (FF2C5AC0/FF2C4BF4).
- The crash handler: Evf VD interrupt 0x6A posts EV_VD_INTERRUPT_EVF
  (EvfState.c:504) - Live View's per-frame state machine.

Working hypothesis: the engine's output stage is paced by the sensor timing
generator (0xC0F06004 = 1 in Live View, 0 paused), the only differing
register group never restored in the reverse tests. Fits LCM13 (trickle at
3 fps), the idle-window zero output at 0.25 fps, and paused exactness.

## LCM38: timing-generator reverse test (paused)

After the exact paused reference: give 0xC0F06004/0xC0F07000 (enable bits)
their Live View value, run the job, restore (where 16); if still exact,
give every differing 0xC0F06/07xxx register its Live View value (where 15).
A broken job confirms the timing generator as the gate.

## LCM38 camera result (LVFR0009)

Paused reference exact (115 ms). Timing-generator enable bits (0xC0F06004,
0xC0F07000) at their Live View value: exact (116 ms). All 15 differing
0xC0F06/07xxx registers at their Live View values: exact (116 ms).
PauseLiveView 194 ms, ResumeLiveView 712 ms.

Caveat: Live View holds its own engine resources (ROM strings
"LockEngineResources HEAD", "LockEngineResources( hRscLckLv )"); pausing
releases them, which can power-gate the timing generator, so writing its
enable bit while paused may have no effect. The conclusion that holds:
**no register value recorded in Canon's ENGIO shadow, alone or combined,
reproduces the Live View blockage.** What remains is power/clock/resource
state owned by Live View's running paths, which only PauseLiveView
releases.

## LCM39: resource-registry diff (read-only)

`lc_resource_enum` walks every node of all 16 resource types in Canon's
registry (manager 0x4C3F8, same fingerprinted layout as FF2C5BF8; bounded,
signature-checked, no writes). Snapshots: frame 3 (Live View running) and
after PauseLiveView, before the reference job. Logged: every locked
resource in each state and the full Live View node list. Shows which
hardware locks Live View holds that pausing releases.

## LCM39 camera result (LVFR0000 after card cleanup)

Registry: 179 resource nodes in both states. **Live View holds 64 locks,
paused 0.** Of the engine's ten resources Live View holds only 5. Live View
holds locks in types 0-7, B, C, E, F; the engine uses types 0, 1, 2, 3, 5,
8. A lock powers its type's domain (FF2C5AC0 -> FF2C4BF4, refcounted
power module per type, table FF8FFAB8). So while Live View runs the
domains of types 4, 6, 7, B, C, E, F are powered; paused they are off -
and register writes into an unpowered block (LCM34-38 reverse tests) do
nothing. Paused engine exact again (117 ms); timing-generator variants
exact again.

## LCM40: power reverse test (paused)

After the exact reference, for each type (4, 6, 7, B, C, E, F, then the
extra Live View locks in the engine's own types 2, 3, 5, 0, 1): create and
try-lock Live View's own locks of that type (minus the engine's ten) with
Canon's functions (FF2C5848/FF2C6228), run the reference job (HW_tries
where 0x20 + type), unlock and delete (FF2C5EB4/FF2C5A60). A type whose
power breaks the job is the blocker.
