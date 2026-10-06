/* Picsonut OS - a tiny 64-bit hobby OS: framebuffer desktop + terminal.
 * Booted by the Picsonut loaders (BIOS + UEFI) or GRUB2 via Multiboot2.
 * This file: boot info, drawing, terminal, keyboard, video modes, shell.
 * See also: mem.c (RAM/paging/heap) cpu.c (IDT/APIC/SMP) fs.c cmds.c (filesystem) vim.c (editor). */
#include "kernel.h"
#include "font.h"

/* ------------------------------------------------------------ boot info */
static u8 *fb; static u32 fbw, fbh, pitch;
static u8 rpos = 16, gpos = 8, bpos = 0;
static u8 *mmap_tag;
static u64 fb_phys;
static int have_fb;
static u64 rsdp_ptr; static u8 rsdp_copy[36]; static int have_rsdp_copy;
#define FB_WINDOW (64ULL * 1024 * 1024)

static void parse_mbi(u64 addr) {
    u8 *p = (u8 *)addr;
    u32 total = *(u32 *)p;
    u8 *end = p + total;
    p += 8;
    while (p < end) {
        u32 type = *(u32 *)p, size = *(u32 *)(p + 4);
        if (type == 0 || size < 8) break;
        if (type == 6) mmap_tag = p;
        if (type == 8) {
            u64 a = *(u64 *)(p + 8);
            u32 bpp = p[28], ftype = p[29];
            if (ftype == 1 && bpp == 32) {
                fb_phys = a; fb = (u8 *)(usize)a;
                pitch = *(u32 *)(p + 16);
                fbw = *(u32 *)(p + 20);
                fbh = *(u32 *)(p + 24);
                rpos = p[32]; gpos = p[34]; bpos = p[36];
                have_fb = 1;
            }
        }
        if ((type == 14 || type == 15) && size >= 28) { memcpy(rsdp_copy, p + 8, size - 8 > 36 ? 36 : size - 8); have_rsdp_copy = 1; }
        if (type == 0x1000 && size >= 16) rsdp_ptr = *(u64 *)(p + 8);   /* Picsonut UEFI loader: pointer to the ACPI RSDP */
        p += (size + 7) & ~7u;
    }
}

static void vga_fail(const char *msg) {
    volatile u16 *v = (volatile u16 *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) v[i] = 0x0720;
    for (int i = 0; msg[i]; i++) v[i] = 0x0C00 | (u8)msg[i];
    for (;;) __asm__ volatile("cli; hlt");
}

/* ------------------------------------------------------------ serial (COM1, debug mirror of the terminal) */
static int serial_ok;
static void serial_init(void) {
    outb(0x3F9, 0); outb(0x3FB, 0x80); outb(0x3F8, 1); outb(0x3F9, 0);   /* 115200 baud */
    outb(0x3FB, 0x03); outb(0x3FA, 0xC7); outb(0x3FC, 0x0B);
    outb(0x3FF, 0xAE);
    serial_ok = inb(0x3FF) == 0xAE;                                      /* scratch register: is there a UART? */
}
static void serial_putc(char c) {
    if (!serial_ok) return;
    for (int i = 0; i < 20000 && !(inb(0x3FD) & 0x20); i++) pause();
    outb(0x3F8, (u8)c);
}
void serial_write(const char *s) { for (; *s; s++) { if (*s == '\n') serial_putc('\r'); serial_putc(*s); } }

/* ------------------------------------------------------------ drawing */
static inline u32 conv(u32 c) {
    return (((c >> 16) & 255u) << rpos) | (((c >> 8) & 255u) << gpos) | ((c & 255u) << bpos);
}
static void fill(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fbw) w = (int)fbw - x;
    if (y + h > (int)fbh) h = (int)fbh - y;
    if (w <= 0 || h <= 0) return;
    c = conv(c);
    for (int j = 0; j < h; j++) {
        u32 *p = (u32 *)(fb + (usize)(y + j) * pitch) + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}
static void glyph_s(int x, int y, char ch, u32 fg, u32 bg, int s) {
    if (ch < 32 || ch > 126) ch = '?';
    const u8 *g = font8x16[ch - 32];
    if (x < 0 || y < 0 || x + 8 * s > (int)fbw || y + 16 * s > (int)fbh) return;
    fg = conv(fg); bg = conv(bg);
    for (int r = 0; r < 16; r++) {
        u8 b = g[r];
        for (int sy = 0; sy < s; sy++) {
            u32 *p = (u32 *)(fb + (usize)(y + r * s + sy) * pitch) + x;
            for (int c = 0; c < 8; c++) {
                u32 v = (b & (0x80 >> c)) ? fg : bg;
                for (int sx = 0; sx < s; sx++) p[c * s + sx] = v;
            }
        }
    }
}
static void glyph(int x, int y, char ch, u32 fg, u32 bg) { glyph_s(x, y, ch, fg, bg, 1); }
static void text(int x, int y, const char *s, u32 fg, u32 bg) {
    for (; *s; s++, x += 8) glyph(x, y, *s, fg, bg);
}

/* ------------------------------------------------------------ clock */
int t_h, t_m, t_s, t_d, t_mo, t_y;
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }
void rtc_read(void) {
    for (int g = 0; g < 100000 && (cmos(0x0A) & 0x80); g++) pause();
    int s = cmos(0), m = cmos(2), h = cmos(4), d = cmos(7), mo = cmos(8), y = cmos(9);
    u8 rb = cmos(0x0B);
    int pm = h & 0x80; h &= 0x7F;
    if (!(rb & 4)) { s = bcd(s); m = bcd(m); h = bcd(h); d = bcd(d); mo = bcd(mo); y = bcd(y); }
    if (!(rb & 2) && pm) h = (h % 12) + 12;
    t_s = s; t_m = m; t_h = h; t_d = d; t_mo = mo; t_y = 2000 + y;
}
static char *p2(char *p, int v) { *p++ = '0' + (v / 10) % 10; *p++ = '0' + v % 10; return p; }
static char *p4(char *p, int v) { p = p2(p, v / 100); return p2(p, v % 100); }
static void fmt_time(char *b) { b = p2(b, t_h); *b++ = ':'; b = p2(b, t_m); *b++ = ':'; b = p2(b, t_s); *b = 0; }
static void fmt_date(char *b) { b = p4(b, t_y); *b++ = '-'; b = p2(b, t_mo); *b++ = '-'; b = p2(b, t_d); *b = 0; }

static void draw_clock(void) {
    char buf[32], d[16], t[16];
    fmt_time(t); fmt_date(d);
    int i = 0;
    for (int k = 0; d[k]; k++) buf[i++] = d[k];
    buf[i++] = ' '; buf[i++] = ' ';
    for (int k = 0; t[k]; k++) buf[i++] = t[k];
    buf[i] = 0;
    int x = (int)fbw - (int)strlen(buf) * 8 - 14;
    text(x, 4, buf, 0xc9d1d9, COL_PANEL);
}

/* ------------------------------------------------------------ terminal */
#define MAXC 240
#define MAXR 68
static char cells[MAXR][MAXC];
static u32  cfg[MAXR][MAXC];
static u8   cinv[MAXR][MAXC];
int term_cols, term_rows;
static int term_px, term_py, fscale = 1;
static int cur_r, cur_c, cursor_vis, dirty_all;
static u32 cur_fg = COL_FG;

static void draw_cell(int r, int c, int inv) {
    int x = term_px + c * 8 * fscale, y = term_py + r * 16 * fscale;
    inv ^= cinv[r][c];
    if (inv) glyph_s(x, y, cells[r][c], COL_WIN_BG, cfg[r][c], fscale);
    else     glyph_s(x, y, cells[r][c], cfg[r][c], COL_WIN_BG, fscale);
}
static void cursor_hide(void) { if (cursor_vis) { draw_cell(cur_r, cur_c, 0); cursor_vis = 0; } }
static void cursor_show(void) { draw_cell(cur_r, cur_c, 1); cursor_vis = 1; }
void term_cursor_off(void) { cursor_hide(); }
static void term_redraw_all(void) {
    for (int r = 0; r < term_rows; r++)
        for (int c = 0; c < term_cols; c++) draw_cell(r, c, 0);
}
static void term_clear_cells(void) {
    for (int r = 0; r < MAXR; r++) for (int c = 0; c < MAXC; c++) { cells[r][c] = ' '; cfg[r][c] = cur_fg; cinv[r][c] = 0; }
}
void term_set(int r, int c, char ch, u32 fg, int inv) {
    if (r < 0 || c < 0 || r >= term_rows || c >= term_cols) return;
    if (cells[r][c] == ch && cfg[r][c] == fg && cinv[r][c] == (u8)inv) return;
    cells[r][c] = ch; cfg[r][c] = fg; cinv[r][c] = (u8)inv;
    draw_cell(r, c, 0);
}
static void term_newline(void) {
    cur_c = 0; cur_r++;
    if (cur_r >= term_rows) {
        memmove(cells[0], cells[1], (usize)(MAXR - 1) * MAXC);
        memmove(cfg[0], cfg[1], (usize)(MAXR - 1) * MAXC * sizeof(u32));
        memmove(cinv[0], cinv[1], (usize)(MAXR - 1) * MAXC);
        for (int c = 0; c < MAXC; c++) { cells[term_rows - 1][c] = ' '; cfg[term_rows - 1][c] = cur_fg; cinv[term_rows - 1][c] = 0; }
        cur_r = term_rows - 1;
        dirty_all = 1;
    }
}
static void term_putc(char ch) {
    if (ch == '\n') { serial_putc('\r'); serial_putc('\n'); term_newline(); return; }
    if (ch == '\r') { cur_c = 0; return; }
    if (ch == '\t') { do term_putc(' '); while (cur_c % 4); return; }
    serial_putc(ch);
    cells[cur_r][cur_c] = ch; cfg[cur_r][cur_c] = cur_fg; cinv[cur_r][cur_c] = 0;
    if (!dirty_all) draw_cell(cur_r, cur_c, 0);
    if (++cur_c >= term_cols) term_newline();
}
void term_putc_raw(char ch) { term_putc(ch); }
void term_write(const char *s) {
    cursor_hide();
    while (*s) term_putc(*s++);
    if (dirty_all) { term_redraw_all(); dirty_all = 0; }
    cursor_show();
}
void term_write_col(const char *s, u32 col) { u32 o = cur_fg; cur_fg = col; term_write(s); cur_fg = o; }
static void term_backspace(void) {
    cursor_hide();
    if (cur_c > 0) cur_c--; else if (cur_r > 0) { cur_r--; cur_c = term_cols - 1; }
    cells[cur_r][cur_c] = ' '; cinv[cur_r][cur_c] = 0;
    serial_putc('\b'); serial_putc(' '); serial_putc('\b');
    draw_cell(cur_r, cur_c, 0);
    cursor_show();
}
void term_clear(void) {
    cursor_vis = 0; term_clear_cells(); cur_r = cur_c = 0;
    term_redraw_all(); cursor_show();
}
void print_u(u64 v) { char b[24]; term_write(u64_to_dec(v, b)); }
static void print_hex(u64 v) { char b[24]; term_write(u64_to_hex(v, b)); }

/* alternate screen for the editor */
static void *saved_screen;
int term_save(void) {
    usize sz = sizeof cells + sizeof cfg + sizeof cinv + 4 * sizeof(int);
    saved_screen = kmalloc(sz);
    if (!saved_screen) return 0;
    u8 *p = saved_screen;
    memcpy(p, cells, sizeof cells); p += sizeof cells;
    memcpy(p, cfg, sizeof cfg);     p += sizeof cfg;
    memcpy(p, cinv, sizeof cinv);   p += sizeof cinv;
    memcpy(p, &cur_r, sizeof(int)); memcpy(p + 4, &cur_c, sizeof(int)); memcpy(p + 8, &cur_fg, sizeof(int));
    return 1;
}
void term_restore(void) {
    if (!saved_screen) return;
    u8 *p = saved_screen;
    memcpy(cells, p, sizeof cells); p += sizeof cells;
    memcpy(cfg, p, sizeof cfg);     p += sizeof cfg;
    memcpy(cinv, p, sizeof cinv);   p += sizeof cinv;
    memcpy(&cur_r, p, sizeof(int)); memcpy(&cur_c, p + 4, sizeof(int)); memcpy(&cur_fg, p + 8, sizeof(int));
    kfree(saved_screen); saved_screen = NULL;
    cursor_vis = 0; dirty_all = 0;
    term_redraw_all(); cursor_show();
}

/* ------------------------------------------------------------ desktop / layout */
static int win_x, win_y, win_w, win_h;

static void draw_desktop(void) {
    for (u32 y = 0; y < fbh; y++) {
        u32 r = 14 + (y * 30) / fbh, g = 18 + (y * 14) / fbh, b = 44 + (y * 50) / fbh;
        fill(0, (int)y, (int)fbw, 1, (r << 16) | (g << 8) | b);
    }
    fill(0, 0, (int)fbw, 24, COL_PANEL);
    fill(0, 24, (int)fbw, 1, COL_BORDER);
    fill(10, 6, 12, 12, 0x7c5cff);
    fill(13, 9, 6, 6, 0xffffff);
    text(30, 4, OS_NAME, 0xffffff, COL_PANEL);
    draw_clock();
}
static void draw_window(void) {
    fill(win_x + 7, win_y + 7, win_w, win_h, 0x07070f);                    /* shadow */
    fill(win_x - 1, win_y - 1, win_w + 2, win_h + 2, COL_BORDER);          /* border */
    fill(win_x, win_y, win_w, 28, COL_TITLE);                              /* title bar */
    fill(win_x + 12, win_y + 8, 12, 12, 0xff5f56);
    fill(win_x + 32, win_y + 8, 12, 12, 0xffbd2e);
    fill(win_x + 52, win_y + 8, 12, 12, 0x27c93f);
    const char *t = "Terminal - picsonut@os";
    text(win_x + (win_w - (int)strlen(t) * 8) / 2, win_y + 6, t, 0xc9d1d9, COL_TITLE);
    fill(win_x, win_y + 28, win_w, win_h - 28, COL_WIN_BG);                /* body */
}
/* The terminal window fills the screen at any resolution (640x480 ... 1920x1080 and beyond). */
static void layout(void) {
    int cw = 8 * fscale, ch = 16 * fscale;
    int mx = fbw < 800 ? 10 : 30, my = fbh < 600 ? 12 : 35;
    win_w = (int)fbw - 2 * mx; if (win_w > MAXC * cw + 16) win_w = MAXC * cw + 16;
    win_h = (int)fbh - 24 - 2 * my; if (win_h > MAXR * ch + 28 + 16) win_h = MAXR * ch + 28 + 16;
    win_x = ((int)fbw - win_w) / 2;
    win_y = 24 + ((int)fbh - 24 - win_h) / 2;
    term_cols = (win_w - 16) / cw;  if (term_cols > MAXC) term_cols = MAXC;
    term_rows = (win_h - 28 - 12) / ch; if (term_rows > MAXR) term_rows = MAXR;
    term_px = win_x + 8; term_py = win_y + 28 + 6;
    if (cur_r >= term_rows) cur_r = term_rows - 1;
    if (cur_c >= term_cols) cur_c = term_cols - 1;
}
static void redraw_everything(void) {
    cursor_vis = 0;
    layout(); draw_desktop(); draw_window(); term_redraw_all(); cursor_show();
}

/* ------------------------------------------------------------ keyboard */
static const char kmap[58] = {0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static const char smap[58] = {0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};
static int shift, caps, ext, ctrl;

/* returns 0 if nothing to report, else a char or one of the KEY_* codes */
int key_poll(void) {
    u8 st = inb(0x64);
    if (!(st & 1)) return 0;
    u8 sc = inb(0x60);
    if (st & 0x20) return 0;                 /* mouse byte, ignore */
    if (sc == 0xE0) { ext = 1; return 0; }
    if (ext) {
        ext = 0;
        int rel = sc & 0x80; u8 code = sc & 0x7F;
        if (code == 0x1D) { ctrl = !rel; return 0; }
        if (rel) return 0;
        switch (code) {
        case 0x48: return KEY_UP;    case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;  case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;  case 0x4F: return KEY_END;
        case 0x53: return KEY_DEL;   case 0x49: return KEY_PGUP;  case 0x51: return KEY_PGDN;
        case 0x1C: return '\n';      case 0x35: return '/';
        }
        return 0;
    }
    if (sc == 0x1D) { ctrl = 1; return 0; }
    if (sc == 0x9D) { ctrl = 0; return 0; }
    if (sc == 0x2A || sc == 0x36) { shift = 1; return 0; }
    if (sc == 0xAA || sc == 0xB6) { shift = 0; return 0; }
    if (sc == 0x3A) { caps = !caps; return 0; }
    if (sc & 0x80 || sc >= 58) return 0;
    char c = shift ? smap[sc] : kmap[sc];
    if (caps) {
        if (!shift && c >= 'a' && c <= 'z') c -= 32;
        else if (shift && c >= 'A' && c <= 'Z') c += 32;
    }
    if (ctrl) {
        if (c >= 'a' && c <= 'z') return KEY_CTRL | c;
        if (c >= 'A' && c <= 'Z') return KEY_CTRL | (c + 32);
    }
    return (u8)c;
}

/* ------------------------------------------------------------ idle / time */
static u32 uptime_s;
void idle_tick(void) {
    static u32 idle; static int last_sec = -1; static u64 last_chk;
    int check;
    if (timer_on) {
        sti(); hlt();                                           /* wake on the 100 Hz timer */
        check = timer_ticks - last_chk >= 25; if (check) last_chk = timer_ticks;
    } else { pause(); check = (++idle & 0xFFFF) == 0; }
    if (check) {
        rtc_read();
        if (t_s != last_sec) { last_sec = t_s; uptime_s++; draw_clock(); }
    }
}
int key_wait(void) {
    for (;;) {
        if (inb(0x64) & 1) { int k = key_poll(); if (k) return k; continue; }   /* drain pending bytes first */
        idle_tick();
    }
}

/* ------------------------------------------------------------ panic screen */
void panic_screen(const char *title, const char *detail, u64 rip, u64 err, u64 cr2) {
    cli();
    char h[24];
    serial_write("\n*** "); serial_write(title); serial_write(": "); serial_write(detail);
    serial_write(" rip="); serial_write(u64_to_hex(rip, h));
    serial_write(" err="); serial_write(u64_to_hex(err, h));
    serial_write(" cr2="); serial_write(u64_to_hex(cr2, h)); serial_write("\n");
    if (have_fb) {
        int x = 40, y = 60;
        fill(x - 10, y - 10, 620, 130, 0x7a1010);
        text(x, y, title, 0xffffff, 0x7a1010);
        text(x, y + 20, detail, 0xffffff, 0x7a1010);
        text(x, y + 44, "RIP", 0xffd0d0, 0x7a1010);  text(x + 40, y + 44, u64_to_hex(rip, h), 0xffffff, 0x7a1010);
        text(x, y + 62, "ERR", 0xffd0d0, 0x7a1010);  text(x + 40, y + 62, u64_to_hex(err, h), 0xffffff, 0x7a1010);
        text(x, y + 80, "CR2", 0xffd0d0, 0x7a1010);  text(x + 40, y + 80, u64_to_hex(cr2, h), 0xffffff, 0x7a1010);
        text(x, y + 100, "System halted. Power-cycle or reset the machine.", 0xffd0d0, 0x7a1010);
    }
    for (;;) hlt();
}

/* ------------------------------------------------------------ video modes */
typedef struct { u16 w, h; } vmode;
static const vmode vmodes[] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,800}, {1920,1080} };
#define NVMODES ((int)(sizeof vmodes / sizeof vmodes[0]))

/* Bochs/QEMU/VirtualBox "VBE DISPI" interface: lets the guest change resolution at run time. */
static void bga_w(u16 i, u16 v) { outw(0x1CE, i); outw(0x1CF, v); }
static u16  bga_r(u16 i) { outw(0x1CE, i); return inw(0x1CF); }
static int bga_present(void) { u16 id = bga_r(0); return id >= 0xB0C0 && id <= 0xB0CF; }
static int bga_set(u32 w, u32 h) {
    u16 ow = bga_r(1), oh = bga_r(2);
    u16 id = bga_r(0);
    if (id >= 0xB0C4) {                                              /* video memory size known? */
        u64 vram = (u64)bga_r(0xA) * 65536;
        if (vram && (u64)w * h * 4 > vram) return -1;
    }
    bga_w(4, 0);
    bga_w(1, (u16)w); bga_w(2, (u16)h); bga_w(3, 32);
    bga_w(4, 0x41);
    if (bga_r(1) != w || bga_r(2) != h) {                            /* refused: put the old mode back */
        bga_w(4, 0); bga_w(1, ow); bga_w(2, oh); bga_w(3, 32); bga_w(4, 0x41);
        return -1;
    }
    u32 vw = bga_r(6); if (vw < w) vw = w;
    fbw = w; fbh = h; pitch = vw * 4;
    return 0;
}

/* ------------------------------------------------------------ commands */
static void cpu_info(void) {
    u32 a, b, c, d; char v[13];
    cpuid(0, &a, &b, &c, &d);
    *(u32 *)&v[0] = b; *(u32 *)&v[4] = d; *(u32 *)&v[8] = c; v[12] = 0;
    char brand[49]; brand[0] = 0;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        for (u32 i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, &a, &b, &c, &d);
            *(u32 *)&brand[i * 16] = a; *(u32 *)&brand[i * 16 + 4] = b;
            *(u32 *)&brand[i * 16 + 8] = c; *(u32 *)&brand[i * 16 + 12] = d;
        }
        brand[48] = 0;
    }
    const char *s = brand; while (*s == ' ') s++;
    term_write("Vendor : "); term_write(v); term_write("\n");
    term_write("Model  : "); term_write(*s ? s : "unknown"); term_write("\n");
    term_write("Cores  : "); print_u((u64)cpu_count); term_write(" online");
    if (cpu_count > 1) { term_write(" (APIC ids"); for (int i = 0; i < cpu_count; i++) { term_write(" "); print_u(cpu_apic_id[i]); } term_write(")"); }
    term_write("\n");
    if (*smp_note && cpu_count == 1) { term_write("SMP    : off - "); term_write(smp_note); term_write("\n"); }
    if (tsc_khz()) { term_write("TSC    : "); print_u(tsc_khz() / 1000); term_write(" MHz\n"); }
}
static void do_reboot(void) {
    term_write("Rebooting...\n");
    for (int i = 0; i < 100000; i++) { if (!(inb(0x64) & 2)) break; }
    outb(0x64, 0xFE);
    struct { u16 l; u64 b; } __attribute__((packed)) idt = {0, 0};
    __asm__ volatile("lidt %0; int3" :: "m"(idt));
}
static void do_shutdown(void) {
    term_write("Shutting down...\n");
    outw(0x604, 0x2000); outw(0xB004, 0x2000); outw(0x4004, 0x3400);
    term_write_col("It is now safe to turn off your computer.\n", 0xffbd2e);
    for (;;) __asm__ volatile("cli; hlt");
}
static void cmd_info(void) {
    static const char *logo[] = {
        "  ######    ", "  ##   ##   ", "  ##   ##   ", "  ######    ", "  ##        ", "  ##        ", "  ##        "
    };
    for (int i = 0; i < 8; i++) {
        term_write_col(i < 7 ? logo[i] : "            ", 0x7c5cff);
        switch (i) {
        case 0: term_write_col(OS_NAME " " OS_VER, 0xffffff); break;
        case 1: term_write("-----------------"); break;
        case 2: term_write("Arch   : x86_64 (64-bit)"); break;
        case 3: term_write("Boot   : BIOS + UEFI"); break;
        case 4: term_write("Video  : "); print_u(fbw); term_write("x"); print_u(fbh); term_write("x32"); break;
        case 5: term_write("Memory : "); print_u(mem_usable_bytes >> 20); term_write(" MiB"); break;
        case 6: term_write("CPUs   : "); print_u((u64)cpu_count); term_write(cpu_count > 1 ? " cores online" : " core"); break;
        case 7: term_write("Shell  : psh   Files: RAM disk"); break;
        }
        term_write("\n");
    }
}
static void cmd_mem(void) {
    u64 ht, hu;
    heap_stats(&ht, &hu);
    term_write("Usable RAM : "); print_u(mem_usable_bytes >> 20); term_write(" MiB\n");
    term_write("Highest    : "); print_hex(mem_top_usable); term_write(mem_top_usable > (4ULL << 30) ? "  (RAM above 4 GiB in use)\n" : "\n");
    term_write("Mapped     : "); print_u(mem_mapped_bytes >> 20); term_write(" MiB, ");
    term_write(mem_use_1g ? "1 GiB pages\n" : "2 MiB pages\n");
    term_write("Kernel heap: "); print_u(hu >> 10); term_write(" KiB used of "); print_u(ht >> 10); term_write(" KiB\n");
    term_write("RAM disk   : "); print_u(fs_total_bytes()); term_write(" bytes in files\n");
}
static void cmd_memtest(void) {
    if (!mem_testwin) { term_write("memtest: no spare RAM window\n"); return; }
    volatile u64 *p = (volatile u64 *)(usize)mem_testwin;
    usize n = (1u << 20) / 8; u64 bad = 0; usize first = 0;
    term_write("Testing 1 MiB at "); print_hex(mem_testwin); term_write(mem_testwin >= (4ULL << 30) ? " (above 4 GiB) ... " : " ... ");
    for (int pass = 0; pass < 2; pass++) {
        for (usize i = 0; i < n; i++) { u64 v = (i * 0x9E3779B97F4A7C15ULL) ^ mem_testwin; p[i] = pass ? ~v : v; }
        for (usize i = 0; i < n; i++) {
            u64 v = (i * 0x9E3779B97F4A7C15ULL) ^ mem_testwin; if (pass) v = ~v;
            if (p[i] != v) { if (!bad) first = i; bad++; }
        }
    }
    if (!bad) term_write_col("PASS\n", COL_PROMPT);
    else { term_write_col("FAIL", COL_ERR); term_write(" - bad words: "); print_u(bad); term_write(", first at "); print_hex(mem_testwin + first * 8); term_write("\n"); }
}

typedef struct { u64 partial[64]; u64 cycles[64]; } smp_res;
#define SMP_WORK 6000000u
static void smp_job(int cpu, void *arg) {
    smp_res *r = arg;
    u32 n = SMP_WORK / (u32)cpu_count;
    u64 t0 = rdtsc(), x = 0x12345678ULL + (u64)cpu;
    for (u32 i = 0; i < n; i++) x = x * 6364136223846793005ULL + 1442695040888963407ULL;
    r->partial[cpu] = x; r->cycles[cpu] = rdtsc() - t0;
}
static void cmd_cores(void) {
    static smp_res r;
    term_write("Running a parallel test on "); print_u((u64)cpu_count); term_write(" core(s)...\n");
    u64 t0 = rdtsc();
    smp_run_all(smp_job, &r);
    u64 total = rdtsc() - t0;
    u64 khz = tsc_khz();
    for (int i = 0; i < cpu_count; i++) {
        term_write("  cpu "); print_u((u64)i); term_write("  apic "); print_u(cpu_apic_id[i]);
        term_write("  result "); print_hex(r.partial[i]);
        if (khz) { term_write("  "); print_u(r.cycles[i] / khz); term_write(" ms"); }
        term_write("\n");
    }
    if (khz) { term_write("Wall time: "); print_u(total / khz); term_write(" ms\n"); }
    if (cpu_count == 1 && *smp_note) { term_write("(single core: "); term_write(smp_note); term_write(")\n"); }
}
static void cmd_res(const char *a) {
    int cur = -1;
    for (int i = 0; i < NVMODES; i++) if (vmodes[i].w == fbw && vmodes[i].h == fbh) cur = i;
    if (!*a) {
        term_write("Resolutions (current "); print_u(fbw); term_write("x"); print_u(fbh); term_write("):\n");
        for (int i = 0; i < NVMODES; i++) {
            term_write(i == cur ? "  * " : "    "); print_u((u64)i + 1); term_write(") ");
            print_u(vmodes[i].w); term_write("x"); print_u(vmodes[i].h); term_write("\n");
        }
        term_write(bga_present() ? "Use: res <number> or res <W>x<H>\n"
                                 : "Run-time switching is not available on this display;\nrestart and choose the resolution in the boot menu.\n");
        return;
    }
    if (!bga_present()) { term_write_col("res: run-time switching unavailable here; use the boot menu\n", COL_ERR); return; }
    u32 w = 0, h = 0; int ok;
    char buf[24]; strlcpy(buf, a, sizeof buf);
    char *x = buf; while (*x && *x != 'x') x++;
    if (*x == 'x') { *x++ = 0; w = (u32)parse_u(buf, &ok); if (!ok) w = 0; h = (u32)parse_u(x, &ok); if (!ok) h = 0; }
    else { u64 n = parse_u(buf, &ok); if (ok && n >= 1 && n <= NVMODES) { w = vmodes[n - 1].w; h = vmodes[n - 1].h; } }
    int known = 0; for (int i = 0; i < NVMODES; i++) if (vmodes[i].w == w && vmodes[i].h == h) known = 1;
    if (!known) { term_write_col("res: unknown mode (type 'res' for the list)\n", COL_ERR); return; }
    if (bga_set(w, h) < 0) { term_write_col("res: the display refused that mode\n", COL_ERR); return; }
    redraw_everything();
    term_write("Resolution set to "); print_u(w); term_write("x"); print_u(h); term_write("\n");
}
static void cmd_font(const char *a) {
    if (!strcmp(a, "1") || !strcmp(a, "2")) {
        fscale = a[0] - '0';
        redraw_everything();
    } else { term_write("Font scale is "); print_u((u64)fscale); term_write("  (usage: font 1|2)\n"); }
}

static void cmd_help(void) {
    term_write("Files:     ls [-la] cd pwd mkdir [-p] rmdir touch rm [-rf] cp [-r] mv cat\n"
               "Editor:    vim|vi|edit FILE   (i = insert, Esc, :wq = save+quit, :help)\n"
               "Redirect:  echo text > file     echo more >> file     ls > file\n"
               "System:    info about cpu cores mem memtest date uptime\n"
               "Display:   res [N|WxH]  font 1|2  color NAME  clear\n"
               "Power:     reboot shutdown\n"
               "Keys:      Up/Down = history\n");
}

#define LINE 200
#define MAXARG 16
static char tbuf[LINE * 2 + 16];
static const char *rd_token(const char *p, char **w) {
    while (*p && *p != ' ' && *p != '>') {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q) *(*w)++ = *p++;
            if (*p) p++;
        } else *(*w)++ = *p++;
    }
    *(*w)++ = 0;
    return p;
}
static int redirectable(const char *c) {
    return !strcmp(c, "echo") || !strcmp(c, "cat") || !strcmp(c, "ls") || !strcmp(c, "ll") || !strcmp(c, "dir") || !strcmp(c, "pwd");
}

static void run(char *line) {
    char *argv[MAXARG]; int argc = 0;
    char *w = tbuf; const char *p = line;
    char *rpath = NULL; int rmode = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '>') {
            rmode = (p[1] == '>') ? 2 : 1; p += rmode;
            while (*p == ' ') p++;
            rpath = w; p = rd_token(p, &w);
            if (!*rpath) { term_write_col("psh: syntax error: expected a file name after '>'\n", COL_ERR); return; }
            continue;
        }
        if (argc >= MAXARG - 1) { term_write_col("psh: too many arguments\n", COL_ERR); return; }
        argv[argc++] = w;
        p = rd_token(p, &w);
    }
    if (!argc) return;
    argv[argc] = NULL;
    const char *line0 = argv[0];

    fnode *redir = NULL;
    if (rmode) {
        if (!redirectable(line0)) { term_write_col("psh: output redirection is not supported for this command\n", COL_ERR); return; }
        fnode *n = fs_find(rpath);
        if (!n) {
            fnode *par; char leaf[FS_NAME_MAX + 1];
            if (fs_split(rpath, &par, leaf) || !(n = fs_create(par, leaf, 0))) { cmd_err("psh", "cannot create file", rpath); return; }
        } else if (n->is_dir) { cmd_err("psh", "Is a directory", rpath); return; }
        else if (rmode == 1) fs_write(n, "", 0);
        redir = n;
    }
    if (cmd_fs_dispatch(argc, argv, redir)) return;

    const char *args = argc > 1 ? argv[1] : "";
    if (!strcmp(line0, "help")) cmd_help();
    else if (!strcmp(line0, "info") || !strcmp(line0, "neofetch")) cmd_info();
    else if (!strcmp(line0, "about") || !strcmp(line0, "ver")) {
        term_write(OS_NAME " " OS_VER " - a tiny 64-bit operating system.\n"
                   "Written from scratch in C and assembly. Boots on BIOS and UEFI.\n");
    }
    else if (!strcmp(line0, "clear") || !strcmp(line0, "cls")) term_clear();
    else if (!strcmp(line0, "date")) {
        char d[16], t[16]; rtc_read(); fmt_date(d); fmt_time(t);
        term_write(d); term_write(" "); term_write(t); term_write(" (hardware clock)\n");
    } else if (!strcmp(line0, "uptime")) {
        u32 s = timer_on ? (u32)(timer_ticks / 100) : uptime_s;
        print_u(s / 3600); term_write("h "); print_u((s / 60) % 60); term_write("m ");
        print_u(s % 60); term_write("s\n");
    } else if (!strcmp(line0, "mem") || !strcmp(line0, "free")) cmd_mem();
    else if (!strcmp(line0, "memtest")) cmd_memtest();
    else if (!strcmp(line0, "cpu")) cpu_info();
    else if (!strcmp(line0, "cores") || !strcmp(line0, "smp")) cmd_cores();
    else if (!strcmp(line0, "res") || !strcmp(line0, "resolution")) cmd_res(args);
    else if (!strcmp(line0, "font")) cmd_font(args);
    else if (!strcmp(line0, "color")) {
        u32 c = 0;
        if (!strcmp(args, "green")) c = 0x7ee787; else if (!strcmp(args, "white")) c = COL_FG;
        else if (!strcmp(args, "amber")) c = 0xffb454; else if (!strcmp(args, "cyan")) c = 0x56d4dd;
        else if (!strcmp(args, "pink")) c = 0xff7eb6; else if (!strcmp(args, "blue")) c = 0x79c0ff;
        if (c) cur_fg = c; else term_write("usage: color green|white|amber|cyan|pink|blue\n");
    } else if (!strcmp(line0, "crashtest")) __asm__ volatile("ud2");   /* hidden: proves the exception handler works */
    else if (!strcmp(line0, "reboot")) do_reboot();
    else if (!strcmp(line0, "shutdown") || !strcmp(line0, "halt") || !strcmp(line0, "poweroff")) do_shutdown();
    else { term_write("psh: command not found: "); term_write(line0); term_write("  (try 'help')\n"); }
}

static void prompt(void) {
    char p[FS_PATH_MAX], q[FS_PATH_MAX];
    fs_path(fs_cwd, p, sizeof p);
    if (!strncmp(p, "/home/user", 10) && (p[10] == 0 || p[10] == '/')) { q[0] = '~'; strlcpy(q + 1, p + 10, sizeof q - 1); }
    else strlcpy(q, p, sizeof q);
    term_write_col("picsonut@os", COL_PROMPT);
    term_write_col(":", COL_FG);
    term_write_col(q, COL_DIR);
    term_write_col("$ ", COL_FG);
}

/* ------------------------------------------------------------ entry */
#define HIST 8
static char hist[HIST][LINE];
static int hist_n, hist_pos;

void kmain(u64 mbi) {
    parse_mbi(mbi);
    serial_init();
    if (!have_fb) vga_fail("Picsonut OS: no 32-bit framebuffer from bootloader.");

    mem_init(mmap_tag, fb_phys, FB_WINDOW);          /* also switches to the kernel's own page tables */
    fs_init();
    if (fbw >= 1600 && fbh >= 900) fscale = 2;        /* big screens get the large font */

    layout();
    term_clear_cells();
    draw_desktop();
    draw_window();
    term_redraw_all();
    rtc_read(); draw_clock();

    cpu_init(rsdp_ptr, have_rsdp_copy ? rsdp_copy : NULL);

    term_write_col("Welcome to " OS_NAME " " OS_VER "\n", 0xffffff);
    print_u(mem_usable_bytes >> 20); term_write(" MiB RAM, ");
    print_u((u64)cpu_count); term_write(cpu_count > 1 ? " CPU cores, " : " CPU core, ");
    print_u(fbw); term_write("x"); print_u(fbh); term_write(" display.\n");
    term_write("Type 'help' to see the available commands.\n\n");
    prompt();

    char line[LINE]; int len = 0;
    for (;;) {
        int k = key_wait();
        if (k == '\n') {
            term_write("\n");
            line[len] = 0;
            if (len) {
                for (int i = 0; i < LINE; i++) hist[hist_n % HIST][i] = line[i];
                hist_n++;
            }
            hist_pos = hist_n;
            run(line);
            len = 0;
            prompt();
        } else if (k == '\b') {
            if (len > 0) { len--; term_backspace(); }
        } else if (k == KEY_UP || k == KEY_DOWN) {
            int np = hist_pos + (k == KEY_UP ? -1 : 1);
            if (np < 0 || np < hist_n - HIST || np > hist_n) continue;
            while (len > 0) { len--; term_backspace(); }
            hist_pos = np;
            if (np < hist_n) {
                const char *h = hist[np % HIST];
                while (*h && len < LINE - 1) { line[len++] = *h; char s[2] = {*h, 0}; term_write(s); h++; }
            }
        } else if (k >= 32 && k < 127 && len < LINE - 1) {
            line[len++] = (char)k;
            char s[2] = {(char)k, 0};
            term_write(s);
        }
    }
}
