/* Picsonut OS - IDT + exceptions, local APIC + timer, ACPI MADT parsing and SMP start-up.
 *
 * Multi-core model: the bootstrap CPU (BSP) runs the shell and the UI.  Every other core found in the
 * ACPI MADT is started with INIT/SIPI, gets its own stack, GDT/IDT and sits in a `hlt` loop until the
 * BSP hands it work with smp_run_all() (an IPI wakes it).  There is no preemptive scheduler. */
#include "kernel.h"

#define MAXCPU 64

int  cpu_count = 1;
u32  cpu_apic_id[MAXCPU];
int  timer_on;
volatile u64 timer_ticks;
const char *smp_note = "";

static volatile u32 *lapic;
static u64 tsc_per_us;
static u32 lapic_ticks_10ms;

/* ------------------------------------------------------------ IDT */
typedef struct { u16 lo, sel; u8 ist, attr; u16 mid; u32 hi, zero; } __attribute__((packed)) idt_ent;
static idt_ent idt[256] __attribute__((aligned(16)));
extern const u64 isr_table[64];
extern const u64 isr_255_addr;
extern void isr_ignore(void);
extern const u8 gdt_ptr64[];

typedef struct {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 vec, err, rip, cs, rflags, rsp, ss;
} frame_t;

static void idt_set(int v, u64 addr) {
    idt[v].lo = (u16)addr; idt[v].sel = 0x08; idt[v].ist = 0; idt[v].attr = 0x8E;
    idt[v].mid = (u16)(addr >> 16); idt[v].hi = (u32)(addr >> 32); idt[v].zero = 0;
}
static void idt_load(void) {
    struct { u16 lim; u64 base; } __attribute__((packed)) p = { sizeof idt - 1, (u64)(usize)idt };
    __asm__ volatile("lidt %0" :: "m"(p));
}
static void gdt_reload(void) {
    __asm__ volatile(
        "lgdt (%0)\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\tmovw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\tmovw %%ax, %%es\n\tmovw %%ax, %%ss\n\tmovw %%ax, %%fs\n\tmovw %%ax, %%gs"
        :: "r"(gdt_ptr64) : "rax", "memory");
}

static const char *exc_name[32] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range", "invalid opcode",
    "device not available", "DOUBLE FAULT", "coprocessor overrun", "invalid TSS", "segment not present",
    "stack fault", "general protection fault", "PAGE FAULT", "reserved", "x87 FPU error",
    "alignment check", "machine check", "SIMD exception", "virtualization", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "security", "reserved" };

static inline u32 lr(u32 off) { return lapic[off / 4]; }
static inline void lw(u32 off, u32 v) { lapic[off / 4] = v; }
#define L_ID 0x20
#define L_TPR 0x80
#define L_EOI 0xB0
#define L_SVR 0xF0
#define L_ICR_LO 0x300
#define L_ICR_HI 0x310
#define L_LVT_TIMER 0x320
#define L_TIMER_INIT 0x380
#define L_TIMER_CUR 0x390
#define L_TIMER_DIV 0x3E0

void interrupt_dispatch(frame_t *f) {
    if (f->vec < 32) {
        u64 cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        panic_screen("KERNEL PANIC", exc_name[f->vec], f->rip, f->err, cr2);
    } else if (f->vec == 0x40) {
        timer_ticks++;
        if (lapic) lw(L_EOI, 0);
    } else if (f->vec == 0x41) {
        if (lapic) lw(L_EOI, 0);
    } else if (f->vec >= 0x20 && f->vec < 0x30) {          /* stray legacy PIC interrupt */
        if (f->vec >= 0x28) outb(0xA0, 0x20);
        outb(0x20, 0x20);
    }
}

static void idt_init(void) {
    for (int v = 0; v < 256; v++) idt_set(v, (u64)(usize)isr_ignore);
    for (int v = 0; v < 64; v++) idt_set(v, isr_table[v]);
    idt_set(255, isr_255_addr);
    idt_load();
}
static void pic_mask_all(void) {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);                /* remap away from the exception vectors */
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xFF); outb(0xA1, 0xFF);                /* everything masked: keyboard is polled */
}

/* ------------------------------------------------------------ time base */
void delay_us(u32 us) {
    if (tsc_per_us) {
        u64 end = rdtsc() + (u64)us * tsc_per_us;
        while (rdtsc() < end) pause();
    } else {
        for (u32 i = 0; i < us; i++) inb(0x80);          /* ~1 us per ISA-port access */
    }
}
u64 tsc_khz(void) { return tsc_per_us * 1000; }

/* Measure the TSC and the LAPIC timer against PIT channel 2 over ~10 ms. */
static int calibrate(void) {
    u8 sv = inb(0x61);
    outb(0x61, (u8)((sv & 0xFC) | 0x01));              /* gate on, speaker off */
    outb(0x43, 0xB0);                                  /* ch2, lo/hi, mode 0 */
    outb(0x42, 11932 & 0xFF); outb(0x42, 11932 >> 8);  /* 11932 / 1.193182 MHz = 10 ms */
    if (lapic) { lw(L_TIMER_DIV, 0x3); lw(L_LVT_TIMER, (1u << 16) | 0x40); lw(L_TIMER_INIT, 0xFFFFFFFFu); }
    u64 t0 = rdtsc();
    u32 guard = 0;
    while (!(inb(0x61) & 0x20)) if (++guard > 3000000u) { outb(0x61, sv); return 0; }
    u64 t1 = rdtsc();
    u32 cur = lapic ? lr(L_TIMER_CUR) : 0;
    if (lapic) lw(L_TIMER_INIT, 0);
    outb(0x61, sv);
    tsc_per_us = (t1 - t0) / 10000;
    if (lapic) lapic_ticks_10ms = 0xFFFFFFFFu - cur;
    return tsc_per_us != 0 && (!lapic || lapic_ticks_10ms != 0);
}

/* ------------------------------------------------------------ local APIC */
static void lapic_enable_local(void) {
    wrmsr(0x1B, rdmsr(0x1B) | (1u << 11));
    lw(L_TPR, 0);
    lw(L_SVR, 0x100 | 0xFF);
}
static void lapic_ipi(u32 apic_id, u32 lo) {
    lw(L_ICR_HI, apic_id << 24);
    lw(L_ICR_LO, lo);
    for (u32 i = 0; i < 100000 && (lr(L_ICR_LO) & 0x1000); i++) pause();
}

/* ------------------------------------------------------------ ACPI */
static int sum_ok(const u8 *p, u32 n) { u8 s = 0; for (u32 i = 0; i < n; i++) s = (u8)(s + p[i]); return s == 0; }
static int rsdp_ok(const u8 *p) { return !memcmp(p, "RSD PTR ", 8) && sum_ok(p, 20); }

static const u8 *find_rsdp(u64 hint, const u8 *copy) {
    if (copy && rsdp_ok(copy)) return copy;
    if (hint && hint < (4ULL << 30) && rsdp_ok((const u8 *)(usize)hint)) return (const u8 *)(usize)hint;
    u16 bda_ebda;
    __asm__ volatile("movw (%1), %0" : "=r"(bda_ebda) : "r"((usize)0x40E) : "memory");   /* BIOS data area */
    u32 ebda = (u32)bda_ebda << 4;
    if (ebda >= 0x80000 && ebda < 0xA0000)
        for (u32 a = ebda; a < ebda + 1024; a += 16) if (rsdp_ok((const u8 *)(usize)a)) return (const u8 *)(usize)a;
    for (u32 a = 0xE0000; a < 0x100000; a += 16) if (rsdp_ok((const u8 *)(usize)a)) return (const u8 *)(usize)a;
    return NULL;
}

static u32 madt_ids[MAXCPU]; static int n_madt;
static int parse_madt(const u8 *t) {
    u32 len = *(const u32 *)(t + 4);
    if (len < 44 || len > 65536 || !sum_ok(t, len)) return 0;
    for (u32 o = 44; o + 2 <= len; ) {
        u8 type = t[o], l = t[o + 1];
        if (l < 2 || o + l > len) break;
        if (type == 0 && l >= 8) {                       /* processor local APIC */
            u32 fl = *(const u32 *)(t + o + 4);
            if ((fl & 3) && n_madt < MAXCPU) madt_ids[n_madt++] = t[o + 3];
        } else if (type == 9 && l >= 16) {               /* x2APIC entry: usable only if the ID fits xAPIC */
            u32 id = *(const u32 *)(t + o + 4), fl = *(const u32 *)(t + o + 8);
            if ((fl & 3) && id < 255 && n_madt < MAXCPU) {
                int dup = 0; for (int i = 0; i < n_madt; i++) if (madt_ids[i] == id) dup = 1;
                if (!dup) madt_ids[n_madt++] = id;
            }
        }
        o += l;
    }
    return n_madt > 0;
}
static int acpi_cpus(u64 hint, const u8 *copy) {
    const u8 *r = find_rsdp(hint, copy);
    if (!r) return 0;
    u64 xs = r[15] >= 2 ? *(const u64 *)(r + 24) : 0;
    u32 rs = *(const u32 *)(r + 16);
    int wide = xs != 0;
    u64 root = wide ? xs : rs;
    if (!root || root >= (4ULL << 30)) return 0;
    const u8 *h = (const u8 *)(usize)root;
    u32 len = *(const u32 *)(h + 4);
    if (len < 36 || len > 65536) return 0;
    u32 esz = wide ? 8 : 4;
    for (u32 o = 36; o + esz <= len; o += esz) {
        u64 a = wide ? *(const u64 *)(h + o) : *(const u32 *)(h + o);
        if (!a || a >= (4ULL << 30)) continue;
        const u8 *t = (const u8 *)(usize)a;
        if (!memcmp(t, "APIC", 4) && parse_madt(t)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ SMP */
extern u8 ap_tramp_start[], ap_tramp_end[], ap_cr3[], ap_stack[], ap_entry[];
static volatile u32 ap_flag, ap_index;
static job_fn volatile job_fn_v;
static void * volatile job_arg;
static volatile u32 job_gen, job_done;

static void ap_loop(int idx) {
    u32 my_gen = job_gen;
    for (;;) {
        cli();
        if (job_gen != my_gen) {
            my_gen = job_gen;
            sti();
            job_fn_v(idx, job_arg);
            __sync_fetch_and_add(&job_done, 1);
            continue;
        }
        __asm__ volatile("sti; hlt");                   /* an IPI (vector 0x41) wakes us */
    }
}
void ap_entry_c(void) {
    gdt_reload();
    idt_load();
    pat_init_this_cpu();
    lapic_enable_local();
    u32 idx = ap_index;
    cpu_apic_id[idx] = lr(L_ID) >> 24;
    __sync_synchronize();
    ap_flag = 1;
    ap_loop((int)idx);
}

static void smp_start_aps(void) {
    if (n_madt < 2) { smp_note = "single processor"; return; }
    if (!mem_range_usable(0x8000, 0x9000)) { smp_note = "no low memory for AP trampoline"; return; }
    if (mem_cr3() >= (4ULL << 30) || !mem_cr3()) { smp_note = "page tables above 4 GiB"; return; }
    usize tlen = (usize)(ap_tramp_end - ap_tramp_start);
    u8 *dst = (u8 *)0x8000;
    u32 bsp = cpu_apic_id[0];
    for (int i = 0; i < n_madt && cpu_count < MAXCPU; i++) {
        if (madt_ids[i] == bsp) continue;
        u8 *stack = kmalloc(16384 + 16);
        if (!stack) { smp_note = "out of memory"; break; }
        memcpy(dst, ap_tramp_start, tlen);
        *(u64 *)(dst + (ap_cr3 - ap_tramp_start))   = mem_cr3();
        *(u64 *)(dst + (ap_stack - ap_tramp_start)) = ((u64)(usize)stack + 16384 + 8) & ~15ULL;
        *(u64 *)(dst + (ap_entry - ap_tramp_start)) = (u64)(usize)ap_entry_c;
        ap_flag = 0; ap_index = (u32)cpu_count;
        __sync_synchronize();
        lapic_ipi(madt_ids[i], 0x4500);                 /* INIT */
        delay_us(10000);
        for (int k = 0; k < 2 && !ap_flag; k++) {       /* SIPI x2, vector 0x08 -> 0x8000 */
            lapic_ipi(madt_ids[i], 0x4600 | 0x08);
            for (int w = 0; w < 200 && !ap_flag; w++) delay_us(100);
        }
        if (ap_flag) cpu_count++;
        else { smp_note = "an AP did not answer"; /* stack intentionally leaked: the core may still wake up */ }
    }
}

void smp_run_all(job_fn fn, void *arg) {
    if (cpu_count > 1) {
        job_arg = arg; job_fn_v = fn; job_done = 0;
        __sync_synchronize();
        job_gen++;
        lw(L_ICR_LO, 0xC4000 | 0x41);                  /* fixed IPI to all other cores */
    }
    fn(0, arg);
    if (cpu_count > 1) {
        u64 end = rdtsc() + (tsc_per_us ? tsc_per_us : 1000) * 2000000ULL;   /* ~2 s */
        while (job_done < (u32)(cpu_count - 1) && rdtsc() < end) pause();
    }
}

void cpu_init(u64 rsdp_hint, const u8 *rsdp_copy) {
    pic_mask_all();
    idt_init();

    u32 a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    int has_apic = (d >> 9) & 1;
    u64 base = rdmsr(0x1B);
    if (!has_apic)            { smp_note = "no local APIC (disabled by the VM/firmware)"; }
    else if (base & (1u << 10)) { smp_note = "x2APIC mode is not supported"; has_apic = 0; }
    else {
        lapic = (volatile u32 *)(usize)(base & 0xFFFFF000ULL);
        if ((u64)(usize)lapic >= (4ULL << 30)) { lapic = NULL; has_apic = 0; smp_note = "local APIC above 4 GiB"; }
    }
    if (has_apic) { lapic_enable_local(); cpu_apic_id[0] = lr(L_ID) >> 24; }
    else cpu_apic_id[0] = b >> 24;

    int cal = calibrate();
    if (has_apic && cal) {
        lw(L_TIMER_DIV, 0x3);
        lw(L_LVT_TIMER, 0x40 | (1u << 17));              /* periodic, vector 0x40 */
        lw(L_TIMER_INIT, lapic_ticks_10ms);              /* 100 Hz */
        timer_on = 1;
    }
    sti();
    if (timer_on) {                                      /* safety net: never `hlt` on a timer that does not tick */
        for (int i = 0; i < 50 && !timer_ticks; i++) delay_us(1000);
        if (!timer_ticks) { lw(L_LVT_TIMER, 1u << 16); timer_on = 0; smp_note = "APIC timer does not tick"; }
    }

    if (has_apic && cal) {
        if (acpi_cpus(rsdp_hint, rsdp_copy)) smp_start_aps();
        else if (!*smp_note) smp_note = "no ACPI MADT";
    } else if (has_apic && !*smp_note) smp_note = "timer calibration failed";
}
