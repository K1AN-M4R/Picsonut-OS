import sys, struct
sys.path.insert(0, "tools")
import sim_boot as S
cases = (("1024x768 BGR, chosen out of six modes", sys.argv[1], dict(w=1024, h=768, pitch=1040 * 4, pos=(16, 8, 0))),
         ("single 800x600 RGB mode", sys.argv[2], dict(w=800, h=600, pitch=800 * 4, pos=(0, 8, 16))))
pages = 0x9F + 0x7000 + 0x30 + 0x20 + 5      # usable pages in the fake firmware map (types 1,2,3,4,7)
ok = True
for name, path, e in cases:
    mem = open(path, "rb").read()
    fb, kb = S.kernel_view(mem, 0)
    good = bool(fb) and (fb["w"], fb["h"], fb["pitch"], (fb["rpos"], fb["gpos"], fb["bpos"])) == (e["w"], e["h"], e["pitch"], e["pos"]) \
        and fb["addr"] == 0xFD000000 and kb == pages * 4 and struct.unpack_from("<II", mem, len(mem) - 8) == (0, 8)
    print("%s UEFI loader on fake firmware: %s" % ("PASS" if good else "FAIL", name)); ok &= good
sys.exit(0 if ok else 1)
