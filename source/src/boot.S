/* Picsonut OS - boot stub.
 * Entry 1: _start       - 32-bit protected mode (GRUB Multiboot2 or Picsonut BIOS loader),
 *                        EAX = 0x36d76289, EBX = pointer to a Multiboot2-style info block.
 * Entry 2: uefi_entry    - already in 64-bit long mode (Picsonut UEFI loader), RDI = info block. */

.set MB2_MAGIC, 0xE85250D6

.section .multiboot2, "a"
.align 8
mb2_start:
    .long MB2_MAGIC
    .long 0                                   /* architecture: i386 protected mode */
    .long mb2_end - mb2_start
    .long -(MB2_MAGIC + (mb2_end - mb2_start))
    /* framebuffer request: 1024x768x32 (optional, bootloader may pick another) */
    .align 8
    .short 5
    .short 1
    .long 20
    .long 1024
    .long 768
    .long 32
    /* end tag */
    .align 8
    .short 0
    .short 0
    .long 8
mb2_end:

.section .text
.code32
.global _start
_start:
    cli
    movl $stack_top, %esp
    movl %ebx, mbi_ptr              /* GRUB passes the multiboot info address in EBX */

    cmpl $0x36d76289, %eax          /* multiboot2 bootloader magic */
    jne hang32

    /* check that the CPU supports long mode */
    movl $0x80000000, %eax
    cpuid
    cmpl $0x80000001, %eax
    jb hang32
    movl $0x80000001, %eax
    cpuid
    testl $(1 << 29), %edx
    jz hang32

    /* identity-map the first 4 GiB with 2 MiB pages */
    movl $pdpt, %eax
    orl $3, %eax
    movl %eax, pml4

    xorl %ecx, %ecx
1:  movl %ecx, %eax
    shll $12, %eax
    addl $pd, %eax
    orl $3, %eax
    movl %eax, pdpt(,%ecx,8)
    incl %ecx
    cmpl $4, %ecx
    jb 1b

    xorl %ecx, %ecx
2:  movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax                 /* present | writable | huge */
    movl %eax, pd(,%ecx,8)
    incl %ecx
    cmpl $2048, %ecx
    jb 2b

    movl $pml4, %eax
    movl %eax, %cr3

    movl %cr4, %eax
    orl $0x20, %eax                 /* PAE */
    movl %eax, %cr4

    movl $0xC0000080, %ecx          /* EFER */
    rdmsr
    orl $0x100, %eax                /* LME */
    wrmsr

    movl %cr0, %eax
    orl $0x80000001, %eax           /* PG | PE */
    movl %eax, %cr0

    lgdt gdt_ptr
    ljmp $0x08, $long_entry

hang32:
    hlt
    jmp hang32

.code64
long_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $stack_top, %rsp
    xorq %rbp, %rbp
    movl mbi_ptr(%rip), %edi
    call kmain
hang64:
    cli
    hlt
    jmp hang64

.code64
.global uefi_entry
uefi_entry:
    cli
    cld
    movq %rdi, %rbx                 /* keep the boot info pointer */
    lgdt gdt_ptr64(%rip)
    pushq $0x08
    leaq 1f(%rip), %rax
    pushq %rax
    lretq                           /* reload CS with our own 64-bit code segment */
1:  movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movq $stack_top, %rsp
    xorq %rbp, %rbp

    /* identity-map the first 4 GiB with 2 MiB pages (same tables as the 32-bit path) */
    movq $pdpt, %rax
    orq $3, %rax
    movq %rax, pml4
    xorq %rcx, %rcx
3:  movq %rcx, %rax
    shlq $12, %rax
    addq $pd, %rax
    orq $3, %rax
    movq %rax, pdpt(,%rcx,8)
    incq %rcx
    cmpq $4, %rcx
    jb 3b
    xorq %rcx, %rcx
4:  movq %rcx, %rax
    shlq $21, %rax
    orq $0x83, %rax
    movq %rax, pd(,%rcx,8)
    incq %rcx
    cmpq $2048, %rcx
    jb 4b
    movq $pml4, %rax
    movq %rax, %cr3

    movq %rbx, %rdi
    call kmain
    jmp hang64

.section .rodata
.align 8
gdt:
    .quad 0x0000000000000000
    .quad 0x00AF9A000000FFFF        /* 0x08: 64-bit code */
    .quad 0x00CF92000000FFFF        /* 0x10: data */
gdt_end:
.align 8
.global gdt_ptr64
gdt_ptr64:
    .word gdt_end - gdt - 1
    .quad gdt
gdt_ptr:
    .word gdt_end - gdt - 1
    .long gdt

.section .bss
.align 4096
pml4:   .skip 4096
pdpt:   .skip 4096
pd:     .skip 4096 * 4
.align 16
stack_bottom: .skip 65536
stack_top:
mbi_ptr: .skip 8
