#define _GNU_SOURCE
#include "hid_parser.h"
#include "compat.h"
#include "log.h"
#include <string.h>
#include <stdio.h>

/* ---- global item state, with a small push/pop stack ---- */
struct gstate {
    uint16_t usage_page;
    int32_t  lmin, lmax;
    uint32_t report_size;
    uint32_t report_count;
    uint8_t  report_id;
};

/* per-report-id running bit offset */
struct idbits { uint8_t id; uint16_t bits; };

static uint16_t idbits_take(struct idbits *tab, int *ntab, uint8_t id, uint16_t add)
{
    int i;
    for (i = 0; i < *ntab; i++)
        if (tab[i].id == id) {
            uint16_t off = tab[i].bits;
            tab[i].bits += add;
            return off;
        }
    if (*ntab < 16) {
        tab[*ntab].id = id;
        tab[*ntab].bits = add;
        (*ntab)++;
    }
    return 0;
}

static int32_t read_signed(const uint8_t *p, int nbytes)
{
    int32_t v = 0;
    int i;
    for (i = 0; i < nbytes; i++)
        v |= (int32_t)p[i] << (8 * i);
    if (nbytes == 1 && (v & 0x80))       v |= ~0xff;
    else if (nbytes == 2 && (v & 0x8000)) v |= ~0xffff;
    return v;
}
static uint32_t read_unsigned(const uint8_t *p, int nbytes)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < nbytes; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

/* Map a Generic-Desktop axis usage to an EV_REL/EV_ABS code. */
static int map_axis(uint16_t usage, int relative, uint16_t *evtype, uint16_t *evcode)
{
    uint16_t abs, rel;
    switch (usage) {
    case HID_GD_X:  abs = ABS_X;  rel = REL_X;  break;
    case HID_GD_Y:  abs = ABS_Y;  rel = REL_Y;  break;
    case HID_GD_Z:  abs = ABS_Z;  rel = REL_Z;  break;
    case HID_GD_RX: abs = ABS_RX; rel = REL_RX; break;
    case HID_GD_RY: abs = ABS_RY; rel = REL_RY; break;
    case HID_GD_RZ: abs = ABS_RZ; rel = REL_RZ; break;
    case HID_GD_WHEEL: abs = ABS_WHEEL; rel = REL_WHEEL; break;
    case HID_GD_SLIDER: abs = ABS_THROTTLE; rel = REL_Z; break;
    case HID_GD_DIAL:   abs = ABS_RUDDER;   rel = REL_RZ; break;
    default: return -1;
    }
    if (relative) { *evtype = EV_REL; *evcode = rel; }
    else          { *evtype = EV_ABS; *evcode = abs; }
    return 0;
}

int hid_parse(const uint8_t *desc, int len, struct hid_profile *out)
{
    struct gstate g, stack[8];
    int sp = 0;
    uint16_t usages[64]; int nusages = 0;
    uint32_t usage_min = 0, usage_max = 0; int have_umin = 0, have_umax = 0;
    struct idbits idtab[16]; int nidtab = 0;
    int i = 0;
    int saw_gamepad_usage = 0;

    memset(out, 0, sizeof(*out));
    memset(&g, 0, sizeof(g));

    while (i < len) {
        uint8_t b = desc[i++];
        uint8_t bSize, bType, bTag;
        const uint8_t *data;
        int nb;

        if (b == 0xfe) {                 /* long item -- skip */
            if (i >= len) break;
            nb = desc[i++];
            i += 1 + nb;
            continue;
        }
        bSize = b & 0x03; if (bSize == 3) bSize = 4;
        bType = (b >> 2) & 0x03;
        bTag  = (b >> 4) & 0x0f;
        if (i + bSize > len) break;
        data = desc + i;
        nb = bSize;
        i += bSize;

        if (bType == 1) {                /* GLOBAL */
            switch (bTag) {
            case 0x0: g.usage_page  = (uint16_t)read_unsigned(data, nb); break; /* Usage Page */
            case 0x1: g.lmin        = read_signed(data, nb); break;             /* Logical Min */
            case 0x2: g.lmax        = read_signed(data, nb); break;             /* Logical Max */
            case 0x7: g.report_size = read_unsigned(data, nb); break;           /* Report Size */
            case 0x8: g.report_id   = (uint8_t)read_unsigned(data, nb);         /* Report ID */
                      out->uses_report_id = 1; break;
            case 0x9: g.report_count= read_unsigned(data, nb); break;           /* Report Count */
            case 0xa: if (sp < 8) stack[sp++] = g; break;                       /* Push */
            case 0xb: if (sp > 0) g = stack[--sp]; break;                       /* Pop */
            default: break;                                                     /* phys/unit ignored */
            }
        } else if (bType == 2) {         /* LOCAL */
            switch (bTag) {
            case 0x0: if (nusages < 64) usages[nusages++] = (uint16_t)read_unsigned(data, nb); break;
            case 0x1: usage_min = read_unsigned(data, nb); have_umin = 1; break;
            case 0x2: usage_max = read_unsigned(data, nb); have_umax = 1; break;
            default: break;
            }
        } else if (bType == 0) {         /* MAIN */
            if (bTag == 0xa) {           /* Collection -- sniff joystick/gamepad app usage */
                if (nusages > 0 && g.usage_page == HID_UP_GENERIC_DESKTOP &&
                    (usages[0] == HID_GD_JOYSTICK || usages[0] == HID_GD_GAMEPAD))
                    saw_gamepad_usage = 1;
            } else if (bTag == 0x8) {    /* INPUT */
                uint32_t flags = read_unsigned(data, nb);
                int is_const = flags & 0x01;
                int is_var   = flags & 0x02;
                int is_rel   = flags & 0x04;
                uint32_t total = g.report_size * g.report_count;
                uint16_t base = idbits_take(idtab, &nidtab, g.report_id, (uint16_t)total);

                if (out->report_id_hint == 0 && g.report_id)
                    out->report_id_hint = g.report_id;

                if (!is_const && is_var) {
                    uint32_t k;
                    for (k = 0; k < g.report_count; k++) {
                        struct hid_field *f;
                        uint16_t usage;
                        if (out->nfields >= HID_MAX_FIELDS) break;
                        /* resolve this control's usage */
                        if ((uint32_t)nusages > k)      usage = usages[k];
                        else if (nusages > 0)           usage = usages[nusages - 1];
                        else if (have_umin)             usage = (uint16_t)(usage_min + k);
                        else                            usage = 0;

                        f = &out->fields[out->nfields];
                        memset(f, 0, sizeof(*f));
                        f->report_id  = g.report_id;
                        f->bit_offset = base + (uint16_t)(k * g.report_size);
                        f->bit_size   = (uint8_t)g.report_size;
                        f->is_signed  = (g.lmin < 0);
                        f->lmin = g.lmin; f->lmax = g.lmax;
                        f->usage_page = g.usage_page;
                        f->usage = usage;

                        if (g.usage_page == HID_UP_GENERIC_DESKTOP && usage == HID_GD_HAT) {
                            f->is_hat = 1;
                            f->evtype = EV_ABS; f->evcode = ABS_HAT0X;
                            out->is_gamepad = 1;
                            out->nfields++;
                        } else if (g.usage_page == HID_UP_GENERIC_DESKTOP &&
                                   map_axis(usage, is_rel, &f->evtype, &f->evcode) == 0) {
                            if (f->evtype == EV_REL) out->is_mouse = 1;
                            else                     out->is_gamepad = 1;
                            out->nfields++;
                        } else if (g.usage_page == HID_UP_BUTTON) {
                            /* store raw 1-based button index in evcode for now */
                            f->evtype = EV_KEY;
                            f->evcode = (uint16_t)usage;   /* button number */
                            out->nfields++;
                        } else {
                            /* unmapped (consumer, vendor, keyboard-in-combo): skip emit */
                        }
                    }
                }
                /* array fields (keyboard keycode arrays) -- note class, don't emit */
                if (!is_var && !is_const && g.usage_page == HID_UP_KEYBOARD)
                    out->is_keyboard = 1;
            }
            /* Local items apply to exactly one main item (HID 6.2.2.8): reset
             * after EVERY main item, including Collection / End Collection. */
            nusages = 0; have_umin = have_umax = 0; usage_min = usage_max = 0;
        }
    }

    if (saw_gamepad_usage) out->is_gamepad = 1;

    /* record per-report INPUT payload sizes (idtab counted every INPUT item,
     * including const padding, so this is the exact wire payload size) */
    for (out->nreport_bits = 0;
         out->nreport_bits < nidtab && out->nreport_bits < HID_MAX_REPORT_IDS;
         out->nreport_bits++) {
        out->report_bits[out->nreport_bits].id   = idtab[out->nreport_bits].id;
        out->report_bits[out->nreport_bits].bits = idtab[out->nreport_bits].bits;
    }

    /* ---- second pass: resolve button numbers to BTN_* by device class ---- */
    {
        static const uint16_t pad_btn[15] = {
            BTN_SOUTH, BTN_EAST, BTN_C, BTN_NORTH, BTN_WEST, BTN_Z,
            BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START,
            BTN_MODE, BTN_THUMBL, BTN_THUMBR
        };
        static const uint16_t mouse_btn[5] = {
            BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA
        };
        int j;
        int as_mouse = (out->is_mouse && !out->is_gamepad);
        for (j = 0; j < out->nfields; j++) {
            struct hid_field *f = &out->fields[j];
            if (f->evtype == EV_KEY) {
                int n = (int)f->evcode;      /* 1-based button number */
                if (n < 1) n = 1;
                if (as_mouse)
                    f->evcode = (n <= 5) ? mouse_btn[n - 1] : (BTN_LEFT + (n - 1));
                else
                    f->evcode = (n <= 15) ? pad_btn[n - 1] : (BTN_TRIGGER_HAPPY + (n - 16));
            }
        }
    }
    return 0;
}

int hid_report_payload_bits(const struct hid_profile *p, uint8_t id)
{
    int i;
    for (i = 0; i < p->nreport_bits; i++)
        if (p->report_bits[i].id == id)
            return (int)p->report_bits[i].bits;
    return -1;
}

void hid_profile_describe(const struct hid_profile *p, char *buf, int buflen)
{
    snprintf(buf, buflen,
             "class=%s%s%s fields=%d report_id=%s(first=%d)",
             p->is_mouse ? "mouse " : "",
             p->is_gamepad ? "gamepad " : "",
             p->is_keyboard ? "keyboard " : "",
             p->nfields,
             p->uses_report_id ? "yes" : "no",
             p->report_id_hint);
}
