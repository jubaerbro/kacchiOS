/* kernel.c - Main kernel with null process */
#include "types.h"
#include "serial.h"
#include "string.h"
#include "memory.h"

#define MAX_INPUT 128

/* Memory manager commands; returns 0 if input is not one of them */
static int memory_command(const char* cmd) {
    if (strcmp(cmd, "help") == 0) {
        serial_puts("Memory commands:\n");
        serial_puts("  meminfo   - memory layout, heap/stack usage, free list\n");
        serial_puts("  buddyinfo - buddy allocator free blocks per order\n");
        serial_puts("  slabinfo  - slab caches used by kmalloc()\n");
        serial_puts("  memdemo   - step-by-step allocation / free demo\n");
        serial_puts("  memtest   - run the memory manager self tests\n");
        serial_puts("Anything else is echoed back.\n");
    } else if (strcmp(cmd, "meminfo") == 0) {
        mem_info();
    } else if (strcmp(cmd, "buddyinfo") == 0) {
        buddy_info();
    } else if (strcmp(cmd, "slabinfo") == 0) {
        slab_info();
    } else if (strcmp(cmd, "memdemo") == 0) {
        mem_demo();
    } else if (strcmp(cmd, "memtest") == 0) {
        mem_selftest();
    } else {
        return 0;
    }
    return 1;
}

void kmain(uint32_t mb_magic, uint32_t mb_info) {
    char input[MAX_INPUT];
    int pos = 0;
    
    /* Initialize hardware */
    serial_init();
    
    /* Initialize memory manager (heap, stacks, buddy, slab) */
    mem_init(mb_magic, mb_info);
    
    /* Print welcome message */
    serial_puts("\n");
    serial_puts("========================================\n");
    serial_puts("    kacchiOS - Minimal Baremetal OS\n");
    serial_puts("========================================\n");
    serial_puts("Hello from kacchiOS!\n");
    mem_info();
    serial_puts("Type 'help' for memory commands.\n");
    serial_puts("Running null process...\n\n");
    
    /* Main loop - the "null process" */
    while (1) {
        serial_puts("kacchiOS> ");
        pos = 0;
        
        /* Read input line */
        while (1) {
            char c = serial_getc();
            
            /* Handle Enter key */
            if (c == '\r' || c == '\n') {
                input[pos] = '\0';
                serial_puts("\n");
                break;
            }
            /* Handle Backspace */
            else if ((c == '\b' || c == 0x7F) && pos > 0) {
                pos--;
                serial_puts("\b \b");  /* Erase character on screen */
            }
            /* Handle normal characters */
            else if (c >= 32 && c < 127 && pos < MAX_INPUT - 1) {
                input[pos++] = c;
                serial_putc(c);  /* Echo character */
            }
        }
        
        /* Run a memory command, otherwise echo back the input */
        if (pos > 0 && !memory_command(input)) {
            serial_puts("You typed: ");
            serial_puts(input);
            serial_puts("\n");
        }
    }
    
    /* Should never reach here */
    for (;;) {
        __asm__ volatile ("hlt");
    }
}