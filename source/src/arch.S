/* Picsonut OS - interrupt stubs and the application-processor (AP) start-up trampoline. */
.set AP_BASE, 0x8000                 /* trampoline is copied here (vector 0x08 in the SIPI) */

.section .text
.code64

/* ------------------------------------------------------------ interrupt stubs */
.macro ISR n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm
.macro ISRE n
isr\n:
    pushq $\n
    jmp isr_common
.endm

ISR 0
ISR 1
ISR 2
ISR 3
ISR 4
ISR 5
ISR 6
ISR 7
ISRE 8
ISR 9
ISRE 10
ISRE 11
ISRE 12
ISRE 13
ISRE 14
ISR 15
ISR 16
ISRE 17
ISR 18
ISR 19
ISR 20
ISRE 21
ISR 22
ISR 23
ISR 24
ISR 25
ISR 26
ISR 27
ISR 28
ISRE 29
ISRE 30
ISR 31
.irp n,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
ISR \n
.endr
ISR 255

isr_common:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, %rdi
    cld
    call interrupt_dispatch
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rdi
    popq %rsi
    popq %rbp
    popq %rbx
    popq %rdx
    popq %rcx
    popq %rax
    addq $16, %rsp
    iretq

.global isr_ignore
isr_ignore:
    iretq

.section .rodata
.align 8
.global isr_table
isr_table:
.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,63
.quad isr\n
.endr
.global isr_255_addr
isr_255_addr: .quad isr255

/* ------------------------------------------------------------ AP trampoline
 * Copied to AP_BASE by the BSP.  A started AP enters here in 16-bit real mode (CS = 0x0800, IP = 0),
 * switches to protected mode, then long mode (using the BSP's page tables) and calls ap_entry_c.
 * All references are computed relative to ap_tramp_start so the code works at its copied address. */
.section .text
.code16
.global ap_tramp_start
ap_tramp_start:
    cli
    cld
    movw %cs, %ax
    movw %ax, %ds
    lgdtl (ap_gdtr - ap_tramp_start)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $(AP_BASE + ap_pm - ap_tramp_start)

.code32
ap_pm:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl %cr4, %eax
    orl $0x20, %eax                  /* PAE */
    movl %eax, %cr4
    movl (AP_BASE + ap_cr3 - ap_tramp_start), %eax
    movl %eax, %cr3
    movl $0xC0000080, %ecx           /* EFER.LME */
    rdmsr
    orl $0x100, %eax
    wrmsr
    movl %cr0, %eax
    orl $0x80000000, %eax            /* PG */
    movl %eax, %cr0
    ljmpl $0x18, $(AP_BASE + ap_lm - ap_tramp_start)

.code64
ap_lm:
    movl $0x10, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorl %ebp, %ebp
    movq (AP_BASE + ap_stack - ap_tramp_start), %rsp
    movq (AP_BASE + ap_entry - ap_tramp_start), %rax
    call *%rax
1:  cli
    hlt
    jmp 1b

.align 8
ap_gdt:
    .quad 0x0000000000000000
    .quad 0x00CF9A000000FFFF         /* 0x08 32-bit code */
    .quad 0x00CF92000000FFFF         /* 0x10 data */
    .quad 0x00AF9A000000FFFF         /* 0x18 64-bit code */
ap_gdt_end:
ap_gdtr:
    .word ap_gdt_end - ap_gdt - 1
    .long AP_BASE + ap_gdt - ap_tramp_start
.align 8
.global ap_cr3
ap_cr3:   .quad 0
.global ap_stack
ap_stack: .quad 0
.global ap_entry
ap_entry: .quad 0
.global ap_tramp_end
ap_tramp_end:
