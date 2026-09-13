#!/usr/bin/env python3
"""Recover 'Reason 14.pkg' from Reason_1410_d100-Stable-886-Mac.zip.

The Reason Studios CDN zip has a broken central directory (32-bit overflow,
not zip64) — unzip/7z/bsdtar refuse it. The only entry starts with a Local
File Header at offset 0, deflate method, sizes in a data descriptor (flag bit 3).

This script stream-decompresses the raw deflate stream from offset 59 (30 LFH +
13 name + 16 extra) and verifies the CRC from the data descriptor at the end.
"""
import os
import struct
import sys
import zlib
from pathlib import Path

ZIP = Path(sys.argv[1] if len(sys.argv) > 1 else
           os.path.expanduser("~/Projects/om-music/installers/Reason_1410_d100-Stable-886-Mac.zip"))
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else
           os.path.expanduser("~/Projects/om-music/installers/Reason1410-Mac/Reason 14.pkg"))
DATA_START = 59  # 30 (LFH) + 13 (name) + 16 (extra) — verified with xxd

with ZIP.open("rb") as f:
    f.seek(0)
    sig = f.read(4)
    assert sig == b"PK\x03\x04", f"bad LFH: {sig!r}"

    # Data descriptor (PK\x07\x08) — search from the end of the file (4 KB window)
    f.seek(0, 2)
    file_size = f.tell()
    f.seek(max(0, file_size - 4096))
    tail = f.read()
    dd = tail.rfind(b"PK\x07\x08")
    assert dd != -1, "no data descriptor at the end"
    desc = tail[dd + 4:]
    crc_expected = struct.unpack("<I", desc[:4])[0]
    # zip32: csize, usize as 4 bytes each; zip64: 8 bytes (on 32-bit overflow)
    csize, usize = struct.unpack("<II", desc[4:12])
    if csize in (0, 0xFFFFFFFF) or usize in (0, 0xFFFFFFFF):
        csize, usize = struct.unpack("<QQ", desc[4:20])
    print(f"descriptor: crc={crc_expected:08x} csize={csize} usize={usize}")

    f.seek(DATA_START)
    d = zlib.decompressobj(-15)  # raw deflate
    written = 0
    crc = 0
    with OUT.open("wb") as out:
        while True:
            chunk = f.read(16 * 1024 * 1024)
            if not chunk:
                break
            try:
                data = d.decompress(chunk)
            except zlib.error as e:
                print(f"end of stream: {e}")
                data = b""
            if data:
                out.write(data)
                crc = zlib.crc32(data, crc)
                written += len(data)
                print(f"\r{written / 1024**3:.2f} GiB", end="", flush=True)
    print()

print(f"saved {OUT}: {written} bytes (expected {usize})")
print(f"output CRC: {crc & 0xffffffff:08x} (expected {crc_expected:08x})",
      "— OK" if crc & 0xffffffff == crc_expected else "— MISMATCH!")
