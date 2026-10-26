#!/usr/bin/env python3
"""Fail a build if autoexec.bin does not embed its selected Magic Lantern image."""

import pathlib
import sys


autoexec_path = pathlib.Path(sys.argv[1])
firmware_path = pathlib.Path(sys.argv[2])
autoexec = autoexec_path.read_bytes()
firmware = firmware_path.read_bytes()

# reboot.c includes the selected firmware as an unchanged binary payload.
offset = autoexec.find(firmware)
if offset < 0:
    raise SystemExit(
        f"ERROR: {autoexec_path} does not embed {firmware_path}; refusing this package"
    )

print(f"Verified {firmware_path} embedded in {autoexec_path} at offset 0x{offset:x}")
