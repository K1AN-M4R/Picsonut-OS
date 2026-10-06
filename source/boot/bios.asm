; Picsonut OS - BIOS loader
; El Torito "no emulation" boot image: the BIOS loads these 2048 bytes (4 virtual sectors) to 0x7C00.
;
; Steps: 64-bit CPU check -> A20 -> read kernel from the boot device -> resolution menu -> VBE 32bpp
;        linear framebuffer (exact match of the chosen size, else nearest) -> E820 memory map -> protected mode -> copy kernel to 1 MiB ->
;        jump to its 32-bit entry (EAX = 0x36d76289, EBX = Multiboot2-style info block).
;
; The ISO builder patches `kernel_lba` (offset 8, in 2048-byte sectors).
%include "build/kernel_info.inc"

bits 16
org 0x7C00

MBI        equ 0x1000          ; boot info block (needs < 0x6000 bytes)
VBEINFO    equ 0x9000          ; scratch: VBE controller info (512 bytes)
VBEMODE    equ 0x9200          ; scratch: VBE mode info (256 bytes)
BOUNCE     equ 0x10000         ; kernel is read here in real mode, then copied to 1 MiB
CHUNK      equ 32768           ; bytes per disk read (never crosses a 64 KiB boundary)

start:
    jmp short entry
    nop
    times 8 - ($ - $$) db 0
kernel_lba:   dd 0             ; patched by the ISO builder
kernel_bytes: dd KERNEL_FILESZ
kernel_mem:   dd KERNEL_MEMSZ

boot_drive:   db 0
unit_shift:   db 0             ; log2(2048 / device sector size): 0 = CD, 2 = 512-byte disk
chunk_secs:   dw 0
rest:         dd 0
best_diff:    dd 0xFFFFFFFF
mode_id:      dw 0
mode_ver:     dw 0
mode_w:       dw 0
mode_h:       dw 0
mode_pitch:   dw 0
mode_fb:      dd 0
mode_pos:     times 6 db 0     ; red pos,size  green pos,size  blue pos,size
tgt_w:        dw 1024          ; requested resolution (changed by the menu)
tgt_h:        dw 768

dap:          db 0x10, 0
dap_cnt:      dw 0
dap_off:      dw 0
dap_seg:      dw BOUNCE >> 4
dap_lba:      dq 0

entry:
    jmp 0x0000:entry2              ; some BIOSes start us at 07C0:0000 - normalise CS to 0
entry2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl

    mov si, msg_boot
    call print

    ; ---- 64-bit capable CPU?
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb  no_long
    mov eax, 0x80000001
    cpuid
    bt  edx, 29
    jnc no_long

    call a20_enable

    ; ---- device sector size (CD = 2048, USB/HDD = 512)
    mov byte [unit_shift], 0
    mov word [VBEINFO], 0x1E
    mov ah, 0x48
    mov dl, [boot_drive]
    mov si, VBEINFO
    int 0x13
    jc  .nosz
    cmp word [VBEINFO + 0x18], 512
    jne .nosz
    mov byte [unit_shift], 2
.nosz:
    mov cl, [unit_shift]

    ; ---- read the kernel into BOUNCE
    mov eax, [kernel_bytes]
    add eax, 2047
    shr eax, 11                    ; 2048-byte sectors
    shl eax, cl                    ; device sectors
    mov [rest], eax
    mov eax, [kernel_lba]
    shl eax, cl
    mov [dap_lba], eax
    mov ax, CHUNK / 2048
    shl ax, cl
    mov [chunk_secs], ax
.rd:
    mov eax, [rest]
    test eax, eax
    jz  .rd_done
    movzx ecx, word [chunk_secs]
    cmp eax, ecx
    jae .full
    mov ecx, eax
.full:
    mov [dap_cnt], cx
    sub [rest], ecx
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, dap
    int 0x13
    jc  disk_err
    movzx ecx, word [dap_cnt]
    add [dap_lba], ecx
    add word [dap_seg], CHUNK >> 4
    jmp .rd
.rd_done:

    call res_menu
    call vbe_setup
    call build_mbi

    ; ---- protected mode
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or  al, 1
    mov cr0, eax
    jmp 0x08:pm32

; ============================================================ real-mode helpers
print:                              ; DS:SI = zero-terminated string
    lodsb
    test al, al
    jz  .d
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp print
.d: ret

die:                                ; SI = message
    call print
.h: cli
    hlt
    jmp .h

no_long:  mov si, msg_nolong
          jmp die
disk_err: mov si, msg_disk
          jmp die

; ------------------------------------------------------------ A20
a20_enable:
    call a20_check
    test ax, ax
    jnz  .ok
    mov  ax, 0x2401                 ; BIOS
    int  0x15
    call a20_check
    test ax, ax
    jnz  .ok
    in   al, 0x92                   ; fast gate
    or   al, 2
    and  al, 0xFE
    out  0x92, al
    call a20_check
    test ax, ax
    jnz  .ok
    call kbc_wait                   ; keyboard controller
    mov  al, 0xD1
    out  0x64, al
    call kbc_wait
    mov  al, 0xDF
    out  0x60, al
    call kbc_wait
    call a20_check
    test ax, ax
    jnz  .ok
    mov  si, msg_a20
    jmp  die
.ok: ret

kbc_wait:
    in   al, 0x64
    test al, 2
    jnz  kbc_wait
    ret

a20_check:                          ; AX = 1 if A20 is enabled, else 0
    push ds
    push es
    xor  ax, ax
    mov  ds, ax
    mov  ax, 0xFFFF
    mov  es, ax
    mov  al, [0x0500]
    mov  ah, [es:0x0510]
    mov  byte [0x0500], 0x00
    mov  byte [es:0x0510], 0xFF
    cmp  byte [0x0500], 0xFF        ; equal => address wrapped => A20 off
    mov  [0x0500], al               ; (mov/pop keep the flags)
    mov  [es:0x0510], ah
    pop  es
    pop  ds
    mov  ax, 1
    jne  .y
    xor  ax, ax
.y: ret

; ------------------------------------------------------------ resolution menu
; Shows the list for ~3 seconds; keys 1-6 pick a size, Enter/timeout keep the default (1024x768).
res_menu:
    mov si, msg_menu
    call print
    xor ax, ax
    mov es, ax
    mov edx, [es:0x46C]             ; BIOS tick counter (18.2 Hz)
    add edx, 55                     ; ~3 seconds
.wait:
    mov ah, 1
    int 0x16
    jnz .key
    mov eax, [es:0x46C]
    sub eax, edx
    js  .wait                       ; deadline not reached yet (signed difference survives wrap-around)
    jmp .done
.key:
    xor ah, ah
    int 0x16
    cmp al, 13
    je  .done
    sub al, '1'
    cmp al, 6
    jae .wait                       ; not 1..6: keep waiting
    movzx bx, al
    shl bx, 2
    mov ax, [mode_table + bx]
    mov [tgt_w], ax
    mov ax, [mode_table + bx + 2]
    mov [tgt_h], ax
.done:
    xor ax, ax
    mov es, ax
    mov si, msg_crlf
    call print
    ret

; ------------------------------------------------------------ VBE
; Picks the 32-bpp linear-framebuffer mode closest to tgt_w x tgt_h and switches to it.
vbe_setup:
    mov dword [VBEINFO], 'VBE2'
    mov ax, 0x4F00
    mov di, VBEINFO
    int 0x10
    cmp ax, 0x004F
    jne vbe_fail
    cmp dword [VBEINFO], 'VESA'
    jne vbe_fail
    mov ax, [VBEINFO + 4]
    mov [mode_ver], ax
    mov si, [VBEINFO + 0x0E]        ; far pointer to the mode list
    mov ax, [VBEINFO + 0x10]
    mov fs, ax
.next:
    mov ax, [VBEINFO + 0x10]        ; (re)load FS: be paranoid about BIOS register use
    mov fs, ax
    mov cx, [fs:si]
    add si, 2
    cmp cx, 0xFFFF
    je  .done
    push si
    push cx
    mov ax, 0x4F01
    mov di, VBEMODE
    int 0x10
    pop cx
    pop si
    xor bx, bx
    mov ds, bx
    mov es, bx
    cmp ax, 0x004F
    jne .next
    mov ax, [VBEMODE]               ; attributes: supported, graphics, linear framebuffer
    and ax, 0x0091
    cmp ax, 0x0091
    jne .next
    cmp byte [VBEMODE + 0x19], 32   ; bits per pixel
    jne .next
    cmp byte [VBEMODE + 0x1B], 6    ; direct colour
    jne .next
    movzx eax, word [VBEMODE + 0x12]      ; score = |w - tgt_w| + |h - tgt_h|  (0 = exact match)
    movzx edx, word [tgt_w]
    sub eax, edx
    jns .abs1
    neg eax
.abs1:
    movzx ebx, word [VBEMODE + 0x14]
    movzx edx, word [tgt_h]
    sub ebx, edx
    jns .abs2
    neg ebx
.abs2:
    add eax, ebx
    cmp eax, [best_diff]
    jae .next
    mov [best_diff], eax            ; new best: remember everything we need
    mov [mode_id], cx
    mov ax, [VBEMODE + 0x12]
    mov [mode_w], ax
    mov ax, [VBEMODE + 0x14]
    mov [mode_h], ax
    mov ax, [VBEMODE + 0x10]
    mov [mode_pitch], ax
    mov eax, [VBEMODE + 0x28]
    mov [mode_fb], eax
    mov al, [VBEMODE + 0x20]
    mov [mode_pos + 0], al
    mov al, [VBEMODE + 0x1F]
    mov [mode_pos + 1], al
    mov al, [VBEMODE + 0x22]
    mov [mode_pos + 2], al
    mov al, [VBEMODE + 0x21]
    mov [mode_pos + 3], al
    mov al, [VBEMODE + 0x24]
    mov [mode_pos + 4], al
    mov al, [VBEMODE + 0x23]
    mov [mode_pos + 5], al
    jmp .next
.done:
    cmp word [mode_id], 0
    je  vbe_fail
    cmp byte [mode_pos + 1], 0      ; BIOS gave no colour layout: assume XRGB8888
    jne .set
    mov dword [mode_pos], 0x08080810
    mov word  [mode_pos + 4], 0x0800
.set:
    mov bx, [mode_id]
    or  bx, 0x4000                  ; linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne vbe_fail
    mov ax, 0x4F06                  ; actual bytes per scan line
    mov bl, 1
    int 0x10
    cmp ax, 0x004F
    jne .ret
    test bx, bx
    jz  .ret
    mov [mode_pitch], bx
.ret:
    ret
vbe_fail:
    mov si, msg_vbe
    jmp die

; ------------------------------------------------------------ boot info (Multiboot2 layout)
build_mbi:
    cld
    mov di, MBI
    xor eax, eax
    mov cx, 0x1800 / 4
    rep stosd                       ; clear 6 KiB

    ; tag 8: framebuffer
    mov dword [MBI + 8 + 0],  8
    mov dword [MBI + 8 + 4],  38
    mov eax, [mode_fb]
    mov [MBI + 8 + 8], eax          ; address (low); high dword already 0
    movzx eax, word [mode_pitch]
    mov [MBI + 8 + 16], eax
    movzx eax, word [mode_w]
    mov [MBI + 8 + 20], eax
    movzx eax, word [mode_h]
    mov [MBI + 8 + 24], eax
    mov byte [MBI + 8 + 28], 32
    mov byte [MBI + 8 + 29], 1      ; direct RGB
    mov eax, [mode_pos]
    mov [MBI + 8 + 32], eax
    mov ax, [mode_pos + 4]
    mov [MBI + 8 + 36], ax

    ; tag 6: memory map (E820), entries are 24 bytes: base, length, type, reserved
    mov dword [MBI + 48 + 0], 6
    mov dword [MBI + 48 + 8], 24
    mov di, MBI + 64
    xor ebx, ebx
    xor bp, bp
.e820:
    mov dword [di + 20], 1          ; ACPI "entry valid" bit for 20-byte answers
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    int 0x15
    jc  .edone
    cmp eax, 0x534D4150
    jne .edone
    test byte [di + 20], 1
    jz  .skip
    mov eax, [di + 8]
    or  eax, [di + 12]
    jz  .skip                       ; zero length
    add di, 24
    inc bp
    cmp bp, 100
    jae .edone
.skip:
    test ebx, ebx
    jnz .e820
.edone:
    movzx eax, bp
    imul eax, 24
    add eax, 16
    mov [MBI + 48 + 4], eax         ; tag size
    ; end tag right after the entries (di points past the last one)
    mov dword [di + 0], 0
    mov dword [di + 4], 8
    add di, 8
    movzx eax, di
    sub eax, MBI
    mov [MBI], eax                  ; total size
    ret

; ============================================================ protected mode
bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00
    cld
    mov esi, BOUNCE
    mov edi, KERNEL_LOAD
    mov ecx, [kernel_bytes]
    add ecx, 3
    shr ecx, 2
    rep movsd                       ; kernel image -> 1 MiB
    mov ecx, [kernel_mem]           ; zero the rest (.bss)
    add ecx, KERNEL_LOAD
    sub ecx, edi
    add ecx, 3
    shr ecx, 2
    xor eax, eax
    rep stosd
    mov eax, 0x36D76289
    mov ebx, MBI
    mov ecx, KERNEL_ENTRY32
    jmp ecx

; ============================================================ data
bits 16
align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF           ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF           ; 0x10: 32-bit data
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

msg_boot:   db "Picsonut OS - loading...", 13, 10, 0
msg_menu:   db 13, 10, "Screen resolution - press a key (default 3, starts in 3 s):", 13, 10
            db "  1) 640x480    2) 800x600    3) 1024x768", 13, 10
            db "  4) 1280x720   5) 1280x800   6) 1920x1080", 13, 10, 0
msg_crlf:   db 13, 10, 0
mode_table: dw 640, 480, 800, 600, 1024, 768, 1280, 720, 1280, 800, 1920, 1080
msg_nolong: db "Picsonut OS: this CPU is not 64-bit.", 0
msg_disk:   db "Picsonut OS: disk read error.", 0
msg_a20:    db "Picsonut OS: cannot enable A20.", 0
msg_vbe:    db "Picsonut OS: no 32-bit VBE graphics mode.", 0

times 2048 - ($ - $$) db 0
