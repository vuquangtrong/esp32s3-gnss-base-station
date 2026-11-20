#!/usr/bin/env python3

import argparse
import csv
import struct
import zlib
from pathlib import Path

MAGIC = b"GNSSFWPK"
FORMAT_VERSION = 1
HEADER_SIZE = 4096
HEADER_FORMAT = "<8s8I"

ROOT_DIR = Path(__file__).resolve().parent


def parse_size(value):
    text = value.strip().upper()
    multipliers = {"K": 1024, "M": 1024 * 1024}
    if text and text[-1] in multipliers:
        return int(text[:-1]) * multipliers[text[-1]]
    return int(text, 0)


def read_partition_sizes(partitions_file):
    sizes = {}
    with open(partitions_file, newline="", encoding="utf-8") as handle:
        for row in csv.reader(line for line in handle if not line.lstrip().startswith("#")):
            if len(row) < 5:
                continue
            sizes[row[0].strip()] = parse_size(row[4])
    return sizes


def build_package(args):
    app_image = Path(args.app).read_bytes()
    www_image = Path(args.www).read_bytes()
    partition_sizes = read_partition_sizes(args.partitions)

    max_app_size = min(partition_sizes["factory"], partition_sizes["ota_0"])
    max_www_size = partition_sizes["www"]
    if len(app_image) > max_app_size:
        raise ValueError(f"App image {len(app_image)} bytes exceeds limit {max_app_size} bytes")
    if len(www_image) > max_www_size:
        raise ValueError(f"WWW image {len(www_image)} bytes exceeds limit {max_www_size} bytes")

    app_offset = HEADER_SIZE
    www_offset = app_offset + len(app_image)
    total_size = www_offset + len(www_image)

    header = struct.pack(
        HEADER_FORMAT,
        MAGIC,
        FORMAT_VERSION,
        total_size,
        app_offset,
        len(app_image),
        zlib.crc32(app_image),
        www_offset,
        len(www_image),
        zlib.crc32(www_image),
    )
    header = header.ljust(HEADER_SIZE, b"\x00")

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("wb") as handle:
        handle.write(header)
        handle.write(app_image)
        handle.write(www_image)

    print(f"Created {output_path} ({total_size} bytes)")
    print(f"  app: {len(app_image)} bytes, crc32=0x{zlib.crc32(app_image):08X}")
    print(f"  www: {len(www_image)} bytes, crc32=0x{zlib.crc32(www_image):08X}")


def main():
    parser = argparse.ArgumentParser(description="Build update.bin for SD-card firmware update")
    parser.add_argument("--app", default=str(ROOT_DIR / "build" / "gnss_station.bin"))
    parser.add_argument("--www", default=str(ROOT_DIR / "build" / "www.bin"))
    parser.add_argument("--partitions", default=str(ROOT_DIR / "partitions.csv"))
    parser.add_argument("--output", default=str(ROOT_DIR / "build" / "update.bin"))
    build_package(parser.parse_args())


if __name__ == "__main__":
    main()
