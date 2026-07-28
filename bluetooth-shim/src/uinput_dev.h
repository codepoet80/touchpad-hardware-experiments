/*
 * uinput_dev.h -- build a uinput node from a parsed HID profile and translate
 * reports into input_events.  This is the part the stock library never wrote:
 * it advertises EV_REL/EV_ABS/EV_KEY as the descriptor requires (not EV_KEY
 * only) so mice and gamepads produce real /dev/input/event* + js* nodes.
 */
#ifndef WEBOS_BT_SHIM_UINPUT_DEV_H
#define WEBOS_BT_SHIM_UINPUT_DEV_H

#include "hid_parser.h"
#include "compat.h"

/* Create a uinput device advertising exactly what `p` needs.
 * name/id are taken from the Bluetooth remote-device info.
 * Returns the uinput fd (>=0) or -1 on failure. */
int uinput_create(const struct hid_profile *p, const char *name, const struct input_id *id);

/* Decode one INPUT report and emit events (+ SYN). */
void uinput_emit_report(int fd, const struct hid_profile *p,
                        const unsigned char *report, int rlen);

void uinput_destroy(int fd);

#endif
