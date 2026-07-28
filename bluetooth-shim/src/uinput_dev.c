#define _GNU_SOURCE
#include "uinput_dev.h"
#include "log.h"
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>

static void set_evbit(int fd, int ev)     { ioctl(fd, UI_SET_EVBIT,  ev); }
static void set_keybit(int fd, int code)  { ioctl(fd, UI_SET_KEYBIT, code); }
static void set_relbit(int fd, int code)  { ioctl(fd, UI_SET_RELBIT, code); }
static void set_absbit(int fd, int code)  { ioctl(fd, UI_SET_ABSBIT, code); }

int uinput_create(const struct hid_profile *p, const char *name, const struct input_id *id)
{
    struct uinput_user_dev udev;
    int fd, j;
    int have_key = 0, have_rel = 0, have_abs = 0;

    fd = open(UINPUT_DEV_NODE, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        shim_log("open(%s) failed: %s", UINPUT_DEV_NODE, strerror(errno));
        return -1;
    }

    memset(&udev, 0, sizeof(udev));
    if (name && *name) strncpy(udev.name, name, UINPUT_MAX_NAME_SIZE - 1);
    else               strncpy(udev.name, "webOS BT HID", UINPUT_MAX_NAME_SIZE - 1);
    if (id) udev.id = *id;
    udev.id.bustype = BUS_BLUETOOTH;

    /* enable per-field capabilities + absmin/absmax */
    for (j = 0; j < p->nfields; j++) {
        const struct hid_field *f = &p->fields[j];
        if (f->evtype == EV_KEY) {
            if (!have_key) { set_evbit(fd, EV_KEY); have_key = 1; }
            set_keybit(fd, f->evcode);
        } else if (f->evtype == EV_REL) {
            if (!have_rel) { set_evbit(fd, EV_REL); have_rel = 1; }
            set_relbit(fd, f->evcode);
        } else if (f->evtype == EV_ABS) {
            if (!have_abs) { set_evbit(fd, EV_ABS); have_abs = 1; }
            if (f->is_hat) {
                set_absbit(fd, ABS_HAT0X);
                set_absbit(fd, ABS_HAT0Y);
                udev.absmin[ABS_HAT0X] = -1; udev.absmax[ABS_HAT0X] = 1;
                udev.absmin[ABS_HAT0Y] = -1; udev.absmax[ABS_HAT0Y] = 1;
            } else {
                set_absbit(fd, f->evcode);
                if (f->evcode < ABS_CNT) {
                    udev.absmin[f->evcode] = f->lmin;
                    udev.absmax[f->evcode] = f->lmax;
                }
            }
        }
    }

    if (write(fd, &udev, sizeof(udev)) != (ssize_t)sizeof(udev)) {
        shim_log("write(uinput_user_dev) failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        shim_log("UI_DEV_CREATE failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    shim_log("uinput node created: name='%s' vid=%04x pid=%04x key=%d rel=%d abs=%d",
             udev.name, udev.id.vendor, udev.id.product, have_key, have_rel, have_abs);
    return fd;
}

/* extract nbits at bitpos (HID little-endian bit order), optional sign-extend */
static int32_t extract(const unsigned char *buf, int buflen, int bitpos, int nbits, int is_signed)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < nbits; i++) {
        int bp = bitpos + i;
        if ((bp >> 3) >= buflen) break;
        if ((buf[bp >> 3] >> (bp & 7)) & 1)
            v |= (uint32_t)1 << i;
    }
    if (is_signed && nbits < 32 && (v & ((uint32_t)1 << (nbits - 1))))
        v |= ~(((uint32_t)1 << nbits) - 1);
    return (int32_t)v;
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));   /* kernel timestamps on write; time may be 0 */
    ev.type = type; ev.code = code; ev.value = value;
    if (write(fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev))
        shim_dbg("emit write failed t=%u c=%u", type, code);
}

/* 8-way hat: logical value (clockwise from up), null => centered */
static void hat_to_xy(const struct hid_field *f, int32_t value, int *x, int *y)
{
    int v = (int)(value - f->lmin);          /* normalize so up==0 */
    int range = (int)(f->lmax - f->lmin) + 1; /* usually 8 */
    *x = 0; *y = 0;
    if (value < f->lmin || value > f->lmax) return;   /* null state */
    if (range >= 8) {
        switch (v & 7) {
        case 0: *y = -1;            break; /* N  */
        case 1: *y = -1; *x =  1;   break; /* NE */
        case 2: *x =  1;            break; /* E  */
        case 3: *x =  1; *y =  1;   break; /* SE */
        case 4: *y =  1;            break; /* S  */
        case 5: *x = -1; *y =  1;   break; /* SW */
        case 6: *x = -1;            break; /* W  */
        case 7: *x = -1; *y = -1;   break; /* NW */
        }
    } else if (range == 4) {                 /* 4-way */
        switch (v & 3) {
        case 0: *y = -1; break;
        case 1: *x =  1; break;
        case 2: *y =  1; break;
        case 3: *x = -1; break;
        }
    }
}

void uinput_emit_report(int fd, const struct hid_profile *p,
                        const unsigned char *report, int rlen)
{
    int payload_bit0 = 0;
    uint8_t rid = 0;
    int j, emitted = 0;

    /* Framing: the stack may hand us [id, payload] or the whole HIDP frame
     * [0xA1 DATA|INPUT, id, payload] -- the stock boot-keyboard parser reads
     * modifiers at report[2] / keys at report[4], which implies the latter.
     * Find a known input-report id whose descriptor-declared payload fits the
     * remaining bytes; prefer the smallest skip. */
    if (p->uses_report_id) {
        int skip, found = -1;
        for (skip = 0; skip <= 2 && skip < rlen; skip++) {
            int bits = hid_report_payload_bits(p, report[skip]);
            if (bits >= 0 && (rlen - skip - 1) * 8 >= bits) { found = skip; break; }
        }
        if (found < 0) {
            shim_dbg("emit: no known input report id in frame (b0=0x%02x len=%d)",
                     rlen ? report[0] : 0, rlen);
            return;
        }
        rid = report[found];
        payload_bit0 = (found + 1) * 8;
    } else {
        /* no report ids: strip a leading HIDP header byte if the payload
         * otherwise doesn't fit the descriptor-declared size */
        int bits = hid_report_payload_bits(p, 0);
        if (bits >= 0 && rlen > 1 && report[0] == 0xA1 &&
            rlen * 8 > bits && (rlen - 1) * 8 >= bits)
            payload_bit0 = 8;
    }

    for (j = 0; j < p->nfields; j++) {
        const struct hid_field *f = &p->fields[j];
        int bitpos;
        int32_t val;

        if (p->uses_report_id && f->report_id != rid) continue;
        bitpos = payload_bit0 + f->bit_offset;
        val = extract(report, rlen, bitpos, f->bit_size, f->is_signed);

        if (f->evtype == EV_KEY) {
            emit(fd, EV_KEY, f->evcode, val ? 1 : 0);
            emitted++;
        } else if (f->evtype == EV_REL) {
            if (val != 0) { emit(fd, EV_REL, f->evcode, val); emitted++; }
        } else if (f->evtype == EV_ABS) {
            if (f->is_hat) {
                int x, y;
                hat_to_xy(f, val, &x, &y);
                emit(fd, EV_ABS, ABS_HAT0X, x);
                emit(fd, EV_ABS, ABS_HAT0Y, y);
                emitted += 2;
            } else {
                emit(fd, EV_ABS, f->evcode, val);
                emitted++;
            }
        }
    }

    if (emitted)
        emit(fd, EV_SYN, SYN_REPORT, 0);
}

void uinput_destroy(int fd)
{
    if (fd < 0) return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
