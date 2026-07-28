#define _GNU_SOURCE
#include "wiimote.h"
#include "compat.h"
#include "log.h"
#include <dlfcn.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

/* Nintendo OUIs (first 3 bytes of the written address). Subset incl. this
 * device's cc:fb:65; extend freely -- only used to recognise a Wii Remote so we
 * inject the address-derived PIN for it and nothing else. */
static const uint8_t NINTENDO_OUI[][3] = {
    {0xcc,0xfb,0x65}, {0x00,0x1e,0x35}, {0x00,0x1f,0x32}, {0x00,0x1f,0xc5},
    {0x00,0x21,0x47}, {0x00,0x21,0xbd}, {0x00,0x22,0x4c}, {0x00,0x22,0xaa},
    {0x00,0x22,0xd7}, {0x00,0x23,0x31}, {0x00,0x23,0xcc}, {0x00,0x24,0x1e},
    {0x00,0x24,0x44}, {0x00,0x24,0xf3}, {0x00,0x25,0xa0}, {0x00,0x26,0x59},
    {0x00,0x27,0x09}, {0x34,0xaf,0x2c}, {0x40,0xd2,0x8a}, {0x58,0xbd,0xa3},
    {0x8c,0x56,0xc5}, {0x9c,0xe6,0x35}, {0xa4,0x5c,0x27}, {0xa4,0xc0,0xe1},
    {0xb8,0x8a,0xec}, {0xe0,0x0c,0x7f}, {0xe0,0xe7,0x51}, {0xe8,0x4e,0xce},
};

static int is_oui(const uint8_t *p)
{
    unsigned i;
    for (i = 0; i < sizeof(NINTENDO_OUI)/3; i++)
        if (p[0]==NINTENDO_OUI[i][0] && p[1]==NINTENDO_OUI[i][1] && p[2]==NINTENDO_OUI[i][2])
            return 1;
    return 0;
}

int wiimote_is_nintendo(const uint8_t *addr6, uint8_t written[6])
{
    uint8_t rev[6];
    int i;
    for (i = 0; i < 6; i++) rev[i] = addr6[5-i];

    if (is_oui(addr6))  { memcpy(written, addr6, 6); return 1; }  /* already written order */
    if (is_oui(rev))    { memcpy(written, rev, 6);   return 1; }  /* stored little-endian  */
    return 0;
}

void wiimote_make_pin(const uint8_t written[6], uint8_t pin_out[6])
{
    int i;
    for (i = 0; i < 6; i++) pin_out[i] = written[5-i];   /* PIN = reverse of written */
}

int wiimote_name_matches(const char *name)
{
    if (!name) return 0;
    return strstr(name, "Nintendo") != 0 || strstr(name, "RVL-CNT") != 0;
}

/* ---- uinput node ---- */
static void kbit(int fd, int code) { ioctl(fd, UI_SET_KEYBIT, code); }

int wiimote_create_uinput(const char *name, uint16_t vendor, uint16_t product)
{
    struct uinput_user_dev udev;
    int fd;

    fd = open(UINPUT_DEV_NODE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) { shim_log("wiimote: open uinput failed: %s", strerror(errno)); return -1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    kbit(fd, BTN_SOUTH); kbit(fd, BTN_EAST); kbit(fd, BTN_NORTH); kbit(fd, BTN_WEST);
    kbit(fd, BTN_START); kbit(fd, BTN_SELECT); kbit(fd, BTN_MODE);
    kbit(fd, BTN_DPAD_UP); kbit(fd, BTN_DPAD_DOWN);
    kbit(fd, BTN_DPAD_LEFT); kbit(fd, BTN_DPAD_RIGHT);

    memset(&udev, 0, sizeof(udev));
    strncpy(udev.name, (name && *name) ? name : "Nintendo Wii Remote", UINPUT_MAX_NAME_SIZE - 1);
    udev.id.bustype = BUS_BLUETOOTH;
    udev.id.vendor  = vendor ? vendor : 0x057e;   /* Nintendo */
    udev.id.product = product ? product : 0x0306;
    if (write(fd, &udev, sizeof(udev)) != (ssize_t)sizeof(udev)) {
        shim_log("wiimote: write udev failed"); close(fd); return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        shim_log("wiimote: UI_DEV_CREATE failed: %s", strerror(errno)); close(fd); return -1;
    }
    shim_log("wiimote: uinput node created ('%s')", udev.name);
    return fd;
}

/* ---- output reports: LED + reporting mode ---- */
typedef int (*setrep_fn)(int handle, int type, const void *data, int len);
static setrep_fn get_setrep(void)
{
    static setrep_fn fn = 0;
    static int tried = 0;
    if (!tried) { tried = 1; fn = (setrep_fn)dlsym(RTLD_DEFAULT, "PmBtBsaifHidhSetReport"); }
    return fn;
}

#define HID_REPORT_OUTPUT 2

void wiimote_start_reporting(int handle)
{
    setrep_fn setrep = get_setrep();
    /* LED1 on (also confirms the output pipe works); then buttons-only mode. */
    unsigned char led[2]  = { 0x11, 0x10 };
    unsigned char mode[3] = { 0x12, 0x00, 0x30 };   /* 0x30 = core buttons only */
    if (!setrep) { shim_log("wiimote: PmBtBsaifHidhSetReport not found"); return; }
    shim_log("wiimote: sending LED + reporting-mode (handle=%d)", handle);
    setrep(handle, HID_REPORT_OUTPUT, led,  sizeof(led));
    setrep(handle, HID_REPORT_OUTPUT, mode, sizeof(mode));
}

/* ---- input report decode ---- */
static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        shim_dbg("wiimote: emit failed");
}

void wiimote_decode(int fd, const unsigned char *rpt, int len)
{
    unsigned rid;
    unsigned b1, b2;
    if (len < 3) return;
    rid = rpt[0];
    /* Core-button reports all carry the 2 button bytes right after the id. */
    if (rid < 0x30 || rid > 0x3f) { shim_dbg("wiimote: non-core report 0x%02x", rid); return; }

    b1 = rpt[1];   /* dpad + plus */
    b2 = rpt[2];   /* A B 1 2 - home */

    emit(fd, EV_KEY, BTN_DPAD_LEFT,  !!(b1 & 0x01));
    emit(fd, EV_KEY, BTN_DPAD_RIGHT, !!(b1 & 0x02));
    emit(fd, EV_KEY, BTN_DPAD_DOWN,  !!(b1 & 0x04));
    emit(fd, EV_KEY, BTN_DPAD_UP,    !!(b1 & 0x08));
    emit(fd, EV_KEY, BTN_START,      !!(b1 & 0x10));   /* Plus  */
    emit(fd, EV_KEY, BTN_WEST,       !!(b2 & 0x01));   /* Two   */
    emit(fd, EV_KEY, BTN_NORTH,      !!(b2 & 0x02));   /* One   */
    emit(fd, EV_KEY, BTN_EAST,       !!(b2 & 0x04));   /* B     */
    emit(fd, EV_KEY, BTN_SOUTH,      !!(b2 & 0x08));   /* A     */
    emit(fd, EV_KEY, BTN_SELECT,     !!(b2 & 0x10));   /* Minus */
    emit(fd, EV_KEY, BTN_MODE,       !!(b2 & 0x80));   /* Home  */
    emit(fd, EV_SYN, SYN_REPORT, 0);
}
