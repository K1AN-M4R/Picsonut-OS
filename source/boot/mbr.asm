; Picsonut OS - hybrid MBR (only used when the ISO is written to a USB stick / disk).
; Copies itself out of the way, loads the 2048-byte BIOS loader image from the ISO into
; 0x7C00 and starts it.  The ISO builder patches `dap_lba` (512-byte sectors) and the
; partition table.
bits 16
org 0x0600

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    jmp 0x0000:relocated
relocated:
    mov si, dap
    mov ah, 0x42
    int 0x13                        ; DL = boot drive, still intact
    jc  .fail
    jmp 0x0000:0x7C00
.fail:
    mov ax, 0x0E21                  ; print '!'
    xor bx, bx
    int 0x10
.hang:
    hlt
    jmp .hang

times 0x1A8 - ($ - $$) db 0
dap:        db 0x10, 0
            dw 4                    ; 4 * 512 = 2048 bytes
            dw 0x7C00, 0
dap_lba:    dq 0                    ; at offset 0x1B0
times 0x1FE - ($ - $$) db 0
            dw 0xAA55
