Live Composite
==============

Continuous light painting with LCD preview and RAW DNG output.

:Author: LC1 prototype authors; module port assisted by OpenAI Codex
:License: GPL
:Summary: Continuous LCD composite and DNG for EOS M 2.0.2

Enable Shoot / Live Composite, close the menu, then hold Q/SET to start.
Hold Q/SET to stop and save. Enabling alone never starts capture. The submenu sets
30/60/120 input limits and 1/2/4-second LCD refresh. Every RAW input is merged.

Use Movie M, fixed ISO/WB, normal view, Global Draw, and ML FPS override at
3 fps with Low light and the slowest shutter. After saving, hold SET for another capture without rebooting.
A reboot is required only if capture/display memory ownership is uncertain.
See INSTALL.txt for complete instructions and current limits.

Standalone module
-----------------

Only livecomp.o and livecomp-lv.o are linked. Standard ML RAW, FPS, display
and key callbacks are used; no other module or modified core is required.
Camera-specific assumptions are selected at runtime after a model/firmware
check. No platform headers or camera-specific compiler flags are used.
See DEVELOPMENT.md for conventions, validation and AI-generation disclosure.

Build with the normal ML toolchain: make -C modules/livecomp.
The optional repository build_tools/build_livecomp.sh runs host checks and
validates imports against an unmodified EOSM.202 core before packaging only
livecomp.mo. The validation core is never included in the install ZIP.

Validation
----------

The continuous capture/LCD/DNG backend passed LCM5 on camera. LCM8's portable integration and repeated
sessions need camera confirmation. The stock core requires long SET presses. Full-sensor readout is not included.
