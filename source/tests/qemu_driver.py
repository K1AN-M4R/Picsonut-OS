"""Tiny QEMU driver used by the tests: boots the ISO headless, types keys through the monitor,
reads the guest's serial mirror of the terminal and takes screenshots."""
import os, socket, subprocess, tempfile, time

KEYS = {'\n': 'ret', ' ': 'spc', '.': 'dot', ',': 'comma', '-': 'minus', '=': 'equal', '/': 'slash', ';': 'semicolon',
        "'": 'apostrophe', '\\': 'backslash', '[': 'bracket_left', ']': 'bracket_right', '`': 'grave_accent', '\t': 'tab',
        '>': 'shift-dot', '<': 'shift-comma', ':': 'shift-semicolon', '"': 'shift-apostrophe', '_': 'shift-minus',
        '+': 'shift-equal', '?': 'shift-slash', '|': 'shift-backslash', '~': 'shift-grave_accent', '{': 'shift-bracket_left',
        '}': 'shift-bracket_right', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4', '%': 'shift-5',
        '^': 'shift-6', '&': 'shift-7', '*': 'shift-8', '(': 'shift-9', ')': 'shift-0'}

class VM:
    def __init__(self, iso, extra=(), uefi=False):
        d = tempfile.mkdtemp(prefix="picsonut-")
        self.sock, self.log = d + "/mon.sock", d + "/serial.log"
        open(self.log, "w").close()
        cmd = ["qemu-system-x86_64", "-display", "none", "-monitor", "unix:%s,server,nowait" % self.sock,
               "-serial", "file:" + self.log, "-cdrom", iso, "-no-reboot"]
        if uefi:
            cmd += ["-bios", "/usr/share/ovmf/OVMF.fd"]
        cmd += list(extra)
        self.p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(self.sock): break
            time.sleep(0.05)
        self.m = socket.socket(socket.AF_UNIX); self.m.connect(self.sock); self._drain()

    def _drain(self):
        self.m.settimeout(0.02)
        try:
            while self.m.recv(65536): pass
        except Exception: pass

    def mon(self, c):
        self.m.sendall((c + "\n").encode()); time.sleep(0.01); self._drain()

    def key(self, k, hold=40): self.mon("sendkey %s %d" % (k, hold)); time.sleep(0.01)

    def type(self, text):
        for ch in text:
            if ch.isalpha() and ch.islower() or ch.isdigit(): k = ch
            elif ch.isalpha(): k = "shift-" + ch.lower()
            else: k = KEYS[ch]
            self.key(k)

    def serial(self): return open(self.log, errors="replace").read()

    def wait(self, text, timeout=30):
        end = time.time() + timeout
        while time.time() < end:
            if text in self.serial(): return True
            time.sleep(0.2)
        return False

    def cmd(self, line, wait_for=None, timeout=20):
        """type a shell command, wait until the next prompt has printed; return the new serial output"""
        before = len(self.serial()); n_prompts = self.serial().count("$ ")
        self.type(line + "\n")
        end = time.time() + timeout
        while time.time() < end:
            s = self.serial()
            if s.count("$ ") > n_prompts and (wait_for is None or wait_for in s[before:]): break
            time.sleep(0.2)
        return self.serial()[before:]

    def shot(self, path):
        self.mon("screendump " + path)
        time.sleep(0.3)

    def close(self):
        try: self.mon("quit")
        except Exception: pass
        try: self.p.wait(5)
        except Exception: self.p.kill()
