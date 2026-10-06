#!/usr/bin/env python3
"""Wrap the flat, position-independent uefi.elf into a PE32+ EFI application.
usage: mkpe.py uefi.elf BOOTX64.EFI"""
import struct, subprocess, sys
elf, out = sys.argv[1], sys.argv[2]
d = open(elf, "rb").read()
entry = struct.unpack_from("<Q", d, 0x18)[0]
shoff = struct.unpack_from("<Q", d, 0x28)[0]
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
secs = []
for i in range(shnum):
    nm, ty, fl, addr, off, size = struct.unpack_from("<IIQQQQ", d, shoff + i * shentsize)
    secs.append((nm, ty, fl, addr, off, size))
stroff = secs[shstrndx][4]
def name(o): return d[stroff + o: d.index(b"\0", stroff + o)].decode()
want = {".text": (".text", 0x60000020), ".rodata": (".rdata", 0x40000040), ".data": (".data", 0xC0000040)}
parts = []
for nm, ty, fl, addr, off, size in secs:
    n = name(nm)
    if n in want:
        assert ty == 1, n + " must be PROGBITS"
        parts.append((want[n][0], addr, d[off:off + size], want[n][1]))
assert [p[0] for p in parts] == [".text", ".rdata", ".data"], "unexpected sections"
# dummy base-relocation block (one padding entry) so that strict loaders see a .reloc directory
parts.append((".reloc", parts[-1][1] + 0x1000, struct.pack("<IIHH", 0, 12, 0, 0), 0x42000040))

ALIGN, FALIGN = 0x1000, 0x200
def up(v, a): return (v + a - 1) // a * a
nsec = len(parts)
hdr_size = up(0x40 + 4 + 20 + 240 + 40 * nsec, FALIGN)
raw_off = hdr_size
sects = b""; body = b""
code = idata = 0
for nm, va, data, ch in parts:
    raw = up(len(data), FALIGN)
    sects += struct.pack("<8sIIIIIIHHI", nm.encode(), len(data), va, raw, raw_off, 0, 0, 0, 0, ch)
    body += data + b"\0" * (raw - len(data))
    if ch & 0x20: code += raw
    else: idata += raw
    raw_off += raw
size_of_image = up(parts[-1][1] + len(parts[-1][2]), ALIGN)
reloc_va, reloc_sz = parts[-1][1], 12
opt = struct.pack("<HBBIIIII", 0x20B, 2, 30, code, idata, 0, entry, parts[0][1])
opt += struct.pack("<QIIHHHHHHIIIIHH", 0x140000000, ALIGN, FALIGN, 0, 0, 0, 0, 0, 0, 0,
                   size_of_image, hdr_size, 0, 10, 0)          # subsystem 10 = EFI application
opt += struct.pack("<QQQQII", 0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
dirs = [(0, 0)] * 16
dirs[5] = (reloc_va, reloc_sz)
opt += b"".join(struct.pack("<II", a, b) for a, b in dirs)
assert len(opt) == 240, len(opt)
coff = struct.pack("<HHIIIHH", 0x8664, nsec, 0, 0, 0, len(opt), 0x0022)
dos = b"MZ" + b"\0" * 0x3A + struct.pack("<I", 0x40)
pe = dos + b"PE\0\0" + coff + opt + sects
pe += b"\0" * (hdr_size - len(pe))
open(out, "wb").write(pe + body)
print("%s: %d bytes, entry RVA %#x, %d sections" % (out, len(pe + body), entry, nsec))
