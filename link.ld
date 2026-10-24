/* link.ld - Linker script */
OUTPUT_FORMAT(elf32-i386)
ENTRY(start)

SECTIONS {
    . = 1M;
    
    .text : {
        *(.multiboot)
        *(.text*)
        *(.rodata*)
    }
    
    .data : {
        *(.data*)
    }
    
    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        __bss_end = .;
    }
    
    /* Future: Students will use memory beyond this point */
    . = ALIGN(4096);
    __kernel_end = .;
}