#!/usr/bin/env python3
"""
macho-dylibs.py — list of dylib dependencies (LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB)
of Mach-O binaries (handles thin and universal/FAT, incl. x86_64+arm64).

Pure Python + struct, no dependencies (no llvm/otool). Usage:
    python3 macho-dylibs.py <file> [--arch x86_64|arm64]

By default prints the dependencies of the x86_64 slice ("macOS Intel 64" is
what we care about).
"""

import struct
import sys

# magic (stored little-endian in the file)
MH_MAGIC_64 = 0xFEEDFACF  # thin 64-bit
FAT_MAGIC = 0xCAFEBABE     # FAT, wpisy fat_arch (32-bitowe)
FAT_MAGIC_64 = 0xCAFEBABF  # FAT, wpisy fat_arch_64

CPU_TYPE_X86_64 = 0x01000007
CPU_TYPE_ARM64 = 0x0100000C
ARCH_NAMES = {CPU_TYPE_X86_64: "x86_64", CPU_TYPE_ARM64: "arm64"}

# load commands
LC_LOAD_DYLIB = 0xC
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001F
LC_ID_DYLIB = 0xD
DYLIB_CMDS = {
    LC_LOAD_DYLIB: "LC_LOAD_DYLIB",
    LC_LOAD_WEAK_DYLIB: "LC_LOAD_WEAK_DYLIB",
    LC_REEXPORT_DYLIB: "LC_REEXPORT_DYLIB",
    LC_ID_DYLIB: "LC_ID_DYLIB",
}

# struct mach_header_64: magic cputype cpusubtype filetype ncmds sizeofcmds flags reserved
MH64_FMT = "<8I"
MH64_SIZE = struct.calcsize(MH64_FMT)
# struct fat_header: magic nfat_arch
# struct fat_arch:     cputype cpusubtype offset size align
FAT_ARCH_FMT = ">5I"
FAT_ARCH_SIZE = struct.calcsize(FAT_ARCH_FMT)
# struct fat_arch_64:  cputype cpusubtype offset(q) size(q) align reserved
FAT_ARCH64_FMT = ">2I2Q2I"
FAT_ARCH64_SIZE = struct.calcsize(FAT_ARCH64_FMT)


def load_cstring(data, off):
    end = data.index(b"\x00", off)
    return data[off:end].decode("utf-8", errors="replace")


def walk_mach64(data, base_off, label, wanted_arch):
    """base_off = offset of the slice in the file (0 for thin); label for messages."""
    magic, = struct.unpack_from("<I", data, base_off)
    if magic != MH_MAGIC_64:
        print(f"  [!] no MH_MAGIC_64 at offset {base_off:#x} (magic={magic:#x})")
        return
    _, cputype, _, _, ncmds, _, _, _ = struct.unpack_from(MH64_FMT, data, base_off)
    arch = ARCH_NAMES.get(cputype, f"cputype={cputype:#x}")
    if arch != wanted_arch:
        return
    print(f"--- {label}: slice {arch} @ offset {base_off:#x} ---")
    off = base_off + MH64_SIZE
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd in DYLIB_CMDS:
            name_off, = struct.unpack_from("<I", data, off + 8)  # dylib_command.name (lc_str)
            try:
                dylib = load_cstring(data, off + name_off)
            except ValueError:
                dylib = "(invalid name offset)"
            print(f"  {DYLIB_CMDS[cmd]:<22} {dylib}")
        off += cmdsize


def main():
    argv = sys.argv[1:]
    args = [a for a in argv if not a.startswith("--")]
    if not args:
        print(__doc__)
        sys.exit(2)
    path = args[0]
    want = "x86_64"
    if "--arch" in argv:
        a = argv[argv.index("--arch") + 1]
        if a in ("arm64", "x86_64"):
            want = a

    data = open(path, "rb").read()
    raw = data[:4]
    if raw == b"\xfe\xed\xfa\xcf":  # MH_MAGIC_64 (LE w pliku)
        magic = MH_MAGIC_64
    elif raw == b"\xca\xfe\xba\xbe":  # FAT_MAGIC (BE w pliku)
        magic = FAT_MAGIC
    elif raw == b"\xca\xfe\xba\xbf":  # FAT_MAGIC_64 (BE w pliku)
        magic = FAT_MAGIC_64
    else:
        print(f"Unknown format: first bytes {raw.hex()} (not Mach-O 64-bit nor FAT)")
        sys.exit(1)

    if magic == MH_MAGIC_64:
        walk_mach64(data, 0, path, want)
    elif magic in (FAT_MAGIC, FAT_MAGIC_64):
        _, nfat = struct.unpack_from(">2I", data, 0)
        is64 = magic == FAT_MAGIC_64
        fmt = FAT_ARCH64_FMT if is64 else FAT_ARCH_FMT
        entry_size = FAT_ARCH64_SIZE if is64 else FAT_ARCH_SIZE
        for i in range(nfat):
            fields = struct.unpack_from(fmt, data, 8 + i * entry_size)
            cputype, _, off, sz = fields[:4]
            if struct.unpack_from("<I", data, off)[0] != MH_MAGIC_64:
                continue
            name = ARCH_NAMES.get(cputype, f"cputype={cputype:#x}")
            walk_mach64(data, off, f"{path}[{name}]", want)
    else:
        print(f"Unknown format: magic={magic:#x} (not Mach-O 64-bit nor FAT)")
        sys.exit(1)


if __name__ == "__main__":
    main()
