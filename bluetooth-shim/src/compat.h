/*
 * compat.h -- self-contained Linux input/uinput ABI for the 2.6.35 target.
 *
 * We do NOT include the host's <linux/uinput.h>/<linux/input.h> so the shim
 * builds cleanly with the old Palm PDK gcc (4.3.3) regardless of the build
 * host's kernel headers.  Every constant/struct below matches what the shipped
 * libPmBtBsaif.so uses (the ioctl request numbers were recovered directly from
 * the decompiled PmBtBsaifHidOpenUInput) and the 2.6.35 kernel UAPI.
 */
#ifndef WEBOS_BT_SHIM_COMPAT_H
#define WEBOS_BT_SHIM_COMPAT_H

#include <stdint.h>
#include <sys/time.h>   /* struct timeval */

/* ---- event types (linux/input.h) ---- */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_REP 0x14

#define SYN_REPORT 0x00

/* ---- relative axes ---- */
#define REL_X     0x00
#define REL_Y     0x01
#define REL_Z     0x02
#define REL_RX    0x03
#define REL_RY    0x04
#define REL_RZ    0x05
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08

/* ---- absolute axes ---- */
#define ABS_X      0x00
#define ABS_Y      0x01
#define ABS_Z      0x02
#define ABS_RX     0x03
#define ABS_RY     0x04
#define ABS_RZ     0x05
#define ABS_THROTTLE 0x06
#define ABS_RUDDER 0x07
#define ABS_WHEEL  0x08
#define ABS_HAT0X  0x10
#define ABS_HAT0Y  0x11

/* ---- misc ---- */
#define MSC_SCAN  0x04
#define MSC_RAW   0x03

/* ---- buttons / keys we emit (linux/input.h) ---- */
#define BTN_LEFT    0x110
#define BTN_RIGHT   0x111
#define BTN_MIDDLE  0x112
#define BTN_SIDE    0x113
#define BTN_EXTRA   0x114

#define BTN_JOYSTICK 0x120
#define BTN_TRIGGER  0x120

#define BTN_GAMEPAD 0x130
#define BTN_SOUTH   0x130  /* A */
#define BTN_EAST    0x131  /* B */
#define BTN_C       0x132
#define BTN_NORTH   0x133  /* X */
#define BTN_WEST    0x134  /* Y */
#define BTN_Z       0x135
#define BTN_TL      0x136
#define BTN_TR      0x137
#define BTN_TL2     0x138
#define BTN_TR2     0x139
#define BTN_SELECT  0x13a
#define BTN_START   0x13b
#define BTN_MODE    0x13c
#define BTN_THUMBL  0x13d
#define BTN_THUMBR  0x13e

#define BTN_TRIGGER_HAPPY 0x2c0

#define BTN_DPAD_UP    0x220
#define BTN_DPAD_DOWN  0x221
#define BTN_DPAD_LEFT  0x222
#define BTN_DPAD_RIGHT 0x223

/* ---- bus types ---- */
#define BUS_BLUETOOTH 0x0005

/* ---- struct input_event: exactly 16 bytes, matches write(fd, ev, 0x10) ---- */
struct input_event {
    struct timeval time;   /* 8 bytes (32-bit) */
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

/* ---- struct input_id ---- */
struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

/* ---- struct uinput_user_dev: 1116 bytes (0x45c), matches write(fd, ..., 0x45c) ---- */
#define UINPUT_MAX_NAME_SIZE 80
#define ABS_CNT 64
struct uinput_user_dev {
    char name[UINPUT_MAX_NAME_SIZE];
    struct input_id id;
    int32_t ff_effects_max;
    int32_t absmax[ABS_CNT];
    int32_t absmin[ABS_CNT];
    int32_t absfuzz[ABS_CNT];
    int32_t absflat[ABS_CNT];
};

/* ---- uinput ioctls (UINPUT_IOCTL_BASE = 'U' = 0x55) ---- */
/* Recovered from the decompile: UI_SET_EVBIT = 0x40045564, and UI_SET_*BIT = base+n */
#define UI_DEV_CREATE   0x5501
#define UI_DEV_DESTROY  0x5502
#define UI_SET_EVBIT    0x40045564
#define UI_SET_KEYBIT   0x40045565
#define UI_SET_RELBIT   0x40045566
#define UI_SET_ABSBIT   0x40045567
#define UI_SET_MSCBIT   0x40045568
#define UI_SET_LEDBIT   0x40045569
#define UI_SET_PHYS     0x4004556c
#define UI_SET_SWBIT    0x4004556d
#define UI_SET_PROPBIT  0x4004556e

#define UINPUT_DEV_NODE "/dev/input/uinput"

#endif /* WEBOS_BT_SHIM_COMPAT_H */
