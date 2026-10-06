/* Picsonut OS - a tiny 64-bit hobby OS: framebuffer desktop + terminal.
 * Booted by the Picsonut loaders (BIOS + UEFI) or GRUB2 via Multiboot2. */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
typedef unsigned long  usize;

#include "font.h"

#define OS_NAME "Picsonut OS"
#define OS_VER  "1.0"

/* ------------------------------------------------------------ low level */
/* LOWLEVEL_BEGIN */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8   inb(u16 p)         { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static inline void pause(void) { __asm__ volatile("pause"); }

void *memcpy(void *d, const void *s, usize n) {
    void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); return r;
}
void *memset(void *d, int c, usize n) {
    void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r;
}
void *memmove(void *d, const void *s, usize n) {
    if ((usize)d <= (usize)s || (usize)d >= (usize)s + n) return memcpy(d, s, n);
    u8 *dd = (u8 *)d + n - 1; const u8 *ss = (const u8 *)s + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n) :: "memory");
    return d;
}
/* LOWLEVEL_END */
static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ------------------------------------------------------------ boot info */
static u8 *fb; static u32 fbw, fbh, pitch;
static u8 rpos = 16, gpos = 8, bpos = 0;
static u8 *mmap_tag;
static int have_fb;

static void parse_mbi(u64 addr) {
    u8 *p = (u8 *)addr;
    u32 total = *(u32 *)p;
    u8 *end = p + total;
    p += 8;
    while (p < end) {
        u32 type = *(u32 *)p, size = *(u32 *)(p + 4);
        if (type == 0) break;
        if (type == 6) mmap_tag = p;
        if (type == 8) {
            u64 a = *(u64 *)(p + 8);
            u32 bpp = p[28], ftype = p[29];
            if (ftype == 1 && bpp == 32 && a < 0x100000000ULL) {
                fb = (u8 *)(usize)a;
                pitch = *(u32 *)(p + 16);
                fbw = *(u32 *)(p + 20);
                fbh = *(u32 *)(p + 24);
                rpos = p[32]; gpos = p[34]; bpos = p[36];
                have_fb = 1;
            }
        }
        p += (size + 7) & ~7u;
    }
}

static void vga_fail(const char *msg) {
    volatile u16 *v = (volatile u16 *)0xB8000;
    for (int i = 0; i < 80 * 25; i++) v[i] = 0x0720;
    for (int i = 0; msg[i]; i++) v[i] = 0x0C00 | (u8)msg[i];
    for (;;) __asm__ volatile("cli; hlt");
}

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
static void glyph(int x, int y, char ch, u32 fg, u32 bg) {
    if (ch < 32 || ch > 126) ch = '?';
    const u8 *g = font8x16[ch - 32];
    fg = conv(fg); bg = conv(bg);
    for (int r = 0; r < 16; r++) {
        u32 *p = (u32 *)(fb + (usize)(y + r) * pitch) + x;
        u8 b = g[r];
        for (int c = 0; c < 8; c++) p[c] = (b & (0x80 >> c)) ? fg : bg;
    }
}
static void text(int x, int y, const char *s, u32 fg, u32 bg) {
    for (; *s; s++, x += 8) glyph(x, y, *s, fg, bg);
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }

/* ------------------------------------------------------------ colors */
#define COL_PANEL   0x11111b
#define COL_WIN_BG  0x0d1117
#define COL_TITLE   0x2a2d45
#define COL_BORDER  0x3a3f5c
#define COL_PROMPT  0x7ee787

/* ------------------------------------------------------------ clock */
static int t_h, t_m, t_s, t_d, t_mo, t_y;
static u8 cmos(u8 r) { outb(0x70, r); return inb(0x71); }
static int bcd(int v) { return (v & 15) + (v >> 4) * 10; }
static void rtc_read(void) {
    while (cmos(0x0A) & 0x80) pause();
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
    int x = (int)fbw - slen(buf) * 8 - 14;
    text(x, 4, buf, 0xc9d1d9, COL_PANEL);
}

/* ------------------------------------------------------------ terminal */
#define MAXC 128
#define MAXR 64
static char cells[MAXR][MAXC];
static u32  cfg[MAXR][MAXC];
static int term_cols, term_rows, term_px, term_py;
static int cur_r, cur_c, cursor_vis, dirty_all;
static u32 cur_fg = 0xd7dae0;

static void draw_cell(int r, int c, int inv) {
    int x = term_px + c * 8, y = term_py + r * 16;
    if (inv) glyph(x, y, cells[r][c], COL_WIN_BG, cfg[r][c]);
    else     glyph(x, y, cells[r][c], cfg[r][c], COL_WIN_BG);
}
static void cursor_hide(void) { if (cursor_vis) { draw_cell(cur_r, cur_c, 0); cursor_vis = 0; } }
static void cursor_show(void) { draw_cell(cur_r, cur_c, 1); cursor_vis = 1; }
static void term_redraw_all(void) {
    for (int r = 0; r < term_rows; r++)
        for (int c = 0; c < term_cols; c++) draw_cell(r, c, 0);
}
static void term_clear_cells(void) {
    for (int r = 0; r < MAXR; r++) for (int c = 0; c < MAXC; c++) { cells[r][c] = ' '; cfg[r][c] = cur_fg; }
}
static void term_newline(void) {
    cur_c = 0; cur_r++;
    if (cur_r >= term_rows) {
        memmove(cells[0], cells[1], (usize)(MAXR - 1) * MAXC);
        memmove(cfg[0], cfg[1], (usize)(MAXR - 1) * MAXC * sizeof(u32));
        for (int c = 0; c < MAXC; c++) { cells[term_rows - 1][c] = ' '; cfg[term_rows - 1][c] = cur_fg; }
        cur_r = term_rows - 1;
        dirty_all = 1;
    }
}
static void term_putc(char ch) {
    if (ch == '\n') { term_newline(); return; }
    if (ch == '\r') { cur_c = 0; return; }
    if (ch == '\t') { do term_putc(' '); while (cur_c % 4); return; }
    cells[cur_r][cur_c] = ch; cfg[cur_r][cur_c] = cur_fg;
    if (!dirty_all) draw_cell(cur_r, cur_c, 0);
    if (++cur_c >= term_cols) term_newline();
}
static void term_write(const char *s) {
    cursor_hide();
    while (*s) term_putc(*s++);
    if (dirty_all) { term_redraw_all(); dirty_all = 0; }
    cursor_show();
}
static void term_write_col(const char *s, u32 col) { u32 o = cur_fg; cur_fg = col; term_write(s); cur_fg = o; }
static void term_backspace(void) {
    cursor_hide();
    if (cur_c > 0) cur_c--; else if (cur_r > 0) { cur_r--; cur_c = term_cols - 1; }
    cells[cur_r][cur_c] = ' ';
    draw_cell(cur_r, cur_c, 0);
    cursor_show();
}
static void term_clear(void) {
    cursor_vis = 0; term_clear_cells(); cur_r = cur_c = 0;
    term_redraw_all(); cursor_show();
}
static void print_u(u64 v) {
    char b[24]; int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = '0' + v % 10; v /= 10; }
    term_write(&b[i]);
}

/* ------------------------------------------------------------ desktop */
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
    text(win_x + (win_w - slen(t) * 8) / 2, win_y + 6, t, 0xc9d1d9, COL_TITLE);
    fill(win_x, win_y + 28, win_w, win_h - 28, COL_WIN_BG);                /* body */
}
static void layout(void) {
    win_w = (int)fbw - 60; if (win_w > MAXC * 8 + 16) win_w = MAXC * 8 + 16;
    win_h = (int)fbh - 24 - 70; if (win_h > MAXR * 16 + 28 + 16) win_h = MAXR * 16 + 28 + 16;
    win_x = ((int)fbw - win_w) / 2;
    win_y = 24 + ((int)fbh - 24 - win_h) / 2;
    term_cols = (win_w - 16) / 8;  if (term_cols > MAXC) term_cols = MAXC;
    term_rows = (win_h - 28 - 12) / 16; if (term_rows > MAXR) term_rows = MAXR;
    term_px = win_x + 8; term_py = win_y + 28 + 6;
}

/* ------------------------------------------------------------ keyboard */
static const char kmap[58] = {0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static const char smap[58] = {0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,'A','S','D','F','G','H','J','K','L',':','"','~',
    0,'|','Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '};
static int shift, caps, ext;
#define KEY_UP   1
#define KEY_DOWN 2

/* returns 0 if nothing to report, else a char (or KEY_UP / KEY_DOWN) */
static int kbd_poll(void) {
    u8 st = inb(0x64);
    if (!(st & 1)) return 0;
    u8 sc = inb(0x60);
    if (st & 0x20) return 0;                 /* mouse byte, ignore */
    if (sc == 0xE0) { ext = 1; return 0; }
    if (ext) {
        ext = 0;
        if (sc == 0x48) return KEY_UP;
        if (sc == 0x50) return KEY_DOWN;
        return 0;
    }
    if (sc == 0x2A || sc == 0x36) { shift = 1; return 0; }
    if (sc == 0xAA || sc == 0xB6) { shift = 0; return 0; }
    if (sc == 0x3A) { caps = !caps; return 0; }
    if (sc & 0x80 || sc >= 58) return 0;
    char c = shift ? smap[sc] : kmap[sc];
    if (caps) {
        if (!shift && c >= 'a' && c <= 'z') c -= 32;
        else if (shift && c >= 'A' && c <= 'Z') c += 32;
    }
    return (u8)c;
}

/* ------------------------------------------------------------ commands */
static u32 uptime_s;

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
}
static u64 total_ram_kb(void) {
    if (!mmap_tag) return 0;
    u32 size = *(u32 *)(mmap_tag + 4), esz = *(u32 *)(mmap_tag + 8);
    u64 sum = 0;
    for (u32 off = 16; off + esz <= size; off += esz) {
        u8 *e = mmap_tag + off;
        if (*(u32 *)(e + 16) == 1) sum += *(u64 *)(e + 8);
    }
    return sum / 1024;
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
    u64 kb = total_ram_kb();
    for (int i = 0; i < 7; i++) {
        term_write_col(logo[i], 0x7c5cff);
        switch (i) {
        case 0: term_write_col(OS_NAME " " OS_VER, 0xffffff); break;
        case 1: term_write("-----------------"); break;
        case 2: term_write("Arch   : x86_64 (64-bit)"); break;
        case 3: term_write("Boot   : BIOS + UEFI"); break;
        case 4: term_write("Video  : "); print_u(fbw); term_write("x"); print_u(fbh); term_write("x32"); break;
        case 5: term_write("Memory : "); print_u(kb / 1024); term_write(" MiB"); break;
        case 6: term_write("Shell  : psh"); break;
        }
        term_write("\n");
    }
}
static void run(char *line) {
    while (*line == ' ') line++;
    if (!*line) return;
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) { *args++ = 0; while (*args == ' ') args++; }

    if (streq(line, "help")) {
        term_write("Commands:\n"
                   "  help      show this list\n"
                   "  info      system summary\n"
                   "  about     about " OS_NAME "\n"
                   "  echo TXT  print text\n"
                   "  clear     clear the terminal\n"
                   "  date      show date and time (RTC)\n"
                   "  uptime    time since boot\n"
                   "  mem       installed memory\n"
                   "  cpu       processor info\n"
                   "  color C   text color: green white amber cyan pink blue\n"
                   "  reboot    restart the computer\n"
                   "  shutdown  power off\n");
    } else if (streq(line, "info") || streq(line, "neofetch")) cmd_info();
    else if (streq(line, "about") || streq(line, "ver")) {
        term_write(OS_NAME " " OS_VER " - a tiny 64-bit operating system.\n"
                   "Written from scratch in C and assembly. Boots on BIOS and UEFI.\n");
    } else if (streq(line, "echo")) { term_write(args); term_write("\n"); }
    else if (streq(line, "clear") || streq(line, "cls")) term_clear();
    else if (streq(line, "date")) {
        char d[16], t[16]; rtc_read(); fmt_date(d); fmt_time(t);
        term_write(d); term_write(" "); term_write(t); term_write(" (hardware clock)\n");
    } else if (streq(line, "uptime")) {
        print_u(uptime_s / 3600); term_write("h "); print_u((uptime_s / 60) % 60); term_write("m ");
        print_u(uptime_s % 60); term_write("s\n");
    } else if (streq(line, "mem")) {
        u64 kb = total_ram_kb();
        if (kb) { print_u(kb / 1024); term_write(" MiB usable RAM\n"); }
        else term_write("memory map unavailable\n");
    } else if (streq(line, "cpu")) cpu_info();
    else if (streq(line, "color")) {
        u32 c = 0;
        if (streq(args, "green")) c = 0x7ee787; else if (streq(args, "white")) c = 0xd7dae0;
        else if (streq(args, "amber")) c = 0xffb454; else if (streq(args, "cyan")) c = 0x56d4dd;
        else if (streq(args, "pink")) c = 0xff7eb6; else if (streq(args, "blue")) c = 0x79c0ff;
        if (c) cur_fg = c; else term_write("usage: color green|white|amber|cyan|pink|blue\n");
    } else if (streq(line, "reboot")) do_reboot();
    else if (streq(line, "shutdown") || streq(line, "halt") || streq(line, "poweroff")) do_shutdown();
    else { term_write("psh: command not found: "); term_write(line); term_write("  (try 'help')\n"); }
}

static void prompt(void) {
    term_write_col("picsonut@os", COL_PROMPT);
    term_write_col(":", 0xd7dae0);
    term_write_col("~", 0x79c0ff);
    term_write_col("$ ", 0xd7dae0);
}

/* ------------------------------------------------------------ entry */
#define HIST 8
#define LINE 200
static char hist[HIST][LINE];
static int hist_n, hist_pos;

void kmain(u64 mbi) {
    parse_mbi(mbi);
    if (!have_fb) vga_fail("Picsonut OS: no 32-bit framebuffer from bootloader.");

    layout();
    term_clear_cells();
    draw_desktop();
    draw_window();
    term_redraw_all();
    rtc_read(); draw_clock();
    int last_sec = t_s;

    term_write_col("Welcome to " OS_NAME " " OS_VER "\n", 0xffffff);
    term_write("Type 'help' to see the available commands.\n\n");
    prompt();

    char line[LINE]; int len = 0; u32 idle = 0;
    for (;;) {
        int k = kbd_poll();
        if (!k) {
            if ((++idle & 0xFFFF) == 0) {
                rtc_read();
                if (t_s != last_sec) { last_sec = t_s; uptime_s++; draw_clock(); }
            }
            pause();
            continue;
        }
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
