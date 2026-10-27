Live Composite
==============

Olympus-style Live Composite: continuous light painting with LCD preview and RAW DNG output.

:Author: LC1 prototype authors; module port assisted by OpenAI Codex; full-res composite assisted by Anthropic Claude
:License: GPL
:Summary: Full-sensor live composite with DNG for EOS M 2.0.2

Full-sensor (5208x3478) composite, 2-60 s shutter, RAW DNG output.
Movie M, fixed ISO/WB, x1 view, FPS override OFF, Global Draw OFF.
Set ISO and aperture on the camera, the shutter in this menu.
SET: exposure preview. SET: start. SET: stop and save LVFRnnnn.DNG.
Half-shutter leaves the preview. Low battery stops and saves.
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

The full-res composite ran on camera with 2, 2.5 and 4 s frames (LCM23-LCM42)
with exact merges, preview and DNG. 6-60 s shutters still need a camera run.
