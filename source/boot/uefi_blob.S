/* Embeds the flat kernel image into the UEFI loader. */
.section .rodata
.balign 16
.global kernel_image
kernel_image:
.incbin "build/kernel.bin"
