/*
 * wiimote.h -- Nintendo Wii Remote support for the shim.
 *
 * The Wii Remote is a non-standard BT-HID device: it needs a legacy PIN equal
 * to its own BD_ADDR reversed (1+2 sync), it stays silent until the host sends
 * a "data reporting mode" output report (0x12), and its input reports are custom
 * (button bitmask, not standard HID usages).  None of that fits the generic HID
 * translator, so Wii Remotes get this dedicated path.
 *
 * Requires no kernel Wiimote driver -- everything rides the CSR HIDH channel via
 * PmBtBsaifHidhSetReport / the reports delivered to PmBtBsaifHidSendToInput.
 */
#ifndef WEBOS_BT_SHIM_WIIMOTE_H
#define WEBOS_BT_SHIM_WIIMOTE_H

#include <stdint.h>

/* Is this 6-byte BD address a Nintendo device?  Fills written[6] with the
 * address in written (display) order regardless of the input byte order.
 * Returns 1 if a Nintendo OUI is recognised (in either orientation). */
int  wiimote_is_nintendo(const uint8_t *addr6, uint8_t written[6]);

/* Wii Remote 1+2 PIN = BD_ADDR bytes in reverse of written order. */
void wiimote_make_pin(const uint8_t written[6], uint8_t pin_out[6]);

/* Name-based detection ("Nintendo RVL-CNT-01" / "-TR"). */
int  wiimote_name_matches(const char *name);

/* Create a uinput node with the Wii Remote's buttons.  Returns fd or -1. */
int  wiimote_create_uinput(const char *name, uint16_t vendor, uint16_t product);

/* Called right after a Wii Remote's HID channel is up: set LEDs + reporting
 * mode so it starts streaming button data.  `handle` is the CSR HIDH handle. */
void wiimote_start_reporting(int handle);

/* Decode one Wii Remote input report and emit uinput events. */
void wiimote_decode(int fd, const unsigned char *rpt, int len);

#endif
