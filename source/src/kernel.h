/* Picsonut OS - shared kernel declarations. */
#ifndef KERNEL_H
#define KERNEL_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned long      usize;
typedef signed char        i8;
typedef int                i32;
typedef long long          i64;
#define NULL ((void *)0)

#define OS_NAME "Picsonut OS"
#define OS_VER  "1.1"

/* ------------------------------------------------------------ port / cpu helpers */
static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8   inb(u16 p)         { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16  inw(u16 p)         { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static inline void pause(void) { __asm__ volatile("pause"); }
static inline u64 rdtsc(void) { u32 lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((u64)hi << 32) | lo; }
static inline u64 rdmsr(u32 m) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((u64)hi << 32) | lo; }
static inline void wrmsr(u32 m, u64 v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((u32)v), "d"((u32)(v >> 32))); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }

/* ------------------------------------------------------------ lib.c */
void *memcpy(void *d, const void *s, usize n);
void *memset(void *d, int c, usize n);
void *memmove(void *d, const void *s, usize n);
int   memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, usize n);
void  strlcpy(char *d, const char *s, usize n);       /* always NUL-terminates */
u64   parse_u(const char *s, int *ok);                 /* decimal */
char *u64_to_dec(u64 v, char *buf24);                  /* returns pointer inside buf */
char *u64_to_hex(u64 v, char *buf19);                  /* "0x...." */

/* ------------------------------------------------------------ mem.c */
typedef struct { u64 base, len; } region_t;
#define MAX_REGIONS 64
extern region_t mem_free[MAX_REGIONS];     /* usable RAM >= 2 MiB, page aligned, sorted */
extern int      mem_nfree;
extern u64      mem_usable_bytes;          /* all E820/UEFI "usable" bytes (what `mem` reports) */
extern u64      mem_top_usable;            /* highest usable physical address (exclusive) */
extern u64      mem_mapped_bytes;          /* RAM covered by the kernel page tables */
extern int      mem_use_1g;                /* 1 GiB pages in use */
extern int      mem_pat_wc;                /* write-combining framebuffer mapping active */
extern u64      mem_testwin;               /* 1 MiB window for `memtest` (0 = none) */
void  mem_init(const u8 *mmap_tag, u64 fb_base, u64 fb_win);   /* parse map, build page tables, init heap */
void *kmalloc(usize n);
void *kzalloc(usize n);
void *krealloc(void *p, usize n);
void  kfree(void *p);
void  heap_stats(u64 *total, u64 *used);
void  pat_init_this_cpu(void);
u64   mem_cr3(void);

/* ------------------------------------------------------------ cpu.c (IDT, APIC, SMP) */
extern int  cpu_count;                     /* cores online */
extern u32  cpu_apic_id[];
extern int  timer_on;                      /* LAPIC timer ticking at 100 Hz */
extern volatile u64 timer_ticks;
extern const char *smp_note;               /* why SMP is off, or "" */
typedef void (*job_fn)(int cpu, void *arg);
void cpu_init(u64 rsdp_hint, const u8 *rsdp_copy);
void smp_run_all(job_fn fn, void *arg);
void delay_us(u32 us);
u64  tsc_khz(void);
void panic_screen(const char *title, const char *l1, u64 a, u64 b, u64 c);

/* ------------------------------------------------------------ kernel.c (terminal, keyboard, video) */
#define COL_PANEL   0x11111b
#define COL_WIN_BG  0x0d1117
#define COL_TITLE   0x2a2d45
#define COL_BORDER  0x3a3f5c
#define COL_PROMPT  0x7ee787
#define COL_FG      0xd7dae0
#define COL_DIR     0x79c0ff
#define COL_ERR     0xff7b72
#define COL_DIM     0x8b949e

#define KEY_UP    0x101
#define KEY_DOWN  0x102
#define KEY_LEFT  0x103
#define KEY_RIGHT 0x104
#define KEY_HOME  0x105
#define KEY_END   0x106
#define KEY_DEL   0x107
#define KEY_PGUP  0x108
#define KEY_PGDN  0x109
#define KEY_CTRL  0x200            /* OR-ed with the lower-case letter */

extern int term_cols, term_rows;
extern int t_h, t_m, t_s, t_d, t_mo, t_y;
void term_write(const char *s);
void term_write_col(const char *s, u32 col);
void term_putc_raw(char ch);
void print_u(u64 v);
void rtc_read(void);
void term_set(int r, int c, char ch, u32 fg, int inv);   /* write one cell (redraws if changed) */
void term_cursor_off(void);
void term_clear(void);
int  term_save(void);                       /* alternate screen: save ... */
void term_restore(void);                    /* ... and restore */
int  key_poll(void);
int  key_wait(void);                        /* blocking; idles the CPU */
void idle_tick(void);
void serial_write(const char *s);

/* ------------------------------------------------------------ fs.c : RAM filesystem */
#define FS_NAME_MAX 63
#define FS_PATH_MAX 256
#define FS_FILE_MAX (1024 * 1024)
typedef struct fnode {
    char  name[FS_NAME_MAX + 1];
    u8    is_dir;
    struct fnode *parent, *child, *next;      /* children: singly linked, sorted by name */
    u8   *data; usize size, cap;
    u16   my; u8 mmo, md, mh, mmi;            /* modification time */
} fnode;
extern fnode *fs_root, *fs_cwd, *fs_oldcwd;
void   fs_init(void);
fnode *fs_find(const char *path);                              /* NULL if missing */
int    fs_split(const char *path, fnode **parent, char *leaf); /* 0 ok, <0 error */
fnode *fs_create(fnode *dir, const char *name, int is_dir);    /* NULL on failure */
int    fs_write(fnode *f, const void *data, usize n);          /* replace contents; 0 ok */
int    fs_append(fnode *f, const void *data, usize n);
void   fs_touch(fnode *f);
void   fs_unlink(fnode *n);                                    /* detach + free (recursive) */
void   fs_attach(fnode *dir, fnode *n);                        /* sorted insert */
void   fs_detach(fnode *n);
int    fs_is_within(fnode *anc, fnode *n);                     /* n == anc or n inside anc */
void   fs_path(fnode *n, char *out, usize cap);
u64    fs_total_bytes(void);

/* ------------------------------------------------------------ cmds.c : shell commands for the filesystem */
int  cmd_fs_dispatch(int argc, char **argv, fnode *redir);     /* 1 if handled */
void cmd_out(const char *s);                                   /* output honouring redirection */
void cmd_out_col(const char *s, u32 col);
void cmd_err(const char *prog, const char *msg, const char *arg);

/* ------------------------------------------------------------ vim.c */
void vim_run(const char *path);

#endif
int mem_range_usable(u64 s, u64 e);        /* [s,e) lies inside one usable RAM range (any address) */
