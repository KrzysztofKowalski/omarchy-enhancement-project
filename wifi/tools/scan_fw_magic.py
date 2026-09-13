#!/usr/bin/env python3
"""Scan BCM4360 firmware binary for 32-bit magic constants (LE and BE).

Clean-room static analysis of /lib/firmware/brcm/brcmfmac4360-pcie.bin.
Reports every file offset where a candidate magic appears, plus ASCII
context (printable run around the hit) to help judge whether the hit is
a literal pool constant, an embedded string, or data.

Usage:
    python3 scan_fw_magic.py [path-to-firmware] [magic ...]
    (defaults: /lib/firmware/brcm/brcmfmac4360-pcie.bin,
     constants: 0x434d4853 0x48534d43 0x43444343 0x43434443 0xa5a5a5a5)
"""
import struct
import sys

DEFAULT_FW = "/lib/firmware/brcm/brcmfmac4360-pcie.bin"
DEFAULT_MAGICS = [
    0x434d4853,  # "SHMC" LE (brcmfmac PCIe shared-ram magic)
    0x48534d43,  # "SHMC" BE / "CMSH" LE
    0x43444343,  # "CCDC" LE ("CDC" dongle family)
    0x43434443,  # "CDCC" LE
    0xA5A5A5A5,  # test magic used by kimptoc T276 / this project's T276
]


def printable_run(data, center, before=16, after=16):
    """Return ASCII context around byte offset `center`."""
    start = max(0, center - before)
    end = min(len(data), center + after)
    run = bytearray()
    for b in data[start:end]:
        run.append(b if 0x20 <= b < 0x7F else ord("."))
    return run.decode("ascii")


def scan(data, magic, endian):
    fmt = "<I" if endian == "LE" else ">I"
    pat = struct.pack(fmt, magic)
    hits = []
    pos = 0
    while True:
        pos = data.find(pat, pos)
        if pos < 0:
            break
        hits.append(pos)
        pos += 1
    return hits


def main():
    fw_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_FW
    magics = [int(m, 0) for m in sys.argv[2:]] if len(sys.argv) > 2 else DEFAULT_MAGICS

    with open(fw_path, "rb") as f:
        data = f.read()

    print(f"# Magic scan: {fw_path} ({len(data)} bytes)")
    for magic in magics:
        le_hits = scan(data, magic, "LE")
        be_hits = scan(data, magic, "BE")
        name = bytes(struct.pack("<I", magic)).decode("latin1")
        print(f"\n## 0x{magic:08X}  ASCII<LE> '{name}'  ASCII<BE> "
              f"'{bytes(struct.pack('>I', magic)).decode('latin1')}'")
        if not le_hits and not be_hits:
            print("    (no occurrences)")
            continue
        for endian, hits in (("LE", le_hits), ("BE", be_hits)):
            if not hits:
                continue
            print(f"  {endian}: {len(hits)} hit(s)")
            for off in hits:
                ctx = printable_run(data, off)
                print(f"    file+0x{off:06X}  bytes={data[off:off+4].hex()}  "
                      f"ctx: ...{ctx}...")
    # Alignment / density hint: group hits per 64-byte window
    print("\n# Dense windows (>=2 magic hits within 64 bytes)")
    from collections import defaultdict
    windows = defaultdict(list)
    for magic in magics:
        for off in scan(data, magic, "LE"):
            windows[off // 64].append((off, magic))
        for off in scan(data, magic, "BE"):
            windows[off // 64].append((off, magic))
    for win, hits in sorted(windows.items()):
        if len(hits) >= 2:
            print(f"  window at 0x{win*64:06X}: " +
                  ", ".join(f"0x{off:06X}=0x{m:08X}" for off, m in hits))


if __name__ == "__main__":
    main()
