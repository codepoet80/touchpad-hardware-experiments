/*
 * hid_parser.h -- minimal HID report-descriptor parser.
 *
 * Walks a raw HID report descriptor (the bytes the CSR stack stashes at
 * dev+0x6a0) and produces a flat list of INPUT fields, each already resolved to
 * a Linux input (EV type + code) mapping, plus a coarse device classification. This
 * is what lets one code path drive keyboards, mice and gamepads instead of the
 * stock keyboard-only translator.
 *
 * Scope for v0.1: variable INPUT fields (axes, buttons) and hat switches -- the
 * shapes real BR/EDR mice and gamepads actually use.  Array fields (classic
 * keyboard keycode arrays) are detected for classification but left to the
 * stock library path (we delegate keyboards untouched).
 */
#ifndef WEBOS_BT_SHIM_HID_PARSER_H
#define WEBOS_BT_SHIM_HID_PARSER_H

#include <stdint.h>

#define HID_MAX_FIELDS 128

/* HID usage pages */
#define HID_UP_GENERIC_DESKTOP 0x01
#define HID_UP_SIMULATION      0x02
#define HID_UP_KEYBOARD        0x07
#define HID_UP_BUTTON          0x09
#define HID_UP_CONSUMER        0x0c

/* Generic Desktop usages */
#define HID_GD_X       0x30
#define HID_GD_Y       0x31
#define HID_GD_Z       0x32
#define HID_GD_RX      0x33
#define HID_GD_RY      0x34
#define HID_GD_RZ      0x35
#define HID_GD_SLIDER  0x36
#define HID_GD_DIAL    0x37
#define HID_GD_WHEEL   0x38
#define HID_GD_HAT     0x39
#define HID_GD_MOUSE   0x02
#define HID_GD_JOYSTICK 0x04
#define HID_GD_GAMEPAD 0x05
#define HID_GD_KEYBOARD 0x06

struct hid_field {
    uint8_t  report_id;    /* 0 if descriptor uses no report IDs         */
    uint16_t bit_offset;   /* bit position within the report payload     */
    uint8_t  bit_size;     /* bits per this control                      */
    uint8_t  is_signed;    /* logical_min < 0                            */
    int32_t  lmin, lmax;   /* logical min/max                           */
    uint16_t usage_page;
    uint16_t usage;        /* resolved usage for this single control     */

    uint16_t evtype;       /* EV_REL / EV_ABS / EV_KEY                   */
    uint16_t evcode;       /* REL_* / ABS_* / BTN_*                      */
    uint8_t  is_hat;       /* hat switch: evcode is ABS_HAT0X (X+Y pair) */
};

/* descriptor-declared INPUT payload size (bits) per report id -- used at emit
 * time to detect how the stack framed the buffer (bare payload / [id,payload] /
 * full HIDP frame [0xA1,id,payload]) */
#define HID_MAX_REPORT_IDS 16
struct hid_report_bits { uint8_t id; uint16_t bits; };

struct hid_profile {
    int uses_report_id;
    int is_mouse;
    int is_gamepad;
    int is_keyboard;
    int report_id_hint;    /* first report id seen for INPUT (for logging) */

    struct hid_report_bits report_bits[HID_MAX_REPORT_IDS];
    int nreport_bits;

    struct hid_field fields[HID_MAX_FIELDS];
    int nfields;
};

/* Returns 0 on success (profile filled), <0 on parse error. */
int hid_parse(const uint8_t *desc, int len, struct hid_profile *out);

/* INPUT payload size in bits for report `id` (0 = no-report-id descriptor),
 * or -1 if the descriptor declares no such input report. */
int hid_report_payload_bits(const struct hid_profile *p, uint8_t id);

/* Human-readable one-liner for logs. */
void hid_profile_describe(const struct hid_profile *p, char *buf, int buflen);

#endif /* WEBOS_BT_SHIM_HID_PARSER_H */
