Live Composite
==============

Olympus-style Live Composite and Live Bulb: continuous light painting, or a bulb exposure you watch building up, with LCD preview and RAW DNG output.

:Author: LC1 prototype authors; module port assisted by OpenAI Codex; full-res composite assisted by Anthropic Claude
:License: GPL
:Summary: Full-sensor live composite / live bulb with DNG for EOS M 2.0.2

Full-sensor (5208x3478) composite, 2.5-60 s shutter (Live Bulb: 3 s and up),
RAW DNG output.
Mode: Live Composite (brightest pixel wins) or Live Bulb (frames add up,
like one bulb exposure of frames x shutter; DCIM/LIVE/LVBLnnnn.DNG).
Movie M, fixed ISO/WB, x1 view, FPS override OFF (Global Draw: automatic).
Set ISO and aperture on the camera, the shutter in this menu.
Preview only ON: SET shows one frame with these settings, nothing saved.
SET starts the composite, SET stops and saves DCIM/LIVE/LVFRnnnn.DNG
(diagnostic LOG/CSV only with Debug ON).
Live Bulb removes column stripes automatically. Dark frame ON: cover the
lens at the end; dark frames (Bulb: as many as light frames, Composite: one)
are subtracted. Save unprocessed ON: also LVBR/LVCR (no in-camera noise
reduction) and LVDK/LVCK (dark frames).
Reboot only if Status says Reboot. See INSTALL.txt for details.

Standalone module
-----------------

Standard ML RAW, FPS, display and key callbacks are used; no other module or
modified core is required. Full-res mode temporarily hooks four fingerprinted
EOS M 2.0.2 sensor-configuration functions while it runs, and Canon's
AbortEDmac for a diagnostic trace only (see FULLRES.md).
Camera-specific assumptions are selected at runtime after a model/firmware
check. No platform headers or camera-specific compiler flags are used.
See DEVELOPMENT.md for conventions, validation and AI-generation disclosure.

Build with the normal ML toolchain: make -C modules/livecomp.
The optional repository build_tools/build_livecomp.sh runs host checks and
validates imports against an unmodified EOSM.202 core before packaging only
livecomp.mo. The validation core is never included in the install ZIP.

Validation
----------

Camera-tested on an EOS M 2.0.2: full-res composites with 2.5, 3, 4, 5, 6
and 10 s shutters (LCM23-LCM61), exact merges, LCD preview, focus-pixel fix,
white balance and DNG. Live Bulb: 3, 4, 6, 15 and 50 s frames (LCM55-LCM61,
2.3 s per added frame), column correction, dark-frame subtraction (in place
and with Save unprocessed) and single-frame bulbs; composite dark frame
(LCM61, 6 s). Not yet confirmed on the camera: the LCM62/63 start-up delay
against the recurring partial frame. Host tests cover the register rewriting, a
simulated session (incl. dark frames, the late timer latch, a simulated
Canon abort), the merge/sum/subtract kernels, the column correction, the
DNG writer, the key gating and the ARM structure layouts.

Documentation
-------------

- INSTALL.txt: installation and use.
- HOW-IT-WORKS.md: the implementation.
- HARDWARE-MERGE.md: why Canon's image engine is not used.
- FULLRES.md: the chronological development log with camera results.
- DEVELOPMENT.md: conventions, build, checks, provenance.
