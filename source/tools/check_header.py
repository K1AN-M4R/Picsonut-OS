#!/usr/bin/env python3
"""Sanity-check that the kernel ELF is a valid x86_64 Multiboot2 image."""
import struct, sys
d = open(sys.argv[1], "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2, "not an ELF64 file"
entry, phoff = struct.unpack_from("<QQ", d, 0x18)[0], struct.unpack_from("<Q", d, 0x20)[0]
phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
found = None
for off in range(0, min(len(d), 32768) - 16, 8):
    magic, arch, length, csum = struct.unpack_from("<4I", d, off)
    if magic == 0xE85250D6:
        assert (magic + arch + length + csum) & 0xFFFFFFFF == 0, "bad multiboot2 checksum"
        found = off
        break
assert found is not None, "multiboot2 header not found in first 32 KiB"
loads = []
for i in range(phnum):
    t, fl, o, va, pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
    if t == 1:
        loads.append((pa, fs, ms))
assert entry < 0x100000000, "entry point must be 32-bit addressable"
print("OK: multiboot2 header at file offset %#x, entry %#x, %d LOAD segment(s)" % (found, entry, len(loads)))
