#!/usr/bin/env python3
"""Generate build/full.bin (for erased flash) and build/update.bin (for SD card update)."""

import hashlib
import os
import struct
import subprocess
import sys

MAGIC = b"GNSSFWUPD"
MAGIC_LEN = 16
VERSION = 1
HEADER_SIZE = 4096

ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(ROOT_DIR, "build")
APP_BIN = os.path.join(BUILD_DIR, "gnss_station.bin")
WWW_BIN = os.path.join(BUILD_DIR, "www_0.bin")
FULL_BIN = os.path.join(BUILD_DIR, "full.bin")
UPDATE_BIN = os.path.join(BUILD_DIR, "update.bin")


def build_firmware() -> None:
    subprocess.run(["idf.py", "build"], cwd=ROOT_DIR, check=True)


def make_full_bin() -> None:
    flash_args = os.path.join(BUILD_DIR, "flash_args")
    if not os.path.isfile(flash_args):
        sys.exit(f"Error: {flash_args} not found. Run 'idf.py build' first.")

    cmd = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32s3",
        "merge-bin",
        "-o",
        FULL_BIN,
        "@flash_args",
    ]
    subprocess.run(cmd, cwd=BUILD_DIR, check=True)
    print(f"Created {FULL_BIN} ({os.path.getsize(FULL_BIN)} bytes)")


def make_update_bin() -> None:
    for path in (APP_BIN, WWW_BIN):
        if not os.path.isfile(path):
            sys.exit(f"Error: {path} not found. Run 'idf.py build' first.")

    with open(APP_BIN, "rb") as f:
        app_data = f.read()
    with open(WWW_BIN, "rb") as f:
        www_data = f.read()

    app_sha256 = hashlib.sha256(app_data).digest()
    www_sha256 = hashlib.sha256(www_data).digest()

    header = struct.pack(
        f"<{MAGIC_LEN}sII32sI32s",
        MAGIC.ljust(MAGIC_LEN, b"\x00"),
        VERSION,
        len(app_data),
        app_sha256,
        len(www_data),
        www_sha256,
    )
    header = header.ljust(HEADER_SIZE, b"\x00")

    with open(UPDATE_BIN, "wb") as f:
        f.write(header)
        f.write(app_data)
        f.write(www_data)

    print(
        f"Created {UPDATE_BIN} ({os.path.getsize(UPDATE_BIN)} bytes: "
        f"header={HEADER_SIZE}, app={len(app_data)}, www={len(www_data)})"
    )


def main() -> None:
    build_firmware()
    make_full_bin()
    make_update_bin()


if __name__ == "__main__":
    main()
