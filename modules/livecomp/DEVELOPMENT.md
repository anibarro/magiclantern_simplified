# Live Composite development

## Scope and module conventions

The module provides continuous Live View RAW maximum blending (Task: Live
composite) and a full-sensor composite (Task: Full-res composite, LCM17-LCM41,
see FULLRES.md), with LCD preview, DNG output, ON/OFF settings, short/long SET
start/stop and reusable sessions. EOS M firmware 2.0.2 is the only supported
camera. This is an experimental module, not a guarantee of zero sensor dead time.

The implementation follows the module boundaries described in
[the maintainer's guidance](https://www.magiclantern.fm/forum/index.php?topic=27459.msg248987#msg248987):

1. No platform headers or camera-specific compilation flags are used by the module.
2. `camera-profile.h` selects the RAW channel, cache alias, idle GUI state and
   combined Q/SET event only after checking the camera model and firmware.
   Unsupported cameras fail initialization before menus, tasks or hardware work.
3. Regular keys use `CBR_KEYPRESS` and `MODULE_KEY_*`; display preparation uses
   `module_send_keypress`. A small `CBR_KEYPRESS_RAW` adapter handles EOS M's
   combined Q/SET, absent from this core's portable translation table. It accesses
   only the shared `struct event`, with the event number selected at runtime.
4. RAW, FPS, display and allocation services are runtime-resolved core exports.
   No patched core or dependent modules are required. Live composite uses no ROM
   hooks. Full-res tasks temporarily hook four fingerprinted sensor-configuration
   functions (CMOS, ADTG, ENGIO lists, EngDrvOut), inert outside x5; see FULLRES.md.
5. Shared structure sizes and every accessed member offset are compared between
   the generic module and the EOS M core by the ARM ABI test. Do not enable another
   camera merely by changing the initialization guard: validate its DMA, cache,
   controls, timing and layout first.

## Capture and ownership

Three DMA input slots progress through capture, ready, processing and free states.
The next frame callback checks DMA idle state, pointer bounds, end sentinel and
outer guards before publishing an input. A separate accumulator stores the exact
per-Bayer-sample maximum. A separate preview snapshot avoids LCD/merge overlap.
Queue exhaustion stops acquisition rather than silently dropping inputs.

Stop drains the last admitted input. Teardown disables new callback admission,
waits for capture/display readers, releases input buffers, saves and validates
the DNG, restores controls and retires the session. Safe completion returns to
Armed. Uncertain DMA/display ownership retains memory and requires a reboot.
Existing DNG, LOG or CSV names are all reserved when selecting the next filename.

The standard EOS M core consumes idle physical Q/SET down before module dispatch,
but forwards physical SET up. LCM10 handles that release through the raw callback
before portable callbacks. A seen press (menu or synthetic long press) suppresses
its physical release; synthetic releases do not clear that suppression. Existing
capture gates prevent menu/playback/Off/half-shutter starts. This corrects the
earlier claim that short SET was impossible with a standard module. Only the
shutter UI lock is held; other locks suppress synthesized Q/SET events.

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

Checks cover independent packed-RAW14 maximum samples, geometry and DMA ownership,
timing changes observed on camera, Stop/drain, preview throttling, DNG decoding and
write failures, trigger gating, 1,000 session retirements and filename allocation,
unsupported camera rejection, the Q adapter and shared ARM structure layouts.
These do not emulate camera DMA scheduling or establish hardware reliability.

## Evidence and next camera check

LCM5 passed a 53-input camera session at 3 fps with 15 preview updates and a complete
DNG. LCM6 was reported to require a long SET press. LCM7 added repeat-session
cleanup. LCM8 changes integration/portability, retaining that capture pipeline.
LCM8 subsequently captured 76/76 inputs on camera with seven LCD updates and a
complete DNG. The full-resolution work (LCM9-LCM41) is documented in FULLRES.md:
the full-res composite ran on camera at 0.25, 0.4 and 0.5 fps; Canon's hardware
max engine was investigated and found unusable while Live View runs, so the
merge is done on the CPU and the engine code was removed in LCM41.

The frame limit remains 30/60/120. The Live composite DNG uses fixed daylight
neutral values; the full-res DNG follows Canon's WB setting.

Local builds, camera images/logs, research copies and obsolete factory experiments
are ignored. Keep only maintained implementation, executable checks and these
usage/development instructions in version control.

## Provenance and contribution status

This implementation and its checks contain substantial code generated and guided
by OpenAI Codex (Live composite, LCM1-LCM14) and Anthropic Claude (full-res
readout and composite, LCM15-LCM41). They developed from the user's earlier Live Composite experiments
and the ML RAW/silent/mlv_lite APIs. The factory capture/JPEG implementation is not linked or distributed. LCM9
adapts its native maximum wrapper for the explicitly selected feasibility check;
this does not require or start a factory capture. Source is marked GPL-2.0-or-later.

The repository's `LLM_POLICY.md` excludes substantial directly generated code from
upstream contributions. This local commit is not a claim of upstream eligibility
or maintainer acceptance; disclose the generation history in any discussion.
