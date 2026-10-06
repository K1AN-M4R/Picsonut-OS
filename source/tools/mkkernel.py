#!/usr/bin/env python3
"""Flatten the kernel ELF into a raw image + generate constants for the loaders.
usage: mkkernel.py picsonut.elf out_dir"""
import struct, subprocess, sys, os
elf, out = sys.argv[1], sys.argv[2]
d = open(elf, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2
phoff = struct.unpack_from("<Q", d, 0x20)[0]
phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
loads = []
for i in range(phnum):
    t, fl, o, va, pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
    if t == 1: loads.append((o, pa, fs, ms))
assert len(loads) == 1, "expected exactly one PT_LOAD"
off, load, filesz, memsz = loads[0]
assert load == 0x100000
# The BIOS loader reads the image into a bounce buffer at 0x10000..0x9FC00 and the kernel must stay below 2 MiB
assert filesz <= 0x80000, "kernel image larger than the 512 KiB the BIOS loader can bounce-buffer"
assert load + memsz <= 0x200000, "kernel image + .bss must end below 2 MiB (the allocator starts there)"
syms = {}
for line in subprocess.check_output(["nm", elf], text=True).splitlines():
    p = line.split()
    if len(p) == 3: syms[p[2]] = int(p[0], 16)
e32, e64 = syms["_start"], syms["uefi_entry"]
os.makedirs(out, exist_ok=True)
open(os.path.join(out, "kernel.bin"), "wb").write(d[off:off + filesz])
open(os.path.join(out, "kernel_info.h"), "w").write(
    "#define KERNEL_LOAD   0x%xULL\n#define KERNEL_FILESZ 0x%xULL\n#define KERNEL_MEMSZ  0x%xULL\n#define KERNEL_ENTRY64 0x%xULL\n" % (load, filesz, memsz, e64))
open(os.path.join(out, "kernel_info.inc"), "w").write(
    "KERNEL_LOAD equ 0x%x\nKERNEL_FILESZ equ 0x%x\nKERNEL_MEMSZ equ 0x%x\nKERNEL_ENTRY32 equ 0x%x\n" % (load, filesz, memsz, e32))
print("kernel.bin: %d bytes (memsz %#x), entry32 %#x, entry64 %#x" % (filesz, memsz, e32, e64))
