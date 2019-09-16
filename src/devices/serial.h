#ifndef DEVICES_SERIAL_H
#define DEVICES_SERIAL_H

#include <stdint.h>

void serial_init_queue (void);
void serial_putc (uint8_t);
uint8_t serial_getc (void);
void serial_flush (void);
void serial_notify (void);

#endif /* devices/serial.h */
