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
