/*
 * devinst.h -- layout of the CSR/BSA HID device instance and the DATA_IND
 * message, as recovered by reverse engineering libPmBtBsaif.so (webOS 3.0.5).
 *
 * See docs/ARCHITECTURE.md ?4/?5 for the decompiled evidence behind every
 * offset.  These are BYTE offsets into the opaque structs that the shipped
 * library passes to PmBtBsaifHidOpenUInput / PmBtBsaifHidSendToInput /
 * PmBtBsaifHidCloseUInput.  We treat the pointers as (uint8_t*) and read fields
 * by offset -- we never reproduce the whole struct, so we are robust to fields
 * we don't care about.
 *
 * IMPORTANT: every offset here is validated at runtime by the dump mode
 * (WEBOS_BT_SHIM_DUMP=1) before the translator trusts it.  If a future webOS
 * build shifts these, the dump output makes it obvious.
 */
#ifndef WEBOS_BT_SHIM_DEVINST_H
#define WEBOS_BT_SHIM_DEVINST_H

#include <stdint.h>

/* ---- device instance (param_1 / "dev") ---- */
#define DEV_ID           0x02   /* u8   device id / control handle           */
#define DEV_CLASS        0x04   /* u32  1=keyboard (only value ever written) */
#define DEV_BDADDR       0x08   /* u8[6] BD_ADDR                             */
#define DEV_PREV_REPORT  0x14   /* u8[256] previous input report            */
#define DEV_PREV_LEN     0x114  /* u32  previous report length              */
#define DEV_UINPUT_FLAG  0x118  /* u8   1 = uinput device created           */
#define DEV_UINPUT_FD    0x11c  /* int  uinput fd                           */
#define DEV_UDEV         0x120  /* struct uinput_user_dev (1116 B)          */
#define DEV_UDEV_NAME    0x120  /*   .name[80]                              */
#define DEV_UDEV_ID      0x170  /*   .id (bustype,vendor,product,version)   */
#define DEV_EV_SCRATCH   0x57c  /* struct input_event scratch (16 B)        */
#define DEV_SDPINFO      0x58c  /* u8   sdpInfo-present flag                 */
#define DEV_SUBCLASS     0x596  /* u8   HID device subclass (bit 0x40=kbd)  */
#define DEV_RDESC_LEN    0x69e  /* u16  HID report-descriptor length        */
#define DEV_RDESC        0x6a0  /* u8[<=1024] raw HID report descriptor     */

#define DEV_RDESC_MAX    0x400  /* library's own allocation guard           */

/* HID device subclass bits (mirror CoD peripheral minor class) */
#define SUBCLASS_KEYBOARD 0x40
#define SUBCLASS_POINTER  0x80
/* peripheral minor (bits 5..2): 0x04=joystick, 0x08=gamepad, 0x0c=remote */

/* ---- DATA_IND message (param_2 / "msg") ---- */
#define MSG_REPORT_TYPE  0x03   /* u8   1 = INPUT report        */
#define MSG_REPORT_LEN   0x04   /* u16  report length in bytes  */
#define MSG_REPORT_PTR   0x08   /* void* report bytes           */

#define REPORT_TYPE_INPUT 1

/* accessors */
#define U8(base, off)   (*(volatile uint8_t  *)((uint8_t *)(base) + (off)))
#define U16(base, off)  (*(volatile uint16_t *)((uint8_t *)(base) + (off)))
#define U32(base, off)  (*(volatile uint32_t *)((uint8_t *)(base) + (off)))
#define PTR(base, off)  (*(void **)((uint8_t *)(base) + (off)))
#define FIELD(base, off) ((uint8_t *)(base) + (off))

#endif /* WEBOS_BT_SHIM_DEVINST_H */
