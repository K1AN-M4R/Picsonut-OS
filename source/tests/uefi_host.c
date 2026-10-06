/* Host-side test of boot/uefi.c: runs efi_main() against a fake UEFI firmware (GOP, memory map,
 * ExitBootServices that rejects the first map key) and dumps what the kernel would receive. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <sys/mman.h>
#include <stdint.h>
#define MS __attribute__((ms_abi))
typedef uint64_t u64; typedef uint32_t u32; typedef uint16_t u16; typedef uint8_t u8;

/* identical layouts to boot/uefi.c (copied on purpose: the test must not share definitions) */
typedef struct { u64 sig; u32 rev, size, crc, res; } HDR;
typedef struct TXT { void *Reset; u64 (MS *OutputString)(struct TXT *, const u16 *); void *a, *b, *c, *d; u64 (MS *ClearScreen)(struct TXT *); } TXT;
typedef struct { u32 Type, Pad; u64 PhysicalStart, VirtualStart, NumberOfPages, Attribute; } MD;
typedef struct {
    HDR Hdr; void *Raise, *Restore;
    u64 (MS *AllocatePages)(u32, u32, u64, u64 *); void *FreePages;
    u64 (MS *GetMemoryMap)(u64 *, MD *, u64 *, u64 *, u32 *);
    u64 (MS *AllocatePool)(u32, u64, void **);
    void *p1[7]; void *p2[3]; void *p3[5]; void *p4[5];
    u64 (MS *ExitBootServices)(void *, u64);
    void *p5[2]; u64 (MS *SetWatchdogTimer)(u64, u64, u64, u16 *);
    void *p6[7];
    u64 (MS *LocateProtocol)(const void *, void *, void **);
} BS;
typedef struct { HDR Hdr; u16 *vendor; u32 rev; void *ci; void *cih; void *coh; TXT *ConOut; void *seh; void *se; void *rt; BS *BootServices; } ST_;
typedef struct { u32 Version, H, V, Fmt, R, G, B, X, Scan; } GI;
typedef struct { u32 Max, Mode; GI *Info; u64 sz; u64 fb; u64 fbsz; } GM;
typedef struct GOP { u64 (MS *Query)(struct GOP *, u32, u64 *, GI **); u64 (MS *Set)(struct GOP *, u32); void *Blt; GM *Mode; } GOP;

static GI modes[] = {
    {0, 640, 480, 1, 0, 0, 0, 0, 640}, {0, 800, 600, 0, 0, 0, 0, 0, 800}, {0, 1280, 720, 2, 0, 0, 0, 0, 1280},
    {0, 1024, 768, 1, 0, 0, 0, 0, 1024 + 16}, {0, 1920, 1080, 1, 0, 0, 0, 0, 1920}, {0, 1024, 768, 3, 0, 0, 0, 0, 1024},
};
static int fmt_override = -1;
static GM gmode = { 6, 0, &modes[0], 36, 0xFD000000ULL, 0 };
static GOP gop;
static int set_calls, exit_calls, map_calls;
static u64 MS q(GOP *g, u32 i, u64 *sz, GI **info) { (void)g; if (i >= 6) return 1; *sz = 36; *info = &modes[i]; return 0; }
static u64 MS s(GOP *g, u32 i) { (void)g; set_calls++; gmode.Mode = i; gmode.Info = &modes[i]; return 0; }
static u64 MS loc(const void *guid, void *r, void **out) {
    static const u8 want[16] = {0xde,0xa9,0x42,0x90,0xdc,0x23,0x38,0x4a,0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a};
    (void)r; if (memcmp(guid, want, 16)) { fprintf(stderr, "wrong GOP GUID\n"); exit(2); }
    *out = &gop; return 0;
}
static u64 MS out_s(TXT *t, const u16 *str) { (void)t; for (; *str; str++) putchar(*str < 128 ? *str : '?'); return 0; }
static u64 MS clr(TXT *t) { (void)t; return 0; }
static u64 MS wd(u64 a, u64 b, u64 c, u16 *d) { (void)a; (void)b; (void)c; (void)d; return 0; }
static u64 MS allocp(u32 type, u32 mt, u64 pages, u64 *addr) {
    (void)mt;
    if (type == 2) {                                   /* AllocateAddress */
        void *p = mmap((void *)*addr, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED || (u64)p != *addr) { fprintf(stderr, "cannot map %#lx\n", (unsigned long)*addr); return 9; }
        memset(p, 0xAA, pages * 4096);                 /* firmware does not hand out zeroed pages */
        return 0;
    }
    void *p = mmap(0, pages * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED || (u64)p > *addr) return 9;
    memset(p, 0xAA, pages * 4096);
    *addr = (u64)p; return 0;
}
static u64 MS allocpool(u32 mt, u64 n, void **b) { (void)mt; *b = malloc(n); return *b ? 0 : 9; }
static MD fake_map[] = {
    {7, 0, 0x0,        0, 0x9F, 0}, {0, 0, 0x9F000, 0, 1, 0}, {7, 0, 0x100000, 0, 0x7000, 0},
    {3, 0, 0x7100000, 0, 0x30, 0}, {4, 0, 0x7130000, 0, 0x20, 0}, {10, 0, 0x7150000, 0, 4, 0},
    {9, 0, 0x7154000, 0, 8, 0}, {11, 0, 0xFEC00000ULL, 0, 1, 0}, {6, 0, 0x7160000, 0, 3, 0}, {2, 0, 0x7170000, 0, 5, 0},
};
#define DSZ 64                                       /* firmware descriptor stride (> sizeof(MD) on purpose) */
static u64 MS getmap(u64 *size, MD *map, u64 *key, u64 *dsize, u32 *ver) {
    u64 need = sizeof fake_map / sizeof *fake_map * DSZ;
    if (*size < need) { *size = need; return 0x8000000000000005ULL; }
    for (unsigned i = 0; i < sizeof fake_map / sizeof *fake_map; i++) { memset((u8 *)map + i * DSZ, 0, DSZ); memcpy((u8 *)map + i * DSZ, &fake_map[i], sizeof(MD)); }
    *size = need; *dsize = DSZ; *ver = 1; *key = 0x1000 + ++map_calls; return 0;
}
static u64 MS exitbs(void *img, u64 key) {
    (void)img; exit_calls++;
    if (exit_calls == 1) return 0x8000000000000002ULL;            /* EFI_INVALID_PARAMETER: stale key */
    if (key != 0x1000 + (u64)map_calls) { fprintf(stderr, "stale map key used!\n"); exit(2); }
    return 0;
}
static jmp_buf done;
static u64 got_mbi;
void host_kernel_entry(u64 mbi) { got_mbi = mbi; longjmp(done, 1); }

extern u64 MS efi_main(void *image, ST_ *st);

int main(int argc, char **argv) {
    const char *dump = argv[1], *kimg = argv[2];
    if (argc > 3) { /* variant: only a single, RGB-format 800x600 mode exists */
        gmode.Max = 1; modes[0] = modes[1]; gmode.Info = &modes[0]; fmt_override = 0;
    }
    gop.Query = q; gop.Set = s; gop.Mode = &gmode;
    TXT t = { .OutputString = out_s, .ClearScreen = clr };
    BS bs; memset(&bs, 0, sizeof bs);
    bs.AllocatePages = allocp; bs.AllocatePool = allocpool; bs.GetMemoryMap = getmap; bs.ExitBootServices = exitbs;
    bs.SetWatchdogTimer = wd; bs.LocateProtocol = loc;
    ST_ st; memset(&st, 0, sizeof st); st.ConOut = &t; st.BootServices = &bs;
    if (setjmp(done) == 0) { efi_main((void *)1, &st); fprintf(stderr, "efi_main returned without starting the kernel\n"); return 3; }

    /* what the kernel sees */
    FILE *f = fopen(kimg, "rb"); u8 kb[1 << 20]; size_t kn = fread(kb, 1, sizeof kb, f); fclose(f);
    if (memcmp((void *)0x100000, kb, kn)) { fprintf(stderr, "kernel image mismatch\n"); return 4; }
    u8 *rest = (u8 *)0x100000 + kn; for (size_t i = 0; i < 0x25700 - kn; i++) if (rest[i]) { fprintf(stderr, ".bss not zeroed at +%zu\n", i); return 5; }
    u32 total = *(u32 *)(uintptr_t)got_mbi;
    f = fopen(dump, "wb"); fwrite((void *)(uintptr_t)got_mbi, 1, total, f); fclose(f);
    printf("\nset_mode calls=%d exit_boot_services calls=%d mbi=%#lx total=%u\n", set_calls, exit_calls, (unsigned long)got_mbi, total);
    printf("mode index=%u\n", gmode.Mode);
    return 0;
}
