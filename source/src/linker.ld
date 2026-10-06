ENTRY(_start)
SECTIONS
{
    . = 1M;
    .multiboot2 : { KEEP(*(.multiboot2)) }
    .text   : { *(.text*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    .data   : { *(.data*) }
    . = ALIGN(4096);
    .bss    : { *(COMMON) *(.bss*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
