/* boot.S - Multiboot header + entry point */
.section .multiboot
.align 4
.long 0x1BADB002                    /* magic */
.long 0x00000002                    /* flags: bit 1 = give us memory info */
.long -(0x1BADB002 + 0x00000002)   /* checksum */

.section .bss
.align 16
stack_bottom:
    .skip 16384                     /* 16KB stack */
stack_top:

.section .text
.global start
.extern kmain

start:
    cli                             /* disable interrupts */
    mov $stack_top, %esp           /* set up stack */
    mov %eax, %esi                  /* save Multiboot magic (al is reused) */
    
    /* Clear BSS section */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %al, %al
    rep stosb
    
    push %ebx                       /* arg 2: Multiboot info pointer */
    push %esi                       /* arg 1: Multiboot magic */
    call kmain                      /* jump to C kernel */
    
.halt:
    cli
    hlt
    jmp .halt