# Live Composite development

## Scope and module conventions

The module provides an Olympus-style live composite at full sensor resolution
(5208x3478) on the EOS M 2.0.2: an optional one-frame preview, then gapless frames of the
chosen shutter merged into an exact per-pixel RAW maximum (Live Composite) or
sum (Live Bulb, LCM54), an LCD preview of the result and a RAW DNG, with
column correction (Live Bulb) and optional dark-frame subtraction (LCM56-LCM61).
EOS M firmware 2.0.2 is the only supported camera.
The first versions (LCM1-LCM14) composited Live View resolution frames at 3 fps;
that mode was removed in LCM43. HOW-IT-WORKS.md describes the implementation,
FULLRES.md is the chronological design log, HARDWARE-MERGE.md the investigation
of Canon's image engine.

| File | Role |
| --- | --- |
| `livecomp.c` | module callbacks, camera guard, EOS M Q/SET adapter |
| `livecomp-lv.c` | menu, start/stop keys, worker task |
| `fullres-lv.c` | full-sensor session: hooks, x5 switch, warm-up, RAW slurp, preview, merge, dark frames, DNG (DCIM/LIVE), LOG/CSV with Debug ON |
| `fullres-regs.h` | CROP_Full_Res register values, shutter (timer) table, list rewriting |
| `raw14-max.h` | packed RAW14 select-max merge kernel (+ reference maximum for tests) |
| `raw14-add.h` | packed RAW14 sum kernel (Live Bulb) and subtract kernel (dark frames) |
| `fullres-fix.h` | column correction, EOS M focus-pixel fix, DNG white balance from Canon's WB |
| `fullres-gamma.h` | preview gamma table |
| `lv-dng.h` | DNG header writer |
| `camera-profile.h` | runtime camera selection |

The implementation follows the module boundaries described in
[the maintainer's guidance](https://www.magiclantern.fm/forum/index.php?topic=27459.msg248987#msg248987):

1. No platform headers or camera-specific compilation flags are used by the module.
2. `camera-profile.h` selects the RAW channel, cache alias, idle GUI state, the
   Q/SET events and the LV YUV buffers only after checking model and firmware.
   Unsupported cameras fail initialization before menus, tasks or hardware work.
3. Regular keys use `CBR_KEYPRESS` and `MODULE_KEY_*`. A small
   `CBR_KEYPRESS_RAW` adapter handles EOS M's combined Q/SET, absent from this
   core's portable translation table, through the shared `struct event` only.
4. Core services are runtime-resolved exports; no patched core or dependent
   modules. While a session runs, four fingerprinted sensor-configuration
   functions (CMOS, ADTG, ENGIO lists, EngDrvOut) are hooked; the hooks are
   inert outside x5 and removed at the end of the session. A fifth hook on
   Canon's AbortEDmac only records calls on the RAW channel (diagnostics).
5. Shared structure sizes and every accessed member offset are compared between
   the generic module and the EOS M core by the ARM ABI test. Do not enable another
   camera merely by changing the initialization guard.

## Session and ownership

The worker checks the camera state and fingerprints, allocates two 32 MB buffers
(capture with guards, accumulator), installs the hooks and requests x5 zoom;
Canon then configures the full-sensor readout through the hooks. After a
12-frame warm-up at 3 fps (throwaway captures), our own RAW slurp (EDMAC 0x12)
is armed from the worker after every frame. Each completed frame is checked
(DMA idle, end pointer, guards) and then either shown (exposure preview) or
merged in 8 bands while the next exposure runs. Stop finishes the exposure in
progress; with Dark frame ON the dark frames follow. The session then waits
until the sensor runs short frames again, leaves x5, removes the hooks,
applies the column correction, frees the capture buffer (or uses it to read
back the unprocessed DNG), fixes the focus pixels and writes the DNG. If the
RAW DMA does not finish, the buffers stay allocated and Status asks for a
reboot.

The standard EOS M core consumes idle physical Q/SET down before module dispatch,
but forwards physical SET up; the raw callback handles that release. A seen press
(menu or synthetic long press) suppresses its physical release. Capture gates
prevent menu/playback/Off/half-shutter starts.

## Build and checks

With the standard ML ARM toolchain and host C compiler installed:

```sh
make -C modules/livecomp
bash build_tools/build_livecomp.sh
```

The second command runs the host checks, builds the module and an ordinary
EOSM.202 core symbol table, then verifies imports and packages a module-only ZIP.
The core is for checking only; it is not included in the ZIP. Windows users may
run `RUN_LIVECOMP_BUILD.cmd` with the toolchain in WSL. Scripts use LF endings.

Checks: register rewriting against the crop_rec reference and the shutter
table; a simulated camera session (switch, warm-up, capture, preview, merge,
every shutter, no limit, partial/late/skipped frames, battery stop, refusals,
DMA quarantine, Live Bulb sessions, dark frames in place / with Save
unprocessed / skipped, composite dark frame, SET before the first long frame
with the late timer latch, a simulated Canon abort inside and after the
warm-up); merge, sum and subtract kernel exactness; column correction
(synthetic sensor with lanes, noise, gradient, bright line and a dark edge);
focus-pixel fix and WB mapping; DNG header and RAW14 byte order;
SET/half-shutter/button-command/touch gating; camera rejection and ARM
structure layouts. They do not establish hardware behaviour.

## Evidence

Live View resolution: LCM8 captured 76/76 frames with LCD updates and a DNG.
Full resolution: camera sessions LCM23-LCM52 with 2-5 and 10 s frames, exact
merges, LCD preview, focus-pixel fix and WB; LCM55-LCM61 Live Bulb (3-50 s),
column correction, dark frames and the composite dark frame (logs in
tests/livecomp/LCMnn-camera).
Canon's hardware max engine was investigated (LCM9-LCM40, LCX1-LCX2) and cannot
merge while the sensor runs; see HARDWARE-MERGE.md. The experimental code is
kept, unbuilt, in research/livecomp-hwx/.

## Core compatibility

The module needs no change to the ML core. It builds against the unmodified
upstream `magiclantern_simplified` dev branch (verified at 142472e95), and
`build_tools/build_livecomp.sh` checks that every import exists in an EOSM.202
core built from that tree and that the shared structure layouts match. The two
repository changes outside the module only add it to the build lists
(`modules/Makefile`, `platform/EOSM.202/modules.included`). Camera tests so far
ran with a locally built EOSM.202 core, not with an official nightly.

## Provenance and contribution status

This implementation and its checks contain substantial code generated and guided
by OpenAI Codex (LCM1-LCM14) and Anthropic Claude (LCM15 onwards). They developed
from the user's earlier Live Composite experiments and the ML RAW APIs. Source is
marked GPL-2.0-or-later.

The repository's `LLM_POLICY.md` excludes substantial directly generated code from
upstream contributions. This local commit is not a claim of upstream eligibility
or maintainer acceptance; disclose the generation history in any discussion.
