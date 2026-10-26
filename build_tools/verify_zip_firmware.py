#!/usr/bin/env python3
"""Fail packaging if the ZIP's autoexec.bin does not embed its selected firmware."""

from pathlib import Path
import sys
import zipfile


archive_path = Path(sys.argv[1])
firmware_path = Path(sys.argv[2])
firmware = firmware_path.read_bytes()

try:
    with zipfile.ZipFile(archive_path) as archive:
        autoexec = archive.read("autoexec.bin")
except (KeyError, zipfile.BadZipFile) as error:
    raise SystemExit(f"ERROR: {archive_path} has no readable autoexec.bin: {error}")

# The ZIP must contain the same firmware payload that passed the pre-package check.
offset = autoexec.find(firmware)
if offset < 0:
    raise SystemExit(
        f"ERROR: {archive_path} does not embed {firmware_path}; refusing this package"
    )

print(f"Verified {firmware_path} embedded in {archive_path}:autoexec.bin at offset 0x{offset:x}")
