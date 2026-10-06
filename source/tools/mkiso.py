#!/usr/bin/env python3
"""Build the Picsonut OS ISO without GRUB/xorriso.

  * ISO 9660 (level 1 names) with an El Torito catalog:
      - BIOS : no-emulation boot image  (boot/bios.asm, 4 virtual sectors)
      - UEFI : FAT12 ESP image containing /EFI/BOOT/BOOTX64.EFI
  * Hybrid MBR in sector 0 so the same file also boots from a USB stick (dd) on BIOS and UEFI.

usage: mkiso.py OUT.iso --bios bios.img --mbr mbr.bin --efi BOOTX64.EFI --kernel kernel.bin [--readme FILE]
"""
import argparse, os, struct, time

SECT = 2048


# ----------------------------------------------------------------------------- FAT12 ESP
def dos_dt(t):
    tm = time.gmtime(t)
    return ((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec // 2),
            ((tm.tm_year - 1980) << 9) | (tm.tm_mon << 5) | tm.tm_mday)


def make_fat12(files, stamp):
    """files: {'EFI/BOOT/BOOTX64.EFI': bytes}.  Returns a 1.44 MB FAT12 image."""
    BPS, TOTAL, FATSZ, ROOTENT = 512, 2880, 9, 224
    rootsecs = ROOTENT * 32 // BPS
    first_root = 1 + 2 * FATSZ
    first_data = first_root + rootsecs
    img = bytearray(TOTAL * BPS)
    img[0:3] = b"\xEB\x3C\x90"
    img[3:11] = b"PICSONUT"
    struct.pack_into("<HBHBHHBHHHII", img, 11, BPS, 1, 1, 2, ROOTENT, TOTAL, 0xF0, FATSZ, 18, 2, 0, 0)
    img[36] = 0x00
    img[38] = 0x29
    struct.pack_into("<I", img, 39, 0x50494353)
    img[43:54] = b"PICSONUT   "
    img[54:62] = b"FAT12   "
    img[510:512] = b"\x55\xAA"

    fat = [0] * 3072
    fat[0], fat[1] = 0xFF0, 0xFFF
    nxt = [2]
    tm, dt = dos_dt(stamp)

    def alloc(data):
        n = max(1, (len(data) + BPS - 1) // BPS)
        first = nxt[0]
        for i in range(n):
            c = first + i
            fat[c] = (c + 1) if i < n - 1 else 0xFFF
            off = (first_data + c - 2) * BPS
            chunk = data[i * BPS:(i + 1) * BPS]
            img[off:off + len(chunk)] = chunk
        nxt[0] += n
        return first

    def entry(name, attr, cluster, size):
        base, _, ext = name.partition(".")
        raw = base.upper().ljust(8)[:8].encode() + ext.upper().ljust(3)[:3].encode()
        return raw + struct.pack("<BBBHHHHHHHI", attr, 0, 0, tm, dt, dt, 0, tm, dt, cluster, size)

    # tree
    tree = {}
    for path, data in files.items():
        parts = path.split("/")
        node = tree
        for p in parts[:-1]:
            node = node.setdefault(p, {})
        node[parts[-1]] = data

    def build_dir(node, parent_cluster, self_cluster=None):
        """returns directory bytes; children are allocated first so their clusters are known"""
        ents = b""
        if self_cluster is not None:
            ents += entry(".", 0x10, self_cluster, 0) + entry("..", 0x10, parent_cluster, 0)
        for name, val in node.items():
            if isinstance(val, dict):
                # reserve the directory's own cluster first (one cluster is enough for our tiny tree)
                c = nxt[0]
                nxt[0] += 1
                body = build_dir(val, self_cluster if self_cluster is not None else 0, c)
                assert len(body) <= BPS
                fat[c] = 0xFFF
                off = (first_data + c - 2) * BPS
                img[off:off + len(body)] = body
                ents += entry(name, 0x10, c, 0)
            else:
                c = alloc(val)
                ents += entry(name, 0x20, c, len(val))
        return ents

    root = build_dir(tree, 0)
    assert len(root) <= rootsecs * BPS
    img[first_root * BPS:first_root * BPS + len(root)] = root

    fb = bytearray(FATSZ * BPS)
    for i in range(0, 3072, 2):
        a, b = fat[i], fat[i + 1]
        o = i // 2 * 3
        fb[o] = a & 0xFF
        fb[o + 1] = ((a >> 8) & 0x0F) | ((b & 0x0F) << 4)
        fb[o + 2] = (b >> 4) & 0xFF
    img[BPS:BPS + FATSZ * BPS] = fb
    img[BPS + FATSZ * BPS:BPS + 2 * FATSZ * BPS] = fb
    return bytes(img)


# ----------------------------------------------------------------------------- ISO 9660
def both32(v): return struct.pack("<I", v) + struct.pack(">I", v)
def both16(v): return struct.pack("<H", v) + struct.pack(">H", v)
def sectors(n): return (n + SECT - 1) // SECT
def pad(b, n=SECT): return b + b"\0" * (-len(b) % n)
def sp(s, n): return s.encode().ljust(n)[:n]


def date7(t):
    tm = time.gmtime(t)
    return bytes([tm.tm_year - 1900, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, 0])


def date17(t):
    tm = time.gmtime(t)
    return time.strftime("%Y%m%d%H%M%S", tm).encode() + b"00" + b"\0"


def dirrec(lba, size, is_dir, ident, t):
    n = len(ident)
    ln = 33 + n + (0 if (33 + n) % 2 == 0 else 1)
    rec = struct.pack("<BB", ln, 0) + both32(lba) + both32(size) + date7(t)
    rec += struct.pack("<BBB", 2 if is_dir else 0, 0, 0) + both16(1) + struct.pack("<B", n) + ident
    return rec + b"\0" * (ln - len(rec))


def el_torito_catalog(bios_lba, bios_count, efi_lba, efi_count):
    val = bytearray(32)
    val[0], val[1] = 1, 0
    val[4:28] = b"PICSONUT OS".ljust(24, b"\0")
    val[30], val[31] = 0x55, 0xAA
    s = sum(struct.unpack("<16H", bytes(val))) & 0xFFFF
    struct.pack_into("<H", val, 28, (-s) & 0xFFFF)
    init = struct.pack("<BBHBBHI", 0x88, 0, 0, 0, 0, bios_count, bios_lba).ljust(32, b"\0")
    hdr = struct.pack("<BBH", 0x91, 0xEF, 1).ljust(32, b"\0")
    efi = struct.pack("<BBHBBHI", 0x88, 0, 0, 0, 0, efi_count, efi_lba).ljust(32, b"\0")
    return bytes(val) + init + hdr + efi


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--bios", required=True)
    ap.add_argument("--mbr", required=True)
    ap.add_argument("--efi", required=True)
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--readme")
    ap.add_argument("--volid", default="PICSONUT_OS")
    a = ap.parse_args()
    stamp = int(os.environ.get("SOURCE_DATE_EPOCH", time.time()))

    bios = bytearray(open(a.bios, "rb").read())
    assert len(bios) == 2048
    mbr = bytearray(open(a.mbr, "rb").read())
    assert len(mbr) == 512 and mbr[510:512] == b"\x55\xAA"
    efi_exe = open(a.efi, "rb").read()
    kernel = open(a.kernel, "rb").read()
    readme = open(a.readme, "rb").read() if a.readme else b"Picsonut OS\r\n"
    esp = make_fat12({"EFI/BOOT/BOOTX64.EFI": efi_exe}, stamp)

    # ---- layout (in 2048-byte sectors)
    L_PVD, L_BOOTREC, L_TERM, L_PT_L, L_PT_M, L_CAT, L_ROOT, L_BOOTDIR = 16, 17, 18, 19, 20, 21, 22, 23
    nxt = 24
    files = {}
    for key, data in (("BIOS.IMG", bytes(bios)), ("EFIBOOT.IMG", esp), ("KERNEL.BIN", kernel)):
        files[key] = (nxt, data)
        nxt += sectors(len(data))
    L_README = nxt
    nxt += sectors(len(readme))
    total = nxt

    # patch the BIOS loader with the kernel location, then store it
    struct.pack_into("<I", bios, 8, files["KERNEL.BIN"][0])
    files["BIOS.IMG"] = (files["BIOS.IMG"][0], bytes(bios))

    # ---- directories
    def dot(lba, parent_lba):
        return dirrec(lba, SECT, True, b"\x00", stamp) + dirrec(parent_lba, SECT, True, b"\x01", stamp)

    boot_ents = b""
    for name in sorted(files):
        lba, data = files[name]
        boot_ents += dirrec(lba, len(data), False, (name + ";1").encode(), stamp)
    bootdir = pad(dot(L_BOOTDIR, L_ROOT) + boot_ents)
    root = pad(dot(L_ROOT, L_ROOT)
               + dirrec(L_BOOTDIR, SECT, True, b"BOOT", stamp)
               + dirrec(L_README, len(readme), False, b"README.TXT;1", stamp))
    assert len(bootdir) == SECT and len(root) == SECT

    # ---- path tables
    def pt(endian):
        def ent(ident, lba, parent):
            e = struct.pack("<BB", len(ident), 0) + struct.pack(endian + "IH", lba, parent) + ident
            return e + (b"\0" if len(ident) % 2 else b"")
        return ent(b"\x00", L_ROOT, 1) + ent(b"BOOT", L_BOOTDIR, 1)
    ptl, ptm = pt("<"), pt(">")
    ptsize = len(ptl)

    # ---- volume descriptors
    pvd = bytearray(SECT)
    pvd[0:8] = b"\x01CD001\x01\x00"
    pvd[8:40] = sp("PICSONUT", 32)
    pvd[40:72] = sp(a.volid, 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECT)
    pvd[132:140] = both32(ptsize)
    struct.pack_into("<I", pvd, 140, L_PT_L)
    struct.pack_into(">I", pvd, 148, L_PT_M)
    pvd[156:190] = dirrec(L_ROOT, SECT, True, b"\x00", stamp)
    for off in (190, 318, 446, 574):
        pvd[off:off + 128] = b" " * 128
    pvd[318:318 + 20] = sp("PICSONUT OS", 20)
    pvd[574:574 + 40] = sp("PICSONUT OS ISO BUILDER", 40)
    for off in (702, 739, 776):
        pvd[off:off + 37] = b" " * 37
    pvd[813:830] = date17(stamp)
    pvd[830:847] = date17(stamp)
    pvd[847:864] = b"0" * 16 + b"\0"
    pvd[864:881] = date17(stamp)
    pvd[881] = 1

    bootrec = bytearray(SECT)
    bootrec[0:7] = b"\x00CD001\x01"
    bootrec[7:39] = b"EL TORITO SPECIFICATION".ljust(32, b"\0")
    struct.pack_into("<I", bootrec, 71, L_CAT)
    term = bytearray(SECT)
    term[0:7] = b"\xFFCD001\x01"

    catalog = pad(el_torito_catalog(files["BIOS.IMG"][0], 4, files["EFIBOOT.IMG"][0], min(len(esp) // 512, 0xFFFF)))

    # ---- hybrid MBR: BIOS loader via boot code, UEFI via an 0xEF partition covering the ESP image
    efi_lba512 = files["EFIBOOT.IMG"][0] * 4
    struct.pack_into("<Q", mbr, 0x1B0, files["BIOS.IMG"][0] * 4)
    struct.pack_into("<I", mbr, 0x1B8, 0x50494353)
    part = struct.pack("<B3sB3sII", 0x80, b"\xFE\xFF\xFF", 0xEF, b"\xFE\xFF\xFF", efi_lba512, len(esp) // 512)
    mbr[0x1BE:0x1BE + 16] = part
    mbr[0x1CE:0x1FE] = b"\0" * 48

    # ---- assemble
    iso = bytearray(total * SECT)
    iso[0:512] = mbr

    def put(lba, data):
        iso[lba * SECT:lba * SECT + len(data)] = data
    put(L_PVD, pvd); put(L_BOOTREC, bootrec); put(L_TERM, term)
    put(L_PT_L, pad(ptl)); put(L_PT_M, pad(ptm)); put(L_CAT, catalog)
    put(L_ROOT, root); put(L_BOOTDIR, bootdir)
    for lba, data in files.values():
        put(lba, data)
    put(L_README, readme)
    open(a.out, "wb").write(iso)
    print("%s: %d bytes (%d sectors); BIOS image @%d, kernel @%d, ESP @%d"
          % (a.out, len(iso), total, files["BIOS.IMG"][0], files["KERNEL.BIN"][0], files["EFIBOOT.IMG"][0]))


if __name__ == "__main__":
    main()
