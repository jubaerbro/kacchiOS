/* serial.c - Serial port driver (COM1) */
#include "serial.h"
#include "io.h"

#define COM1 0x3F8   /* I/O port base address for COM1 */

/*
You can find more information here: https://caro.su/msx/ocm_de1/16550.pdf

Your Keyboard
    ↓
Terminal (stdin)
    ↓
QEMU (-serial stdio)
    ↓
Emulated COM1 port (0x3F8)
    ↓
serial_getc() reads from COM1
    ↓
Your OS receives the character

If you want real keyboard input, you'd need to add a keyboard driver.
*/

void serial_init(void) {
    outb(COM1 + 1, 0x00);    /* Disable interrupts */
    outb(COM1 + 3, 0x80);    /* Enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x03);    /* Divisor low byte (38400 baud) */
    outb(COM1 + 1, 0x00);    /* Divisor high byte */
    outb(COM1 + 3, 0x03);    /* 8 bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xC7);    /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1 + 4, 0x0B);    /* IRQs enabled, RTS/DSR set */
}

static int is_transmit_empty(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    if (c == '\n') {
        serial_putc('\r');  /* Add carriage return */
    }
    while (!is_transmit_empty());
    outb(COM1, c);
}

void serial_puts(const char* str) {
    while (*str) {
        serial_putc(*str++);
    }
}

static int serial_received(void) {
    return inb(COM1 + 5) & 0x01;
}

char serial_getc(void) {
    while (!serial_received());
    return inb(COM1);
}
