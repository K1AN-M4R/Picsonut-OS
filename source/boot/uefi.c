/* Picsonut OS - UEFI loader (BOOTX64.EFI)
 * Shows a resolution menu, picks the matching 32-bit GOP mode (exact, else nearest), copies the embedded kernel to 1 MiB,
 * builds a Multiboot2-style info block (framebuffer + memory map), exits boot services
 * and jumps to the kernel's 64-bit entry point.  No external headers or libraries. */
#include "../build/kernel_info.h"

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
typedef u64 UINTN;
typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef u16 CHAR16;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS 0

typedef struct { u32 a; u16 b, c; u8 d[8]; } EFI_GUID;

typedef struct { u64 sig; u32 rev, size, crc, res; } EFI_TABLE_HEADER;

typedef struct { u16 ScanCode, UnicodeChar; } EFI_INPUT_KEY;
typedef struct EFI_TEXT_IN {
    void *Reset;
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_TEXT_IN *, EFI_INPUT_KEY *);
} EFI_TEXT_IN;
typedef struct { EFI_GUID g; void *table; } EFI_CONFIG_TABLE;

typedef struct EFI_TEXT_OUT {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_TEXT_OUT *, const CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_TEXT_OUT *);
} EFI_TEXT_OUT;

typedef struct {
    u32 Type, Pad;
    u64 PhysicalStart, VirtualStart, NumberOfPages, Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(u32 type, u32 memtype, UINTN pages, u64 *addr);
    void *FreePages;
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *size, EFI_MEMORY_DESCRIPTOR *map, UINTN *key, UINTN *dsize, u32 *dver);
    EFI_STATUS (EFIAPI *AllocatePool)(u32 memtype, UINTN size, void **buf);
    void *FreePool, *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    void *HandleProtocol, *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath;
    void *InstallConfigurationTable, *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE image, UINTN key);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN timeout, u64 code, UINTN dsize, CHAR16 *data);
    void *ConnectController, *DisconnectController, *OpenProtocol, *CloseProtocol;
    void *OpenProtocolInformation, *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *proto, void *reg, void **iface);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor; u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle; EFI_TEXT_IN *ConIn;
    EFI_HANDLE ConsoleOutHandle; EFI_TEXT_OUT *ConOut;
    EFI_HANDLE StandardErrorHandle; void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries; EFI_CONFIG_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    u32 Version, HorizontalResolution, VerticalResolution, PixelFormat;
    u32 RedMask, GreenMask, BlueMask, ReservedMask;
    u32 PixelsPerScanLine;
} GOP_INFO;
typedef struct GOP {
    EFI_STATUS (EFIAPI *QueryMode)(struct GOP *, u32 mode, UINTN *size, GOP_INFO **info);
    EFI_STATUS (EFIAPI *SetMode)(struct GOP *, u32 mode);
    void *Blt;
    struct { u32 MaxMode, Mode; GOP_INFO *Info; UINTN InfoSize; u64 FrameBufferBase; UINTN FrameBufferSize; } *Mode;
} GOP;

static const EFI_GUID acpi2_guid = { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const EFI_GUID acpi1_guid = { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
static const EFI_GUID gop_guid = { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };

/* the kernel image, linked in by uefi_blob.S */
extern const u8 kernel_image[];

static EFI_SYSTEM_TABLE *ST;

static void say(const CHAR16 *s) { if (ST->ConOut) ST->ConOut->OutputString(ST->ConOut, s); }
static void halt(const CHAR16 *msg) {
    say(u"\r\nPicsonut OS: "); say(msg); say(u"\r\n");
    for (;;) __asm__ volatile("cli; hlt");
}
static void mcopy(void *d, const void *s, u64 n) { __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); }
static void mzero(void *d, u64 n) { __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(0) : "memory"); }

static u32 abs_diff(u32 a, u32 b) { return a > b ? a - b : b - a; }
static int guid_eq(const EFI_GUID *a, const EFI_GUID *b) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (int i = 0; i < 16; i++) if (x[i] != y[i]) return 0;
    return 1;
}
static u32 mask_pos(u32 m) { u32 p = 0; while (m && !(m & 1)) { m >>= 1; p++; } return p; }

/* resolution menu: 1-6 selects, Enter or a 3 s timeout keeps 1024x768 */
static void pick_resolution(EFI_SYSTEM_TABLE *st, u32 *tw, u32 *th) {
    static const u16 tab[6][2] = { {640,480}, {800,600}, {1024,768}, {1280,720}, {1280,800}, {1920,1080} };
    *tw = 1024; *th = 768;
    if (!st->ConIn) return;
    say(u"\r\nScreen resolution - press a key (default 3, starts in 3 s):\r\n"
        u"  1) 640x480    2) 800x600    3) 1024x768\r\n"
        u"  4) 1280x720   5) 1280x800   6) 1920x1080\r\n");
    for (int i = 0; i < 30; i++) {
        EFI_INPUT_KEY k;
        if (st->ConIn->ReadKeyStroke(st->ConIn, &k) == EFI_SUCCESS) {
            if (k.UnicodeChar == 13) return;
            if (k.UnicodeChar >= '1' && k.UnicodeChar <= '6') { *tw = tab[k.UnicodeChar - '1'][0]; *th = tab[k.UnicodeChar - '1'][1]; return; }
        } else st->BootServices->Stall(100000);
    }
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    ST = st;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    bs->SetWatchdogTimer(0, 0, 0, 0);
    if (st->ConOut) st->ConOut->ClearScreen(st->ConOut);
    say(u"Picsonut OS - loading...\r\n");

    /* ---- graphics: menu, then the 32-bit mode closest to the requested size */
    u32 tw, th;
    pick_resolution(st, &tw, &th);
    GOP *gop = 0;
    if (bs->LocateProtocol(&gop_guid, 0, (void **)&gop) != EFI_SUCCESS || !gop)
        halt(u"no graphics output (GOP) available.");
    u32 best = gop->Mode->Mode, best_score = 0xFFFFFFFFu; int found = 0;
    for (u32 i = 0; i < gop->Mode->MaxMode; i++) {
        UINTN sz; GOP_INFO *inf;
        if (gop->QueryMode(gop, i, &sz, &inf) != EFI_SUCCESS) continue;
        if (inf->PixelFormat > 2) continue;                  /* RGBX / BGRX / bit-mask only (not blt-only) */
        u32 score = abs_diff(inf->HorizontalResolution, tw) + abs_diff(inf->VerticalResolution, th);
        if (score < best_score) { best_score = score; best = i; found = 1; }
    }
    if (!found) halt(u"no 32-bit graphics mode.");
    if (best != gop->Mode->Mode) gop->SetMode(gop, best);
    GOP_INFO *gi = gop->Mode->Info;
    if (gi->PixelFormat > 2) halt(u"unsupported framebuffer.");

    /* ---- ACPI RSDP (needed by the kernel to find the other CPU cores) */
    u64 rsdp = 0;
    for (UINTN i = 0; i < st->NumberOfTableEntries; i++) {
        if (guid_eq(&st->ConfigurationTable[i].g, &acpi2_guid)) { rsdp = (u64)st->ConfigurationTable[i].table; break; }
        if (!rsdp && guid_eq(&st->ConfigurationTable[i].g, &acpi1_guid)) rsdp = (u64)st->ConfigurationTable[i].table;
    }

    /* ---- copy the kernel to its fixed address and zero its .bss */
    u64 kaddr = KERNEL_LOAD;
    u64 kpages = (KERNEL_MEMSZ + 4095) / 4096;
    if (bs->AllocatePages(2 /*AllocateAddress*/, 2 /*LoaderData*/, kpages, &kaddr) != EFI_SUCCESS)
        halt(u"cannot reserve memory at 1 MiB.");
    mzero((void *)KERNEL_LOAD, kpages * 4096);
    mcopy((void *)KERNEL_LOAD, kernel_image, KERNEL_FILESZ);

    /* ---- boot info block below 4 GiB, and a buffer for the firmware memory map */
    u64 mbi_addr = 0xFFFFFFFFULL;
    if (bs->AllocatePages(1 /*AllocateMaxAddress*/, 2, 8, &mbi_addr) != EFI_SUCCESS)
        halt(u"out of memory.");
    u8 *mbi = (u8 *)mbi_addr;
    mzero(mbi, 8 * 4096);

    UINTN map_cap = 128 * 1024;
    EFI_MEMORY_DESCRIPTOR *map = 0;
    if (bs->AllocatePool(2, map_cap, (void **)&map) != EFI_SUCCESS) halt(u"out of memory.");

    /* ---- leave boot services (retry: the map key changes if anything allocated in between) */
    UINTN map_size = 0, key = 0, dsize = 0; u32 dver = 0;
    int ok = 0;
    for (int tries = 0; tries < 8 && !ok; tries++) {
        map_size = map_cap;
        if (bs->GetMemoryMap(&map_size, map, &key, &dsize, &dver) != EFI_SUCCESS) halt(u"cannot read the memory map.");
        if (bs->ExitBootServices(image, key) == EFI_SUCCESS) ok = 1;
    }
    if (!ok) for (;;) __asm__ volatile("cli; hlt");
    __asm__ volatile("cli");                         /* firmware timer interrupts must not reach us any more */

    /* ---- from here on no firmware calls.  Build the Multiboot2-style info block. */
    /* tag 8: framebuffer */
    u8 *t = mbi + 8;
    *(u32 *)(t + 0) = 8;  *(u32 *)(t + 4) = 38;
    *(u64 *)(t + 8) = gop->Mode->FrameBufferBase;
    *(u32 *)(t + 16) = gi->PixelsPerScanLine * 4;
    *(u32 *)(t + 20) = gi->HorizontalResolution;
    *(u32 *)(t + 24) = gi->VerticalResolution;
    t[28] = 32; t[29] = 1;
    if (gi->PixelFormat == 0)      { t[32] = 0;  t[34] = 8; t[36] = 16; }   /* RGBX: R in byte 0 */
    else if (gi->PixelFormat == 1) { t[32] = 16; t[34] = 8; t[36] = 0;  }   /* BGRX: B in byte 0 */
    else { t[32] = (u8)mask_pos(gi->RedMask); t[34] = (u8)mask_pos(gi->GreenMask); t[36] = (u8)mask_pos(gi->BlueMask); }
    t[33] = t[35] = t[37] = 8;

    /* custom tag 0x1000: pointer to the ACPI RSDP */
    t = mbi + 48;
    *(u32 *)(t + 0) = 0x1000; *(u32 *)(t + 4) = 16; *(u64 *)(t + 8) = rsdp;

    /* tag 6: memory map */
    t = mbi + 64;
    u8 *e = t + 16; u32 n = 0;
    for (u8 *p = (u8 *)map; p + dsize <= (u8 *)map + map_size && n < 600; p += dsize) {
        EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)p;
        u32 type;
        switch (d->Type) {
        case 1: case 2: case 3: case 4: case 7: type = 1; break;   /* usable (incl. loader/boot-services) */
        case 9:  type = 3; break;                                     /* ACPI reclaimable */
        case 10: type = 4; break;                                     /* ACPI NVS */
        default: type = 2; break;
        }
        *(u64 *)(e + 0) = d->PhysicalStart;
        *(u64 *)(e + 8) = d->NumberOfPages * 4096;
        *(u32 *)(e + 16) = type;
        *(u32 *)(e + 20) = 0;
        e += 24; n++;
    }
    *(u32 *)(t + 0) = 6;
    *(u32 *)(t + 4) = 16 + 24 * n;
    *(u32 *)(t + 8) = 24;
    *(u32 *)(t + 12) = 0;
    *(u32 *)(e + 0) = 0; *(u32 *)(e + 4) = 8;       /* end tag */
    *(u32 *)mbi = (u32)((e + 8) - mbi);

    ((void (*)(u64))KERNEL_ENTRY64)(mbi_addr);        /* never returns */
    for (;;) __asm__ volatile("cli; hlt");
}
