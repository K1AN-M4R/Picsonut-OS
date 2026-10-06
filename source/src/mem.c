/* Picsonut OS - physical memory map, page tables (any amount of RAM) and kernel heap.
 *
 * The boot stub identity-maps only the first 4 GiB.  mem_init() reads the firmware memory
 * map (E820 or UEFI, delivered as a Multiboot2 "mmap" tag), builds its own page tables
 *   - [0, 4 GiB)            : 2 MiB pages; RAM = write-back, everything else (MMIO) = uncached,
 *                             the framebuffer window = write-combining (PAT)
 *   - RAM above 4 GiB (up to 1 TiB): 1 GiB pages if the CPU has them, else 2 MiB pages
 *   - a 64-bit framebuffer address above 4 GiB is mapped explicitly
 * and then sets up a heap in the largest free region. */
#include "kernel.h"

#define KiB 1024ULL
#define MiB (1024ULL * KiB)
#define GiB (1024ULL * MiB)
#define MIN_ALLOC (2 * MiB)         /* nothing below 2 MiB is ever handed out (BIOS data, MBI, AP trampoline, kernel) */
#define MAX_PHYS  (1024ULL * GiB)  /* 1 TiB */
#define HEAP_MAX  GiB

#define P_PRESENT 0x001ULL
#define P_RW      0x002ULL
#define P_PWT     0x008ULL
#define P_PCD     0x010ULL
#define P_PS      0x080ULL
#define PAT_VALUE 0x0007010600070106ULL   /* PA0 WB, PA1 WC, PA2 UC-, PA3 UC (repeated) */

typedef struct { u64 base, len; u32 type; } raw_t;
#define RAW_MAX 512
static raw_t raw[RAW_MAX];
static int   nraw;

region_t mem_free[MAX_REGIONS];
int      mem_nfree;
u64      mem_usable_bytes, mem_top_usable, mem_mapped_bytes, mem_testwin;
int      mem_use_1g, mem_pat_wc;

static u64 *pml4;
static region_t ml[MAX_REGIONS];     /* "RAM-like" (usable + ACPI) ranges, 2 MiB aligned, merged */
static int      nml;

static inline u64 align_up(u64 v, u64 a)   { return (v + a - 1) & ~(a - 1); }
static inline u64 align_down(u64 v, u64 a) { return v & ~(a - 1); }

static void sort_merge(region_t *r, int *n, u64 gap_ok) {
    for (int i = 1; i < *n; i++) {                       /* insertion sort by base */
        region_t t = r[i]; int j = i - 1;
        while (j >= 0 && r[j].base > t.base) { r[j + 1] = r[j]; j--; }
        r[j + 1] = t;
    }
    int w = 0;
    for (int i = 0; i < *n; i++) {
        if (w && r[i].base <= r[w - 1].base + r[w - 1].len + gap_ok) {
            u64 e = r[i].base + r[i].len, pe = r[w - 1].base + r[w - 1].len;
            if (e > pe) r[w - 1].len = e - r[w - 1].base;
        } else r[w++] = r[i];
    }
    *n = w;
}

static void parse_map(const u8 *tag) {
    if (!tag) return;
    u32 size = *(const u32 *)(tag + 4), esz = *(const u32 *)(tag + 8);
    if (esz < 20) return;
    for (u32 off = 16; off + esz <= size && nraw < RAW_MAX; off += esz) {
        const u8 *e = tag + off;
        raw_t r = { *(const u64 *)e, *(const u64 *)(e + 8), *(const u32 *)(e + 16) };
        if (r.len) raw[nraw++] = r;
    }
}

static int ram_overlap(u64 s, u64 e) {
    for (int i = 0; i < nml; i++) if (ml[i].base < e && ml[i].base + ml[i].len > s) return 1;
    return 0;
}

/* ---- page-table pages come from the start of the first free region below 4 GiB (still
 *      reachable through the boot page tables while we build the new ones). */
static u64 *alloc_table(void) {
    for (int i = 0; i < mem_nfree; i++) {
        if (mem_free[i].len >= 4096 && mem_free[i].base + 4096 <= 4 * GiB) {
            u64 *p = (u64 *)(usize)mem_free[i].base;
            mem_free[i].base += 4096; mem_free[i].len -= 4096;
            memset(p, 0, 4096);
            return p;
        }
    }
    return NULL;
}
static u64 *next_table(u64 *t, int idx) {
    if (!(t[idx] & P_PRESENT)) {
        u64 *n = alloc_table();
        if (!n) return NULL;
        t[idx] = (u64)(usize)n | P_PRESENT | P_RW;
    }
    return (u64 *)(usize)(t[idx] & 0x000FFFFFFFFFF000ULL);
}
static int map2m(u64 pa, u64 fl) {
    u64 *pdpt = next_table(pml4, (int)((pa >> 39) & 511)); if (!pdpt) return 0;
    u64 *pd = next_table(pdpt, (int)((pa >> 30) & 511));   if (!pd) return 0;
    pd[(pa >> 21) & 511] = pa | fl | P_PS | P_PRESENT | P_RW;
    return 1;
}
static int map1g(u64 pa, u64 fl) {
    u64 *pdpt = next_table(pml4, (int)((pa >> 39) & 511)); if (!pdpt) return 0;
    pdpt[(pa >> 30) & 511] = pa | fl | P_PS | P_PRESENT | P_RW;
    return 1;
}

void pat_init_this_cpu(void) {
    if (mem_pat_wc) wrmsr(0x277, PAT_VALUE);
}
u64 mem_cr3(void) { return (u64)(usize)pml4; }

/* ---------------------------------------------------------------- heap */
typedef struct blk { u64 size; u64 prev; } blk;      /* size includes the header; bit 0 = in use */
static u8 *heap_lo, *heap_hi;
static u64 heap_used_bytes;

static void heap_init(u64 base, u64 size) {
    base = align_up(base, 16); size &= ~15ULL;
    if (size < 4096) return;
    heap_lo = (u8 *)(usize)base; heap_hi = heap_lo + size;
    blk *f = (blk *)heap_lo;
    f->size = size - 16; f->prev = 0;
    blk *s = (blk *)(heap_hi - 16);
    s->size = 16 | 1; s->prev = f->size;
}
void *kmalloc(usize n) {
    if (!heap_lo) return NULL;
    n = (n + 15) & ~15UL; if (!n) n = 16;
    u64 need = n + 16;
    for (blk *b = (blk *)heap_lo; (u8 *)b < heap_hi - 16; b = (blk *)((u8 *)b + (b->size & ~1ULL))) {
        u64 sz = b->size & ~1ULL;
        if ((b->size & 1) || sz < need) continue;
        if (sz - need >= 32) {
            blk *r = (blk *)((u8 *)b + need);
            r->size = sz - need; r->prev = need;
            blk *nn = (blk *)((u8 *)r + r->size); nn->prev = r->size;
            b->size = need | 1;
        } else b->size = sz | 1;
        heap_used_bytes += b->size & ~1ULL;
        return b + 1;
    }
    return NULL;
}
void *kzalloc(usize n) { void *p = kmalloc(n); if (p) memset(p, 0, n); return p; }
void kfree(void *p) {
    if (!p) return;
    blk *b = (blk *)p - 1;
    if (!(b->size & 1)) return;                       /* double free: ignore */
    b->size &= ~1ULL;
    heap_used_bytes -= b->size;
    blk *nx = (blk *)((u8 *)b + b->size);
    if (!(nx->size & 1)) b->size += nx->size;
    if (b->prev) {
        blk *pv = (blk *)((u8 *)b - b->prev);
        if (!(pv->size & 1)) { pv->size += b->size; b = pv; }
    }
    blk *nn = (blk *)((u8 *)b + b->size);
    nn->prev = b->size;
}
void *krealloc(void *p, usize n) {
    if (!p) return kmalloc(n);
    blk *b = (blk *)p - 1;
    usize osz = (usize)(b->size & ~1ULL) - 16;
    if (n <= osz) return p;
    void *q = kmalloc(n);
    if (!q) return NULL;
    memcpy(q, p, osz);
    kfree(p);
    return q;
}
void heap_stats(u64 *total, u64 *used) { *total = (u64)(heap_hi - heap_lo); *used = heap_used_bytes; }

int mem_range_usable(u64 s, u64 e) {
    for (int i = 0; i < nraw; i++)
        if (raw[i].type == 1 && raw[i].base <= s && raw[i].base + raw[i].len >= e) return 1;
    return 0;
}

/* ---------------------------------------------------------------- init */
void mem_init(const u8 *mmap_tag, u64 fb_base, u64 fb_win) {
    parse_map(mmap_tag);

    /* totals + the list of RAM-like ranges (2 MiB aligned outward, merged) */
    region_t tmp[MAX_REGIONS]; int nt = 0;
    for (int i = 0; i < nraw; i++) {
        if (raw[i].type == 1) mem_usable_bytes += raw[i].len;
        if (raw[i].type == 1 || raw[i].type == 3 || raw[i].type == 4) {
            u64 s = align_down(raw[i].base, 2 * MiB), e = align_up(raw[i].base + raw[i].len, 2 * MiB);
            if (e > MAX_PHYS) e = MAX_PHYS;
            if (s < e && nt < MAX_REGIONS) { tmp[nt].base = s; tmp[nt].len = e - s; nt++; }
        }
    }
    sort_merge(tmp, &nt, 0);
    for (int i = 0; i < nt; i++) ml[i] = tmp[i];
    nml = nt;
    for (int i = 0; i < nml; i++) mem_mapped_bytes += ml[i].len;

    /* free (allocatable) regions: type 1, page aligned, >= 2 MiB, never above 1 TiB */
    nt = 0;
    for (int i = 0; i < nraw; i++) {
        if (raw[i].type != 1) continue;
        u64 s = align_up(raw[i].base, 4096), e = align_down(raw[i].base + raw[i].len, 4096);
        if (s < MIN_ALLOC) s = MIN_ALLOC;
        if (e > MAX_PHYS) e = MAX_PHYS;
        if (s < e && nt < MAX_REGIONS) { tmp[nt].base = s; tmp[nt].len = e - s; nt++; }
    }
    sort_merge(tmp, &nt, 0);
    for (int i = 0; i < nt; i++) { mem_free[i] = tmp[i]; if (tmp[i].base + tmp[i].len > mem_top_usable) mem_top_usable = tmp[i].base + tmp[i].len; }
    mem_nfree = nt;

    /* CPU features */
    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    mem_pat_wc = (d >> 16) & 1;
    cpuid(0x80000000, &a, &b, &c, &d);
    mem_use_1g = 0;
    if (a >= 0x80000001) { cpuid(0x80000001, &a, &b, &c, &d); mem_use_1g = (d >> 26) & 1; }

    /* ---- build the page tables */
    pml4 = alloc_table();
    int ok = pml4 != NULL;
    u64 fb_lo = align_down(fb_base, 2 * MiB), fb_hi = align_up(fb_base + fb_win, 2 * MiB);
    for (u64 pa = 0; ok && pa < 4 * GiB; pa += 2 * MiB) {
        u64 fl;
        if (ram_overlap(pa, pa + 2 * MiB))                 fl = 0;                         /* WB  */
        else if (mem_pat_wc && pa >= fb_lo && pa < fb_hi)  fl = P_PWT;                     /* WC  */
        else                                               fl = P_PWT | P_PCD;             /* UC  */
        ok = map2m(pa, fl);
    }
    for (int i = 0; ok && i < nml; i++) {
        u64 s = ml[i].base, e = ml[i].base + ml[i].len;
        if (e <= 4 * GiB) continue;
        for (u64 pa = s < 4 * GiB ? 4 * GiB : s; ok && pa < e; ) {
            if (mem_use_1g && !(pa & (GiB - 1)) && pa + GiB <= e) { ok = map1g(pa, 0); pa += GiB; }
            else { ok = map2m(pa, 0); pa += 2 * MiB; }
        }
    }
    if (ok && fb_base >= 4 * GiB && !ram_overlap(fb_lo, fb_hi)) {
        for (u64 pa = fb_lo; ok && pa < fb_hi; pa += 2 * MiB) ok = map2m(pa, mem_pat_wc ? P_PWT : 0);
    }
    if (ok) {
        pat_init_this_cpu();
        __asm__ volatile("mov %0, %%cr3" :: "r"((u64)(usize)pml4) : "memory");
    } else {
        pml4 = NULL;                                  /* out of table memory: stay on the 4 GiB boot mapping */
        mem_mapped_bytes = mem_mapped_bytes > 4 * GiB ? 4 * GiB : mem_mapped_bytes;
    }

    /* ---- heap: the largest free region (capped at 1 GiB); keep a 1 MiB window for `memtest` */
    int big = -1, top = -1;
    for (int i = 0; i < mem_nfree; i++) {
        if (big < 0 || mem_free[i].len > mem_free[big].len) big = i;
        if (mem_free[i].len >= 4 * MiB) top = i;
    }
    if (top >= 0) mem_testwin = mem_free[top].base + mem_free[top].len - MiB;
    if (big >= 0) {
        u64 base = mem_free[big].base, size = mem_free[big].len;
        if (size > HEAP_MAX) size = HEAP_MAX;
        if (big == top && base + size > mem_testwin) size = mem_testwin - base;
        heap_init(base, size);
    }
}
