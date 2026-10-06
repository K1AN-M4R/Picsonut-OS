/* Flat, position-independent layout; tools/mkpe.py wraps the sections into a PE32+ file. */
ENTRY(efi_main)
SECTIONS
{
    . = 0x1000;
    .text   : { *(.text .text.*) }
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) *(.rela*) *(.got*) *(.plt*) *(.interp) }
}
