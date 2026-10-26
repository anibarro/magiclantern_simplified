Live Composite
==============

Olympus-style Live Composite: continuous light painting with LCD preview and RAW DNG output.

:Author: LC1 prototype authors; module port assisted by OpenAI Codex; full-res composite assisted by Anthropic Claude
:License: GPL
:Summary: Live and full-sensor composite with DNG for EOS M 2.0.2

Enable Shoot / Live Composite, close the menu, press SET to start, SET to stop.
Full-res composite: 5208x3478 frames at 0.25-0.5 fps; Movie M, fixed ISO/WB,
x1 view, FPS override OFF, Global Draw OFF. Saves LVFRnnnn.DNG.
Live composite: 1736x697 frames at 3 fps; FPS override 3 fps, Low light,
slowest shutter, Global Draw ON. Saves LVCPnnnn.DNG.
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

LCM8 captured 76 Live View frames with LCD preview and DNG output on camera.
The full-res composite ran on camera at 0.25, 0.4 and 0.5 fps (LCM23-LCM40)
with exact merges, preview and DNG. Short SET is handled on release; long SET
remains available.
