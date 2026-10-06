#!/usr/bin/env python3
"""A tiny x86 interpreter + fake PC BIOS, just good enough to run boot/bios.asm (and boot/mbr.asm).

It interprets the *assembly source lines* found in nasm's listing files (so every address is
exactly what nasm assigned), provides fake INT 10h (VBE) / INT 13h / INT 15h services, runs the
loader up to the jump into the kernel, and then checks what the kernel would see:
  * the kernel image at 1 MiB (+ zeroed .bss), EAX magic, EBX -> Multiboot2-style info block
  * the framebuffer tag and memory-map tag, parsed exactly like kernel.c does.

usage: sim_boot.py ISO
"""
import re, struct, sys, os, itertools

M32 = 0xFFFFFFFF
R32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
R8 = ["al", "cl", "dl", "bl", "ah", "ch", "dh", "bh"]
SEG = ["es", "cs", "ss", "ds", "fs", "gs"]


class Fault(Exception):
    pass


# ----------------------------------------------------------------------------- listing parser
def parse_listing(path, extra_equ_files=(), org=0):
    equs = {}
    for ef in extra_equ_files:
        for line in open(ef):
            m = re.match(r"\s*(\w+)\s+equ\s+(.+)", line)
            if m: equs[m.group(1)] = int(eval(m.group(2).split(";")[0]))
    instrs, labels = {}, {}
    pending, scope = [], ""
    lines = open(path).read().splitlines()
    for ln in lines:
        m = re.match(r"^\s*\d+\s+([0-9A-F]{8})?\s*((?:[0-9A-F\[\]()]|<rep [0-9A-Fa-f]+h>)*-?)?\s+(.*)$", ln)
        if not m: continue
        addr = int(m.group(1), 16) + org if m.group(1) else None
        hexs = (m.group(2) or "")
        src = m.group(3).split(";")[0].rstrip()
        mm = re.match(r"^\s*(\.?[A-Za-z_]\w*):(.*)$", src)
        if mm:
            name = mm.group(1)
            if not name.startswith("."): scope = name
            full = scope + name if name.startswith(".") else name
            if addr is not None: labels[full] = addr
            else: pending.append(full)
            src = mm.group(2).strip()
        else:
            src = src.strip()
        if addr is not None and pending:
            for p in pending: labels[p] = addr
            pending = []
        if not src: continue
        em = re.match(r"^(\w+)\s+equ\s+(.+)$", src)
        if em:
            equs[em.group(1)] = None
            equs[em.group(1)] = ("expr", em.group(2))
            continue
        word = src.split()[0].lower()
        if word in ("bits", "org", "times", "align", "db", "dw", "dd", "dq", "%include", "section", "global", "%define") or addr is None:
            continue
        nbytes = len(re.sub(r"[\[\]()-]", "", hexs)) // 2
        instrs[addr] = (src, nbytes, scope)
    return instrs, labels, equs


# ----------------------------------------------------------------------------- machine
class Machine:
    def __init__(self, instrs, labels, equs, disk, sector_size=2048, a20_mode="int15", e820_entry_size=24):
        self.instrs, self.labels, self.equs = instrs, labels, equs
        self.mem = bytearray(0x400000)
        self.r = dict.fromkeys(R32, 0)
        self.seg = dict.fromkeys(SEG, 0)
        self.flags = dict(zf=0, cf=0, sf=0, of=0)
        self.ip = 0x7C00
        self.pm = False
        self.cr0 = 0
        self.a20 = False
        self.a20_mode = a20_mode
        self.disk, self.sector_size = disk, sector_size
        self.console = ""
        self.vbe_mode = None
        self.e820_entry_size = e820_entry_size
        self.halted = False
        self.steps = 0
        self.kbc_a20 = False
        self.symcache = {}
        self.stop_at = None

    # ---- memory
    def lin(self, seg, off):
        if self.pm: a = off & M32
        else: a = ((self.seg[seg] << 4) + (off & 0xFFFF)) & 0x1FFFFF
        if not self.a20: a &= 0xFFFFF
        return a
    def rd(self, a, size):
        if not self.a20: a &= 0xFFFFF
        return int.from_bytes(self.mem[a:a + size], "little")
    def wr(self, a, size, v):
        if not self.a20: a &= 0xFFFFF
        self.mem[a:a + size] = (v & ((1 << (8 * size)) - 1)).to_bytes(size, "little")

    # ---- registers
    def getreg(self, n):
        if n in self.r: return self.r[n], 4
        if n in R16: return self.r["e" + n] & 0xFFFF, 2
        if n in R8:
            i = R8.index(n)
            return ((self.r[R32[i & 3]] >> (8 if i >= 4 else 0)) & 0xFF), 1
        if n in self.seg: return self.seg[n], 2
        raise Fault("reg " + n)
    def setreg(self, n, v):
        if n in self.r: self.r[n] = v & M32; return
        if n in R16:
            e = "e" + n; self.r[e] = (self.r[e] & ~0xFFFF) | (v & 0xFFFF); return
        if n in R8:
            i = R8.index(n); e = R32[i & 3]; sh = 8 if i >= 4 else 0
            self.r[e] = (self.r[e] & ~(0xFF << sh)) | ((v & 0xFF) << sh); return
        if n in self.seg: self.seg[n] = v & 0xFFFF; return
        raise Fault("reg " + n)
    def isreg(self, n): return n in self.r or n in R16 or n in R8 or n in self.seg

    # ---- expression / operand evaluation
    def sym(self, name, scope):
        if name.startswith("."): name = scope + name
        if name in self.labels: return self.labels[name]
        if name in self.equs:
            e = self.equs[name]
            if isinstance(e, tuple): e = self.eval(e[1], "")
            self.equs[name] = e
            return e
        raise Fault("unknown symbol " + name)
    def eval(self, expr, scope, regs=False):
        expr = expr.strip()
        expr = re.sub(r"'(.{1,4})'", lambda m: str(int.from_bytes(m.group(1).encode(), "little")), expr)
        expr = expr.replace("/", "//")
        def sub(m):
            w = m.group(0)
            if regs and self.isreg(w): return str(self.getreg(w)[0])
            if re.match(r"^0[xX]", w) or w.isdigit(): return w
            return str(self.sym(w, scope))
        expr = re.sub(r"\.?[A-Za-z_]\w*|0[xX][0-9A-Fa-f]+|\d+", sub, expr)
        return eval(expr, {}, {})

    def parse_op(self, op, scope, size_hint=None):
        """returns ('reg',name,size) | ('mem',seg,off,size) | ('imm',value)"""
        op = op.strip()
        size = None
        m = re.match(r"^(byte|word|dword)\s+(.*)$", op)
        if m:
            size = {"byte": 1, "word": 2, "dword": 4}[m.group(1)]; op = m.group(2).strip()
        if op.startswith("["):
            inner = op[1:op.rindex("]")]
            seg = "ds"
            sm = re.match(r"^(es|ds|fs|gs|ss|cs):(.*)$", inner.strip())
            if sm: seg, inner = sm.group(1), sm.group(2)
            off = self.eval(inner, scope, regs=True)
            return ("mem", seg, off, size or size_hint)
        if self.isreg(op):
            v, sz = self.getreg(op); return ("reg", op, sz)
        return ("imm", self.eval(op, scope) & M32)

    def read(self, o, size=None):
        if o[0] == "imm": return o[1]
        if o[0] == "reg": return self.getreg(o[1])[0]
        sz = o[3] or size
        if not sz: raise Fault("unsized memory operand")
        return self.rd(self.lin(o[1], o[2]), sz)
    def write(self, o, v, size=None):
        if o[0] == "reg": self.setreg(o[1], v); return
        if o[0] == "mem":
            sz = o[3] or size
            if not sz: raise Fault("unsized memory operand")
            self.wr(self.lin(o[1], o[2]), sz, v); return
        raise Fault("write to imm")
    def osize(self, o, other=None):
        if o[0] == "reg": return o[2]
        if o[0] == "mem" and o[3]: return o[3]
        if other is not None:
            if other[0] == "reg": return other[2]
            if other[0] == "mem" and other[3]: return other[3]
        raise Fault("unknown operand size")

    # ---- flags
    def setf(self, res, size, cf=None, of=None):
        bits = 8 * size; mask = (1 << bits) - 1
        self.flags["zf"] = int((res & mask) == 0)
        self.flags["sf"] = int(bool(res & (1 << (bits - 1))))
        if cf is not None: self.flags["cf"] = cf
        if of is not None: self.flags["of"] = of
    def alu(self, op, a, b, size):
        bits = 8 * size; mask = (1 << bits) - 1; sign = 1 << (bits - 1)
        if op in ("add", "adc"):
            c = self.flags["cf"] if op == "adc" else 0
            r = a + b + c
            self.setf(r, size, cf=int(r > mask), of=int(bool(~(a ^ b) & (a ^ r) & sign)))
        elif op in ("sub", "cmp"):
            r = a - b
            self.setf(r, size, cf=int(a < b), of=int(bool((a ^ b) & (a ^ r) & sign)))
        elif op in ("and", "test"): r = a & b; self.setf(r, size, cf=0, of=0)
        elif op == "or": r = a | b; self.setf(r, size, cf=0, of=0)
        elif op == "xor": r = a ^ b; self.setf(r, size, cf=0, of=0)
        else: raise Fault(op)
        return r & mask

    def cond(self, cc):
        f = self.flags
        t = {"z": f["zf"], "e": f["zf"], "nz": not f["zf"], "ne": not f["zf"], "c": f["cf"], "b": f["cf"], "nc": not f["cf"],
             "ae": not f["cf"], "nb": not f["cf"], "be": f["cf"] or f["zf"], "a": not (f["cf"] or f["zf"]),
             "s": f["sf"], "ns": not f["sf"], "o": f["of"], "no": not f["of"]}
        return bool(t[cc])

    # ---- stack
    def push(self, v, size):
        self.r["esp"] = (self.r["esp"] - size) & M32 if self.pm else (self.r["esp"] & ~0xFFFF) | ((self.r["esp"] - size) & 0xFFFF)
        self.wr(self.lin("ss", self.r["esp"] if self.pm else self.r["esp"] & 0xFFFF), size, v)
    def pop(self, size):
        a = self.lin("ss", self.r["esp"] if self.pm else self.r["esp"] & 0xFFFF)
        v = self.rd(a, size)
        self.r["esp"] = (self.r["esp"] + size) & M32 if self.pm else (self.r["esp"] & ~0xFFFF) | ((self.r["esp"] + size) & 0xFFFF)
        return v

    # ---- BIOS services
    def int_(self, n):
        ax = self.r["eax"] & 0xFFFF; ah = ax >> 8
        es_di = lambda: self.lin("es", self.r["edi"] & 0xFFFF)
        f = self.flags
        if n == 0x10:
            if ah == 0x0E:
                self.console += chr(ax & 0xFF); return
            if ax == 0x4F00:
                a = es_di()
                assert self.rd(a, 4) == int.from_bytes(b"VBE2", "little"), "loader must request VBE2 info"
                self.mem[a:a + 4] = b"VESA"; self.wr(a + 4, 2, 0x0300)
                self.wr(a + 0x0E, 2, 0x0200); self.wr(a + 0x10, 2, 0x0000)   # mode list at 0000:0200 (ROM-like)
                modes = [0x101, 0x112, 0x115, 0x118, 0x11B, 0x14C, 0xFFFF]
                for i, md in enumerate(modes): self.wr(0x200 + 2 * i, 2, md)
                self.setreg("ax", 0x004F); return
            if ax == 0x4F01:
                cx = self.r["ecx"] & 0xFFFF; a = es_di()
                info = {0x101: (640, 480, 8, 4), 0x112: (640, 480, 32, 6), 0x115: (800, 600, 32, 6),
                        0x118: (1024, 768, 32, 6), 0x11B: (1280, 1024, 32, 6), 0x14C: (1600, 1200, 32, 6)}
                if cx not in info: self.setreg("ax", 0x014F); return
                w, h, bpp, model = info[cx]
                self.mem[a:a + 256] = bytes(256)
                self.wr(a, 2, 0x009B); self.wr(a + 0x10, 2, w * bpp // 8); self.wr(a + 0x12, 2, w); self.wr(a + 0x14, 2, h)
                self.mem[a + 0x19] = bpp; self.mem[a + 0x1B] = model
                self.mem[a + 0x1F:a + 0x25] = bytes([8, 16, 8, 8, 8, 0]) if bpp == 32 else bytes(6)
                self.wr(a + 0x28, 4, 0xFD000000)
                self.setreg("ax", 0x004F); return
            if ax == 0x4F02:
                self.vbe_mode = self.r["ebx"] & 0xFFFF; self.setreg("ax", 0x004F); return
            if ax == 0x4F06:
                self.setreg("ax", 0x004F); self.setreg("bx", 1024 * 4 + 64); return   # BIOS pitch with padding
            raise Fault("int 10h ax=%#x" % ax)
        if n == 0x13:
            ds_si = self.lin("ds", self.r["esi"] & 0xFFFF)
            if ah == 0x42:
                assert self.mem[ds_si] == 0x10
                cnt, off, seg = struct.unpack_from("<HHH", self.mem, ds_si + 2)
                lba = struct.unpack_from("<Q", self.mem, ds_si + 8)[0]
                dst = (seg << 4) + off
                data = self.disk[lba * self.sector_size:(lba + cnt) * self.sector_size]
                assert len(data) == cnt * self.sector_size, "read beyond end of disk"
                assert (dst & 0xFFFF) + len(data) <= 0x10000, "disk read crosses 64KiB boundary"
                self.mem[dst:dst + len(data)] = data
                f["cf"] = 0; self.setreg("ax", 0); return
            if ah == 0x48:
                a = ds_si
                self.wr(a, 2, 0x1E); self.wr(a + 0x18, 2, self.sector_size)
                f["cf"] = 0; self.setreg("ax", 0); return
            raise Fault("int 13h ah=%#x" % ah)
        if n == 0x15:
            if ax == 0x2401:
                if self.a20_mode == "int15": self.a20 = True; f["cf"] = 0; self.setreg("ax", 0)
                else: f["cf"] = 1; self.setreg("ax", 0x8600)
                return
            if (self.r["eax"] & M32) == 0xE820:
                assert self.r["edx"] == 0x534D4150 and (self.r["ecx"] & M32) >= 20
                idx = self.r["ebx"]
                mp = E820_MAP
                a = es_di()
                e = mp[idx]
                self.wr(a, 8, e[0]); self.wr(a + 8, 8, e[1]); self.wr(a + 16, 4, e[2])
                n_ret = self.e820_entry_size
                if n_ret == 24: self.wr(a + 20, 4, 1)
                self.r["eax"] = 0x534D4150; self.r["ecx"] = n_ret
                self.r["ebx"] = idx + 1 if idx + 1 < len(mp) else 0
                f["cf"] = 0; return
            raise Fault("int 15h ax=%#x" % ax)
        raise Fault("int %#x" % n)

    def port_in(self, p):
        if p == 0x92: return 2 if (self.a20 and self.a20_mode == "fast") else 0
        if p == 0x64: return 0
        raise Fault("in %#x" % p)
    def port_out(self, p, v):
        if p == 0x92:
            if self.a20_mode == "fast" and v & 2: self.a20 = True
            return
        if p == 0x64:
            self.kbc_cmd = v; return
        if p == 0x60:
            if self.a20_mode == "kbc" and getattr(self, "kbc_cmd", 0) == 0xD1 and v == 0xDF: self.a20 = True
            return
        raise Fault("out %#x" % p)

    # ---- run
    def step(self):
        ins = self.instrs.get(self.ip)
        if ins is None:
            raise Fault("executing outside of known code at %#x" % self.ip)
        src, size, scope = ins
        self.steps += 1
        mn, _, rest = src.partition(" ")
        mn = mn.lower(); rest = re.sub(r'^(short|near)\s+', '', rest.strip())
        ops = [o for o in re.split(r",(?![^\[]*\])", rest)] if rest else []
        nxt = self.ip + size
        if mn == "rep":
            sub = rest.lower()
            cnt_reg = "ecx" if self.pm else "cx"
            while self.getreg(cnt_reg)[0]:
                if sub == "stosd": self.wr(self.lin("es", self.getreg("edi" if self.pm else "di")[0]), 4, self.r["eax"]); self.step_str(4, "edi")
                elif sub == "movsd":
                    v = self.rd(self.lin("ds", self.getreg("esi" if self.pm else "si")[0]), 4)
                    self.wr(self.lin("es", self.getreg("edi" if self.pm else "di")[0]), 4, v)
                    self.step_str(4, "esi"); self.step_str(4, "edi")
                elif sub == "movsw":
                    v = self.rd(self.lin("ds", self.getreg("si")[0]), 2)
                    self.wr(self.lin("es", self.getreg("di")[0]), 2, v)
                    self.step_str(2, "si"); self.step_str(2, "di")
                else: raise Fault("rep " + sub)
                self.setreg(cnt_reg, self.getreg(cnt_reg)[0] - 1)
            self.ip = nxt; return
        P = lambda i, hint=None: self.parse_op(ops[i], scope, hint)
        if mn in ("cli", "sti", "cld", "nop"): pass
        elif mn == "hlt": self.halted = True; return
        elif mn == "mov":
            if ops[0].strip() == "cr0":
                self.cr0 = self.r[ops[1].strip()]; self.pm = bool(self.cr0 & 1)
            elif ops[1].strip() == "cr0": self.setreg(ops[0].strip(), self.cr0)
            else:
                d, s = P(0), P(1, None)
                sz = self.osize(d, s) if d[0] != "reg" or s[0] != "reg" else d[2]
                if s[0] == "mem" and not s[3]: s = ("mem", s[1], s[2], sz)
                self.write(d, self.read(s), sz)
        elif mn == "movzx":
            d, s = P(0), P(1)
            self.write(d, self.read(s, s[2] if s[0] == "reg" else s[3]), d[2])
        elif mn in ("add", "adc", "sub", "cmp", "and", "or", "xor", "test"):
            d, s = P(0), P(1)
            sz = self.osize(d, s)
            if s[0] == "mem" and not s[3]: s = ("mem", s[1], s[2], sz)
            if d[0] == "mem" and not d[3]: d = ("mem", d[1], d[2], sz)
            a, b = self.read(d, sz), self.read(s, sz)
            if s[0] == "imm" and sz < 4: b &= (1 << 8 * sz) - 1
            if s[0] == "imm" and sz == 4: b &= M32
            r = self.alu(mn, a, b, sz)
            if mn not in ("cmp", "test"): self.write(d, r, sz)
        elif mn in ("inc", "dec"):
            d = P(0); sz = self.osize(d); a = self.read(d, sz); cf = self.flags["cf"]
            r = self.alu("add" if mn == "inc" else "sub", a, 1, sz); self.flags["cf"] = cf; self.write(d, r, sz)
        elif mn == "neg":
            d = P(0); sz = self.osize(d); a = self.read(d, sz)
            r = self.alu("sub", 0, a, sz); self.flags["cf"] = int(a != 0); self.write(d, r, sz)
        elif mn in ("shl", "shr"):
            d = P(0); sz = self.osize(d); a = self.read(d, sz)
            cnt = self.read(P(1)) & 31
            mask = (1 << 8 * sz) - 1
            r = ((a << cnt) if mn == "shl" else (a >> cnt)) & mask
            if cnt: self.setf(r, sz, cf=((a << cnt) >> (8 * sz)) & 1 if mn == "shl" else (a >> (cnt - 1)) & 1)
            self.write(d, r, sz)
        elif mn == "mul":
            s = P(0); sz = self.osize(s); assert sz == 4
            r = (self.r["eax"] * self.read(s, 4))
            self.r["eax"], self.r["edx"] = r & M32, (r >> 32) & M32
            self.flags["cf"] = self.flags["of"] = int(self.r["edx"] != 0)
        elif mn == "imul":
            assert len(ops) == 2
            d, s = P(0), P(1)
            r = (self.read(d) * self.read(s)) & M32
            self.write(d, r, 4)
        elif mn == "bt":
            d, s = P(0), P(1); self.flags["cf"] = (self.read(d) >> (self.read(s) & 31)) & 1
        elif mn == "push":
            o = P(0); sz = 4 if self.pm else 2
            if o[0] == "reg" and o[2] == 4 and not self.pm: sz = 4
            self.push(self.read(o), o[2] if o[0] == "reg" else sz)
        elif mn == "pop":
            o = P(0); self.write(o, self.pop(o[2]), o[2])
        elif mn == "lodsb":
            self.setreg("al", self.rd(self.lin("ds", self.getreg("si")[0]), 1)); self.step_str(1, "si")
        elif mn == "call":
            tgt = self.read(P(0)); self.push(nxt, 4 if self.pm else 2); nxt = tgt
        elif mn == "ret": nxt = self.pop(4 if self.pm else 2)
        elif mn == "jmp":
            if ":" in rest:
                seg, off = rest.split(":"); seg = self.eval(seg, scope); off = self.eval(off, scope)
                self.seg["cs"] = seg
                if self.pm: nxt = off
                else:
                    assert self.cr0 & 1 or seg == 0, "real-mode far jmp must keep CS=0"
                    if self.cr0 & 1: assert seg == 0x08; self.pm = True
                    nxt = off
                    if not (self.cr0 & 1) and off == 0x7C00 and self.on_jump_7c00: self.on_jump_7c00(self)
            else:
                o = P(0); nxt = self.read(o) & (M32 if self.pm else 0xFFFF)
        elif mn.startswith("j") and mn[1:] in ("z", "nz", "c", "nc", "b", "ae", "be", "a", "e", "ne", "s", "ns", "o", "no", "nb"):
            if self.cond(mn[1:]): nxt = self.read(P(0))
        elif mn == "int": self.int_(self.read(P(0)))
        elif mn == "cpuid":
            eax = self.r["eax"]
            if eax == 0x80000000: self.r.update(eax=0x80000008)
            elif eax == 0x80000001: self.r.update(eax=0, ebx=0, ecx=0, edx=(1 << 29) | (1 << 11))
            else: raise Fault("cpuid %#x" % eax)
        elif mn == "in":
            d, s = P(0), ops[1].strip()
            port = self.getreg("dx")[0] if s == "dx" else self.eval(s, scope)
            self.setreg("al", self.port_in(port))
        elif mn == "out":
            s = ops[0].strip()
            port = self.getreg("dx")[0] if s == "dx" else self.eval(s, scope)
            self.port_out(port, self.getreg("al")[0])
        elif mn == "lgdt":
            o = P(0); a = self.lin(o[1], o[2])
            limit = self.rd(a, 2); base = self.rd(a + 2, 4)
            self.gdt = (base, limit)
            assert limit == 23, "GDT must have 3 descriptors"
            assert self.rd(base + 8, 8) == 0x00CF9A000000FFFF and self.rd(base + 16, 8) == 0x00CF92000000FFFF
        else: raise Fault("unimplemented: " + src)
        self.ip = nxt

    def step_str(self, size, reg):
        self.setreg(reg, self.getreg(reg)[0] + size)

    def run(self, max_steps=3_000_000):
        while not self.halted and self.steps < max_steps:
            if self.stop_at is not None and self.pm and self.ip == self.stop_at: return "kernel"
            self.step()
        return "halted" if self.halted else "timeout"

    on_jump_7c00 = None


E820_MAP = [
    (0x0, 0x9FC00, 1), (0x9FC00, 0x400, 2), (0xF0000, 0x10000, 2),
    (0x100000, 0x7FE0000 - 0x100000, 1), (0x7FE0000, 0x20000, 2), (0xFFFC0000, 0x40000, 2),
]


# ----------------------------------------------------------------------------- kernel-side checks
def kernel_view(mem, mbi):
    """Port of parse_mbi()/total_ram_kb() from src/kernel.c."""
    total = struct.unpack_from("<I", mem, mbi)[0]
    p, end = mbi + 8, mbi + total
    fb = mm = None
    while p < end:
        typ, size = struct.unpack_from("<II", mem, p)
        if typ == 0: break
        if typ == 6: mm = p
        if typ == 8:
            a = struct.unpack_from("<Q", mem, p + 8)[0]
            bpp, ftype = mem[p + 28], mem[p + 29]
            if ftype == 1 and bpp == 32 and a < 0x100000000:
                fb = dict(addr=a, pitch=struct.unpack_from("<I", mem, p + 16)[0], w=struct.unpack_from("<I", mem, p + 20)[0],
                          h=struct.unpack_from("<I", mem, p + 24)[0], rpos=mem[p + 32], gpos=mem[p + 34], bpos=mem[p + 36])
        p += (size + 7) & ~7
    kb = 0
    if mm:
        size, esz = struct.unpack_from("<II", mem, mm + 4)
        off = 16
        while off + esz <= size:
            e = mm + off
            if struct.unpack_from("<I", mem, e + 16)[0] == 1: kb += struct.unpack_from("<Q", mem, e + 8)[0]
            off += esz
        kb //= 1024
    return fb, kb


def run_case(name, iso, bios_img, bios_lst, kernel_bin, kinfo, mbr_lst=None, **kw):
    ok = True
    def check(c, msg):
        nonlocal ok
        print("   %s %s" % ("PASS" if c else "FAIL", msg)); ok &= bool(c)
    print("== " + name)
    instrs, labels, equs = parse_listing(bios_lst, [kinfo], org=0x7C00)
    sector = kw.pop("sector_size", 2048)
    # the "disk": for 2048-byte devices the ISO itself; for HDD mode the same bytes in 512-byte units
    mach = Machine(instrs, labels, equs, iso, sector_size=sector, **kw)
    mach.stop_at = equs["KERNEL_ENTRY32"]
    mach.r["edx"] = 0x80
    mach.r["esp"] = 0
    if mbr_lst:
        minstrs, mlabels, mequs = parse_listing(mbr_lst, org=0x600)
        mbr_map = dict(minstrs)
        # initial MBR executes at 0x7C00 but was assembled for 0x600: position independent until the jump
        mach.mem[0x7C00:0x7E00] = iso[:512]
        mach.instrs = {a - 0x600 + 0x7C00: v for a, v in minstrs.items() if a < 0x61E}
        mach.instrs.update(minstrs)
        mach.labels, mach.equs = mlabels, mequs
        def switch(m):
            m.instrs, m.labels, m.equs = instrs, labels, equs
            loaded = bytes(m.mem[0x7C00:0x7C00 + 2048])
            check(loaded == bios_img_bytes, "MBR loaded exactly the 2048-byte BIOS loader image to 0x7C00")
        mach.on_jump_7c00 = switch
        # the MBR code at its relocated position must be a copy of itself:
    else:
        mach.mem[0x7C00:0x7C00 + 2048] = iso[kernel_lba_of(iso, "BIOS"):kernel_lba_of(iso, "BIOS") + 2048]
    try:
        res = mach.run()
    except Fault as e:
        print("   FAULT:", e, "at ip %#x" % mach.ip); return False
    check(res == "kernel", "loader reached the kernel entry point (%s, %d steps)" % (res, mach.steps))
    if res != "kernel":
        print("   console:", repr(mach.console)); return False
    check(mach.a20, "A20 line enabled")
    check(mach.vbe_mode == (0x118 | 0x4000), "VBE: picked 1024x768x32 linear (mode %#x)" % (mach.vbe_mode or 0))
    check(mach.r["eax"] == 0x36D76289, "EAX = multiboot2 loader magic")
    kernel = open(kernel_bin, "rb").read()
    memsz = equs["KERNEL_MEMSZ"]
    img = bytes(mach.mem[0x100000:0x100000 + memsz])
    check(img == kernel + b"\0" * (memsz - len(kernel)), "kernel image at 1 MiB, .bss zeroed (%d bytes)" % memsz)
    fb, kb = kernel_view(mach.mem, mach.r["ebx"])
    check(fb is not None, "kernel parses the framebuffer tag")
    if fb:
        check((fb["w"], fb["h"]) == (1024, 768), "framebuffer %dx%d" % (fb["w"], fb["h"]))
        check(fb["addr"] == 0xFD000000, "framebuffer address %#x" % fb["addr"])
        check(fb["pitch"] == 1024 * 4 + 64, "pitch taken from VBE 4F06 (%d)" % fb["pitch"])
        check((fb["rpos"], fb["gpos"], fb["bpos"]) == (16, 8, 0), "colour layout R%d G%d B%d" % (fb["rpos"], fb["gpos"], fb["bpos"]))
    exp = sum(l for _, l, t in E820_MAP if t == 1) // 1024 // 1024
    check(kb // 1024 == exp, "kernel computes %d MiB of RAM from the E820 map (expected %d)" % (kb // 1024, exp))
    return ok


def kernel_lba_of(iso, which):
    # El Torito initial entry (BIOS) via the boot catalog, like a real BIOS would
    cat = struct.unpack_from("<I", iso, 17 * 2048 + 71)[0]
    lba = struct.unpack_from("<I", iso, cat * 2048 + 32 + 8)[0]
    return lba * 2048


if __name__ == "__main__":
    iso = open(sys.argv[1], "rb").read()
    b = "build/"
    bios_img_bytes = open(b + "bios.img", "rb").read()
    all_ok = True
    # patched BIOS image as found in the ISO
    lba = kernel_lba_of(iso, "BIOS") // 2048
    bios_img_bytes = iso[lba * 2048: lba * 2048 + 2048]
    for a20, e820 in itertools.product(("int15", "fast", "kbc"), (24, 20)):
        all_ok &= run_case("CD boot, A20 via %s, E820 entries of %d bytes" % (a20, e820), iso, bios_img_bytes,
                           b + "bios.lst", b + "kernel.bin", b + "kernel_info.inc", a20_mode=a20, e820_entry_size=e820)
    # USB / hard-disk style boot through the hybrid MBR: device sectors are 512 bytes
    all_ok &= run_case("USB/HDD boot via hybrid MBR (512-byte sectors)", iso, bios_img_bytes, b + "bios.lst", b + "kernel.bin",
                       b + "kernel_info.inc", mbr_lst=b + "mbr.lst", sector_size=512, a20_mode="fast")
    print("\nALL SIMULATIONS PASSED" if all_ok else "\nSIMULATION FAILURES")
    sys.exit(0 if all_ok else 1)
