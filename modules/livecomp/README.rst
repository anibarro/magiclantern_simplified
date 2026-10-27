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
Reboot only if Status says Reboot. See INSTALL.txt for details.

Standalone module
-----------------

Standard ML RAW, FPS, display and key callbacks are used; no other module or
modified core is required. Full-res mode temporarily hooks four fingerprinted
EOS M 2.0.2 sensor-configuration functions while it runs (see FULLRES.md).
Camera-specific assumptions are selected at runtime after a model/firmware
check. No platform headers or camera-specific compiler flags are used.
See DEVELOPMENT.md for conventions, validation and AI-generation disclosure.

Build with the normal ML toolchain: make -C modules/livecomp.
The optional repository build_tools/build_livecomp.sh runs host checks and
validates imports against an unmodified EOSM.202 core before packaging only
livecomp.mo. The validation core is never included in the install ZIP.

Validation
----------

Camera-tested on an EOS M 2.0.2: full-res composites with 2.5, 3, 4, 5 and
10 s shutters (LCM23-LCM52), exact merges, LCD preview, focus-pixel fix,
white balance and DNG; 6-60 s still need a camera run. Live Bulb: 3 s
frames (LCM55, 2.3 s per merge, LCD updated every frame); it needs a 3 s or
longer shutter. Host tests cover the
register rewriting, a simulated session, the merge kernel, the DNG writer,
the key gating and the ARM structure layouts.

Documentation
-------------

- INSTALL.txt: installation and use.
- HOW-IT-WORKS.md: the implementation.
- HARDWARE-MERGE.md: why Canon's image engine is not used.
- FULLRES.md: the chronological development log with camera results.
- DEVELOPMENT.md: conventions, build, checks, provenance.
