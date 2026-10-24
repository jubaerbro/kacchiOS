/* serial.h - Serial port driver interface */
#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char* str);
char serial_getc(void);

#endif