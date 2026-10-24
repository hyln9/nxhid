#pragma once
#include <switch.h>

Result usb_keyboard_init(void);
void usb_keyboard_exit(void);
// Call continuously to service enumeration, LEDs and asynchronous idle reports.
// Any tick/write error is latched until exit/init; never reuse pending DMA buffers.
Result usb_keyboard_tick(void);
// Waits for delivery. A neutral report during suspend is deferred until resume.
Result usb_keyboard_write(const uint8_t report[8]);
// True after the initial/resume neutral report completes, while configured.
bool usb_keyboard_ready(void);
uint8_t usb_keyboard_leds(void);
