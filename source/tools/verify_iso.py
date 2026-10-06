#!/usr/bin/env python3
"""Independent structural check of picsonut-os.iso (does not share code with mkiso.py)."""
import struct, sys
iso = open(sys.argv[1], "rb").read()
S = 2048
def sec(n, c=1): return iso[n * S:(n + c) * S]
ok = True
def check(c, msg):
    global ok
    print(("PASS " if c else "FAIL ") + msg)
    ok &= bool(c)

# --- PVD
pvd = sec(16)
check(pvd[0] == 1 and pvd[1:6] == b"CD001" and pvd[6] == 1, "PVD signature")
total = struct.unpack_from("<I", pvd, 80)[0]
check(struct.unpack_from(">I", pvd, 84)[0] == total and total * S == len(iso), "volume size matches file (%d sectors)" % total)
check(struct.unpack_from("<H", pvd, 128)[0] == 2048, "logical block size 2048")
check(sec(18)[0] == 255 and sec(18)[1:6] == b"CD001", "volume descriptor terminator")
lpt, mpt = struct.unpack_from("<I", pvd, 140)[0], struct.unpack_from(">I", pvd, 148)[0]
ptsz = struct.unpack_from("<I", pvd, 132)[0]
check(sec(lpt)[:ptsz] != sec(mpt)[:ptsz] and len(sec(lpt)) == S, "path tables present")

def parse_dir(lba, size):
    out = []; data = iso[lba * S: lba * S + size]; p = 0
    while p < len(data):
        n = data[p]
        if n == 0:
            p = (p // S + 1) * S; continue
        a = struct.unpack_from("<I", data, p + 2)[0]; sz = struct.unpack_from("<I", data, p + 10)[0]
        assert struct.unpack_from(">I", data, p + 6)[0] == a and struct.unpack_from(">I", data, p + 14)[0] == sz
        fl = data[p + 25]; ln = data[p + 32]; name = data[p + 33:p + 33 + ln]
        assert n % 2 == 0 and n >= 33 + ln
        out.append((name, a, sz, fl)); p += n
    return out

root = struct.unpack_from("<I", pvd, 156 + 2)[0]
rsz = struct.unpack_from("<I", pvd, 156 + 10)[0]
tree = {}
def walk(lba, size, path):
    for name, a, sz, fl in parse_dir(lba, size):
        if name in (b"\x00", b"\x01"): continue
        n = name.decode()
        if fl & 2: walk(a, sz, path + n + "/")
        else: tree[path + n.split(";")[0]] = (a, sz)
walk(root, rsz, "/")
for k, v in sorted(tree.items()): print("   %-18s lba %-5d size %d" % (k, v[0], v[1]))
def f(path): a, sz = tree[path]; return iso[a * S: a * S + sz]

# --- El Torito
br = sec(17)
check(br[0] == 0 and br[1:6] == b"CD001" and br[7:30] == b"EL TORITO SPECIFICATION", "boot record descriptor")
cat_lba = struct.unpack_from("<I", br, 71)[0]
cat = sec(cat_lba)
check(sum(struct.unpack_from("<16H", cat, 0)) & 0xFFFF == 0 and cat[30:32] == b"\x55\xAA" and cat[0] == 1, "validation entry checksum")
check(cat[32] == 0x88 and cat[33] == 0, "BIOS entry: bootable, no emulation")
cnt, bios_lba = struct.unpack_from("<HI", cat, 32 + 6)
check(cnt == 4, "BIOS load size = 4 virtual sectors (2048 bytes)")
check(cat[64] == 0x91 and cat[65] == 0xEF, "UEFI section header (platform 0xEF)")
check(cat[96] == 0x88 and cat[97] == 0, "UEFI entry: bootable, no emulation")
efi_cnt, efi_lba = struct.unpack_from("<HI", cat, 96 + 6)

bios = sec(bios_lba)
check(bios == f("/BOOT/BIOS.IMG"), "El Torito BIOS image == /BOOT/BIOS.IMG")
check(bios[0] == 0xEB and bios[8 + 12 + 0:8 + 12 + 1] is not None, "BIOS image starts with jmp")
klba = struct.unpack_from("<I", bios, 8)[0]
kfile = f("/BOOT/KERNEL.BIN")
kb = struct.unpack_from("<I", bios, 12)[0]
check(klba == tree["/BOOT/KERNEL.BIN"][0], "kernel LBA patched into BIOS image (%d)" % klba)
check(kb == len(kfile), "kernel size in BIOS image matches (%d)" % kb)
check(kfile[:8] == b"\xd6\x50\x52\xe8\x00\x00\x00\x00", "kernel.bin begins with Multiboot2 header (flat image of the ELF)")

# --- ESP / FAT12
esp = f("/BOOT/EFIBOOT.IMG")
check(iso[efi_lba * S: efi_lba * S + len(esp)] == esp, "El Torito UEFI image == /BOOT/EFIBOOT.IMG")
check(len(esp) == 2880 * 512 and esp[510:512] == b"\x55\xAA" and esp[54:62] == b"FAT12   ", "ESP is a 1.44 MB FAT12 image")
bps, spc, rsv, nfat, rootent, tot16, media, fatsz = struct.unpack_from("<HBHBHHBH", esp, 11)
fat = esp[rsv * bps:(rsv + fatsz) * bps]
def fat_next(c):
    o = c * 3 // 2; v = fat[o] | (fat[o + 1] << 8)
    return (v >> 4) if c & 1 else (v & 0xFFF)
rootstart = rsv + nfat * fatsz; datastart = rootstart + rootent * 32 // bps
def read_chain(c, size=None):
    d = b""
    while 2 <= c < 0xFF8:
        d += esp[(datastart + c - 2) * bps:(datastart + c - 1) * bps]; c = fat_next(c)
    return d if size is None else d[:size]
def fat_dir(data):
    r = {}
    for i in range(0, len(data), 32):
        e = data[i:i + 32]
        if e[0] == 0: break
        if e[0] == 0xE5: continue
        name = e[:8].decode().strip(); ext = e[8:11].decode().strip()
        r[(name + "." + ext) if ext else name] = (e[11], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0])
    return r
rootd = fat_dir(esp[rootstart * bps:datastart * bps])
efid = fat_dir(read_chain(rootd["EFI"][1])); bootd = fat_dir(read_chain(efid["BOOT"][1]))
attr, cl, sz = bootd["BOOTX64.EFI"]
efi_bin = read_chain(cl, sz)
check(efi_bin[:2] == b"MZ" and len(efi_bin) == sz, "ESP: \\EFI\\BOOT\\BOOTX64.EFI readable via FAT chain (%d bytes)" % sz)
pe = struct.unpack_from("<I", efi_bin, 0x3C)[0]
check(efi_bin[pe:pe + 4] == b"PE\0\0" and struct.unpack_from("<H", efi_bin, pe + 4)[0] == 0x8664, "BOOTX64.EFI is PE32+ x86-64")
check(struct.unpack_from("<H", efi_bin, pe + 24 + 68)[0] == 10, "EFI subsystem = 10 (application)")

# --- hybrid MBR
mbr = iso[:512]
check(mbr[510:512] == b"\x55\xAA", "MBR signature")
ptype = mbr[0x1BE + 4]; pstart, plen = struct.unpack_from("<II", mbr, 0x1BE + 8)
check(ptype == 0xEF and pstart == efi_lba * 4 and plen == len(esp) // 512, "MBR partition 1 = ESP (type EF) at ISO ESP image")
check(struct.unpack_from("<Q", mbr, 0x1B0)[0] == bios_lba * 4, "MBR loads BIOS image at LBA %d (512-byte units)" % (bios_lba * 4))
print("\nALL CHECKS PASSED" if ok else "\nSOME CHECKS FAILED"); sys.exit(0 if ok else 1)
