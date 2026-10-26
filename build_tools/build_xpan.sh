#!/bin/bash
# Build the XPan firmware for the Canon EOS M (2.0.2). Run from anywhere
# (Linux or WSL); the toolchain must be on PATH or given with ARM_BINPATH.
#   build_tools/build_xpan.sh            -> platform/EOSM.202/build_xpan/magiclantern.zip
#   B=my_dir build_tools/build_xpan.sh   -> custom build directory
set -euo pipefail
cd "$(dirname "$0")/.."
B=${B:-build_xpan}
LOG=platform/EOSM.202/$B.log
make -C platform/EOSM.202 -j"$(nproc)" BUILD_DIR=$B ZIP_DIR=$B/zip CONFIG_QEMU=n XPAN=y \
  ${ARM_BINPATH:+ARM_BINPATH=$ARM_BINPATH} 2>&1 | tee "$LOG"
D=platform/EOSM.202/$B
{
  echo "--- verification"
  sha256sum "$D/magiclantern.zip" "$D/autoexec.bin" "$D/magiclantern.bin"
  python3 build_tools/verify_embedded_firmware.py "$D/autoexec.bin" "$D/magiclantern.bin"
} 2>&1 | tee -a "$LOG"
echo "XPAN BUILD DONE"
