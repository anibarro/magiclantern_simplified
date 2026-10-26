#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
B=platform/EOSM.202/build_livecomp
mkdir -p "$B"
python3 build_tools/test_livecomp_lv.py
python3 build_tools/test_livecomp_settings.py
python3 build_tools/test_livecomp_dng.py
python3 build_tools/test_livecomp_trigger.py
python3 build_tools/test_livecomp_sessions.py
python3 build_tools/test_livecomp_portability.py
python3 build_tools/test_livecomp_frlv.py
python3 build_tools/test_livecomp_fullres_fix.py
make -C modules/livecomp -j1 CONFIG_QEMU=n CONFIG_DEBUGMSG=0 \
  build/module_complete build/effective-config.txt 2>&1 | tee "$B/module-build.log"
# Standard core only for import checking; never packaged or installed.
make -C platform/EOSM.202 -j2 CONFIG_QEMU=n CONFIG_DEBUGMSG=0 \
  build/EOSM_202.sym > "$B/stock-core-check.log" 2>&1
python3 build_tools/package_livecomp_standalone.py
echo "Module-only package: $(ls -t $B/livecomp-*-module-only.zip | head -1)"
