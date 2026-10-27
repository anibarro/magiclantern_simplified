# Live Composite development

## Scope and module conventions

The module provides an Olympus-style live composite at full sensor resolution
(5208x3478) on the EOS M 2.0.2: an exposure preview, then gapless frames of the
chosen shutter merged into an exact per-pixel RAW maximum, an LCD preview of the
composite and a RAW DNG. EOS M firmware 2.0.2 is the only supported camera.
The first versions (LCM1-LCM14) composited Live View resolution frames at 3 fps;
that mode was removed in LCM43. FULLRES.md is the design log of the full-res work.

| File | Role |
| --- | --- |
| `livecomp.c` | module callbacks, camera guard, EOS M Q/SET adapter |
| `livecomp-lv.c` | menu, start/stop keys, worker task |
| `fullres-lv.c` | full-sensor session: hooks, x5 switch, RAW slurp, preview, merge, DNG/LOG/CSV |
| `fullres-regs.h` | CROP_Full_Res register values, shutter (timer) table, list rewriting |
| `raw14-max.h` | packed RAW14 select-max merge kernel (+ reference maximum for tests) |
| `fullres-fix.h` | EOS M focus-pixel fix, DNG white balance from Canon's WB |
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
   inert outside x5 and removed at the end of the session.
5. Shared structure sizes and every accessed member offset are compared between
   the generic module and the EOS M core by the ARM ABI test. Do not enable another
   camera merely by changing the initialization guard.

## Session and ownership

The worker checks the camera state and fingerprints, allocates two 32 MB buffers
(capture with guards, accumulator), installs the hooks and requests x5 zoom;
Canon then configures the full-sensor readout through the hooks. Our own RAW
slurp (EDMAC 0x12) is armed from the worker after every frame. Each completed
frame is checked (DMA idle, end pointer, guards) and then either shown (exposure
preview) or merged in 8 bands while the next exposure runs. Stop finishes the
exposure in progress; the session then leaves x5, removes the hooks, frees the
capture buffer, fixes the focus pixels and writes the DNG. If the RAW DMA does
not finish, the buffers stay allocated and Status asks for a reboot.

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
table; a simulated camera session (switch, capture, preview, merge, every
shutter, no limit, partial/late/skipped frames, battery stop, refusals, DMA
quarantine); merge kernel exactness; focus-pixel fix and WB mapping; DNG
header and RAW14 byte order; SET/half-shutter gating; camera rejection and ARM
structure layouts. They do not establish hardware behaviour.

## Evidence

Live View resolution: LCM8 captured 76/76 frames with LCD updates and a DNG.
Full resolution: camera sessions LCM23-LCM42 with 2, 2.5 and 4 s frames, exact
merges, LCD preview, focus-pixel fix and WB (logs in tests/livecomp/). Canon's
hardware max engine was investigated (LCM9-LCM40) and found unusable while
Live View runs; see FULLRES.md.

## Provenance and contribution status

This implementation and its checks contain substantial code generated and guided
by OpenAI Codex (LCM1-LCM14) and Anthropic Claude (LCM15 onwards). They developed
from the user's earlier Live Composite experiments and the ML RAW APIs. Source is
marked GPL-2.0-or-later.

The repository's `LLM_POLICY.md` excludes substantial directly generated code from
upstream contributions. This local commit is not a claim of upstream eligibility
or maintainer acceptance; disclose the generation history in any discussion.
