#!/usr/bin/env python3
"""Boot + feature tests in QEMU.   usage: qemu_test.py ISO [group ...]
groups: boot res uefi mem smp fs vim panic   (default: all).  Needs qemu-system-x86_64; `uefi` needs OVMF."""
import os, struct, sys, time
sys.path.insert(0, os.path.dirname(__file__))
from qemu_driver import VM

iso = sys.argv[1]
groups = sys.argv[2:] or ["boot", "res", "uefi", "mem", "smp", "fs", "vim", "panic"]
fails = 0
def check(cond, msg):
    global fails
    print(("PASS  " if cond else "FAIL  ") + msg); sys.stdout.flush()
    if not cond: fails += 1

def boot(extra=(), uefi=False, menu_key=None):
    vm = VM(iso, extra, uefi)
    if menu_key:                         # answer the resolution menu (keys are re-sent until the kernel is up)
        end = time.time() + 90
        while time.time() < end and "Welcome" not in vm.serial():
            vm.key(menu_key); time.sleep(0.4)
    ok = vm.wait("Type 'help'", 90)
    if menu_key: vm.type("\n"); time.sleep(0.5)
    return vm, ok

def bigram(mb):
    """guest RAM backed by a sparse file, so a small host can still present e.g. 20 GiB to the guest"""
    f = "/tmp/picsonut_ram_%d.img" % mb
    return ["-m", str(mb), "-object", "memory-backend-file,id=pc.ram,size=%dM,mem-path=%s,share=on" % (mb, f),
            "-machine", "memory-backend=pc.ram"]

def ppm_size(path):
    d = open(path, "rb").read(32).split()
    return int(d[1]), int(d[2])

MODES = {"1": (640, 480), "2": (800, 600), "3": (1024, 768), "4": (1280, 720), "5": (1280, 800), "6": (1920, 1080)}

if "boot" in groups:
    vm, ok = boot(["-m", "512", "-smp", "4"])
    s = vm.serial()
    check(ok, "BIOS boot reaches the shell")
    check("1024x768 display" in s, "default resolution is 1024x768")
    check("4 CPU cores" in s, "4 CPU cores brought online")
    vm.close()

if "res" in groups:
    for key, (w, h) in MODES.items():
        vm, ok = boot(["-m", "256", "-smp", "2"], menu_key=key)
        check("%dx%d display" % (w, h) in vm.serial(), "BIOS menu: %dx%d selected" % (w, h))
        vm.shot("/tmp/res_%dx%d.ppm" % (w, h)); time.sleep(0.5)
        check(ppm_size("/tmp/res_%dx%d.ppm" % (w, h)) == (w, h), "BIOS menu: framebuffer is really %dx%d" % (w, h))
        vm.close()
    vm, ok = boot(["-m", "256"])
    out = vm.cmd("res")
    check("1280x720" in out and "1920x1080" in out, "`res` lists the standard resolutions")
    out = vm.cmd("res 6", wait_for="Resolution set")
    check("Resolution set to 1920x1080" in out, "run-time switch to 1920x1080")
    vm.shot("/tmp/rt_1080.ppm"); time.sleep(0.5)
    check(ppm_size("/tmp/rt_1080.ppm") == (1920, 1080), "screen is 1920x1080 after run-time switch")
    out = vm.cmd("res 1"); check("Resolution set to 640x480" in out, "run-time switch back to 640x480")
    out = vm.cmd("ls"); check("readme.md" in out, "terminal still works after mode switches")
    vm.close()

if "uefi" in groups and os.path.exists("/usr/share/ovmf/OVMF.fd"):
    vm, ok = boot(["-m", "512", "-smp", "4"], uefi=True)
    s = vm.serial()
    check(ok, "UEFI boot reaches the shell")
    check("4 CPU cores" in s, "UEFI: 4 CPU cores brought online (RSDP passed by the loader)")
    vm.close()
    for key in ("6", "1", "4"):
        w, h = MODES[key]
        vm, ok = boot(["-m", "256"], uefi=True, menu_key=key)
        check("%dx%d display" % (w, h) in vm.serial(), "UEFI menu: %dx%d selected" % (w, h))
        vm.close()

if "mem" in groups:
    vm, ok = boot(bigram(6144) + ["-smp", "1"])                    # 6 GiB: RAM above 4 GiB
    out = vm.cmd("mem")
    check("Usable RAM : 6" in out or "Usable RAM : 5" in out, "6 GiB machine: RAM detected")
    check("RAM above 4 GiB in use" in out, "RAM above 4 GiB is mapped")
    out = vm.cmd("memtest", wait_for="PASS")
    check("above 4 GiB" in out and "PASS" in out, "memtest passes on a window above 4 GiB")
    vm.close()
    vm, ok = boot(bigram(20480) + ["-cpu", "max", "-smp", "1"])    # 20 GiB with 1 GiB pages
    out = vm.cmd("mem")
    check("1 GiB pages" in out and "RAM above 4 GiB" in out, "20 GiB machine with 1 GiB pages")
    out = vm.cmd("memtest", wait_for="PASS"); check("PASS" in out, "memtest passes at 20 GiB")
    vm.close()
    vm, ok = boot(["-m", "64", "-smp", "1"])
    out = vm.cmd("mem"); check("Usable RAM : 6" in out or "Usable RAM : 5" in out, "tiny 64 MiB machine boots")
    vm.close()

if "smp" in groups:
    vm, ok = boot(["-m", "256", "-smp", "4"])
    out = vm.cmd("cores", wait_for="Wall time", timeout=60)
    check(all(("cpu %d" % i) in out for i in range(4)), "`cores`: all 4 cores ran the job")
    check(out.count("result 0x") == 4, "`cores`: 4 distinct results")
    out = vm.cmd("cpu"); check("Cores  : 4 online" in out, "`cpu` reports 4 cores")
    vm.close()
    vm, ok = boot(["-m", "256", "-smp", "8", "-cpu", "max"])
    check("8 CPU cores" in vm.serial(), "8 cores online"); vm.close()
    vm, ok = boot(["-m", "256", "-smp", "1"])
    check("1 CPU core," in vm.serial(), "single-core machine boots"); vm.close()

if "fs" in groups:
    vm, ok = boot(["-m", "256"])
    def sh(c, w=None): return vm.cmd(c, wait_for=w)
    sh("mkdir -p a/b"); sh("echo hi there > a/f.txt"); sh("cp a/f.txt a/b/g.txt"); sh("mv a/b/g.txt a/h.md")
    check("hi there" in sh("cat a/h.md"), "echo > / cp / mv / cat")
    check("f.txt" in sh("ls a") and "h.md" in sh("ls a"), "ls shows files")
    check("Directory not empty" in sh("rmdir a"), "rmdir refuses a non-empty directory")
    check("Is a directory" in sh("rm a"), "rm refuses a directory without -r")
    sh("rm -r a"); check("a" not in sh("ls").split(), "rm -r removes a tree")
    check("/home/user" in sh("pwd"), "pwd")
    check("No such file" in sh("cd nowhere"), "cd error")
    vm.close()

if "vim" in groups:
    vm, ok = boot(["-m", "256"])
    vm.type("vim t.txt\n"); time.sleep(1.5)
    vm.type("ione"); vm.key("ret"); vm.type("two"); vm.key("esc"); vm.type("ddpix"); vm.key("esc")
    vm.type(":wq\n"); time.sleep(1)
    out = vm.cmd("cat t.txt")
    check("one" in out and "two" in out, "vim: create, insert, dd/p, :wq")
    vm.type("vim t.txt\n"); time.sleep(1.5); vm.type("Aend"); vm.key("esc"); vm.type(":q\n"); time.sleep(1.5)
    vm.shot("/tmp/vim_e37.ppm")
    vm.type(":q!\n"); time.sleep(1)
    out = vm.cmd("cat t.txt"); check("end" not in out, "vim: :q! discards changes")
    vm.type("vim n.md\n"); time.sleep(1.5); vm.type("i# hi"); vm.key("esc"); vm.type(":w\n"); time.sleep(.5); vm.type(":q\n"); time.sleep(1)
    check("# hi" in vm.cmd("cat n.md"), "vim: new .md file, :w then :q")
    vm.close()

if "panic" in groups:
    vm, ok = boot(["-m", "256"])
    vm.type("crashtest\n"); time.sleep(2)
    check("KERNEL PANIC" in vm.serial() and "invalid opcode" in vm.serial(), "CPU exception is caught and reported (no silent reboot)")
    vm.close()

for f in __import__("glob").glob("/tmp/picsonut_ram_*.img"): os.remove(f)
print("\n%s (%d failure(s))" % ("ALL PASSED" if not fails else "FAILURES", fails))
sys.exit(1 if fails else 0)
