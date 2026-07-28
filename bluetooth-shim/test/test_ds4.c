/*
 * test_ds4.c -- feed a real DualShock 4 (report-1 "simple mode") descriptor
 * through the parser + emitter, in every framing the CSR stack might use:
 *
 *   A) buffer = [0x01, payload...]        -- bare report with id
 *   B) buffer = [0xA1, 0x01, payload...]  -- full HIDP DATA|INPUT frame; this
 *      is what the stock keyboard parser's offsets imply the stack delivers
 *      (modifier byte read at report[2], keycodes at report[4])
 *   C) buffer = full-mode report 0x11     -- must be silence, not garbage
 *   D) no-report-id mouse with an HIDP header byte
 */
#define _GNU_SOURCE
#include "hid_parser.h"
#include "uinput_dev.h"
#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok   : %s\n", msg); \
    else    { printf("  FAIL : %s\n", msg); failures++; } } while (0)

/* DS4 (054c:05c4) report-1 input fragment -- same layout on USB and BT.
 * Payload after the ID byte: LX LY RX RY | hat(4b)+14 buttons+counter(6b) | L2 R2  = 9 bytes */
static const unsigned char DESC_DS4[] = {
    0x05,0x01,       /* Usage Page (Generic Desktop) */
    0x09,0x05,       /* Usage (Gamepad)              */
    0xA1,0x01,       /* Collection (Application)     */
    0x85,0x01,       /*   Report ID (1)              */
    0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35,  /* X Y Z Rz  (LX LY RX RY) */
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x04, 0x81,0x02,
    0x09,0x39,       /*   Hat switch */
    0x15,0x00, 0x25,0x07, 0x35,0x00, 0x46,0x3B,0x01, 0x65,0x14,
    0x75,0x04, 0x95,0x01, 0x81,0x42,             /* 4-bit, null state */
    0x65,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x0E,             /* buttons 1..14 */
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x0E, 0x81,0x02,
    0x06,0x00,0xFF, 0x09,0x20,                   /* vendor 6-bit counter */
    0x75,0x06, 0x95,0x01, 0x15,0x00, 0x25,0x7F, 0x81,0x02,
    0x05,0x01, 0x09,0x33, 0x09,0x34,             /* Rx Ry (L2 R2 analog) */
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0xC0
};

/* logical state: LX=0x80 LY=0x80 RX=0x40 RY=0xC0, hat=E(2), Cross(btn2) held,
 * counter=0, L2=0x00 R2=0xFF */
static const unsigned char PAYLOAD[9] = {
    0x80, 0x80, 0x40, 0xC0,
    0x22,             /* hat=2 in low nibble, btn2 = bit 5 */
    0x00, 0x00,
    0x00, 0xFF
};

/* boot-style mouse, no report ids: 3 buttons + 5 pad + rel X/Y */
static const unsigned char DESC_MOUSE[] = {
    0x05,0x01, 0x09,0x02, 0xA1,0x01,
      0x09,0x01, 0xA1,0x00,
        0x05,0x09, 0x19,0x01, 0x29,0x03,
        0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x03, 0x81,0x02,
        0x75,0x05, 0x95,0x01, 0x81,0x03,
        0x05,0x01, 0x09,0x30, 0x09,0x31,
        0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x02, 0x81,0x06,
      0xC0,
    0xC0
};

static int emit_and_collect(const struct hid_profile *p, const unsigned char *rep, int rlen,
                            struct input_event *out, int maxout)
{
    char tmpl[] = "/tmp/btshim_ds4XXXXXX";
    int fd = mkstemp(tmpl);
    int n = 0;
    struct input_event ev;
    unlink(tmpl);
    uinput_emit_report(fd, p, rep, rlen);
    lseek(fd, 0, SEEK_SET);
    while (n < maxout && read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev))
        out[n++] = ev;
    close(fd);
    return n;
}

static int find_ev(struct input_event *ev, int n, int type, int code, int *val)
{
    int i;
    for (i = 0; i < n; i++)
        if (ev[i].type == type && ev[i].code == code) { if (val) *val = ev[i].value; return 1; }
    return 0;
}

static void check_ds4_events(struct input_event *ev, int n, const char *label)
{
    int v;
    char msg[96];
#define C(cond, what) do { snprintf(msg, sizeof(msg), "%s: %s", label, what); CHECK(cond, msg); } while (0)
    C(find_ev(ev, n, EV_ABS, ABS_X, &v)  && v == 0x80, "ABS_X (LX) = 0x80");
    C(find_ev(ev, n, EV_ABS, ABS_Z, &v)  && v == 0x40, "ABS_Z (RX) = 0x40");
    C(find_ev(ev, n, EV_ABS, ABS_RZ, &v) && v == 0xC0, "ABS_RZ (RY) = 0xC0");
    C(find_ev(ev, n, EV_ABS, ABS_HAT0X, &v) && v == 1, "hat East -> HAT0X=+1");
    C(find_ev(ev, n, EV_KEY, BTN_EAST, &v) && v == 1, "btn2 (Cross) pressed");
    C(find_ev(ev, n, EV_ABS, ABS_RY, &v) && v == 0xFF, "ABS_RY (R2) = 255");
    C(find_ev(ev, n, EV_SYN, SYN_REPORT, &v), "SYN_REPORT");
#undef C
}

int main(void)
{
    struct hid_profile p;
    struct input_event ev[64];
    char descr[128];
    int n, v;

    printf("== DS4 report-1 descriptor ==\n");
    CHECK(hid_parse(DESC_DS4, sizeof(DESC_DS4), &p) == 0, "parses");
    hid_profile_describe(&p, descr, sizeof(descr));
    printf("  %s\n", descr);
    CHECK(p.is_gamepad, "classified as gamepad");
    CHECK(p.uses_report_id, "uses report ids");
    CHECK(p.nfields == 21, "21 mapped fields (4 sticks + hat + 14 btns + 2 triggers)");
    CHECK(hid_report_payload_bits(&p, 1) == 72, "report 1 payload = 72 bits");

    printf("== A: [id, payload] ==\n");
    {
        unsigned char rep[10];
        rep[0] = 0x01; memcpy(rep + 1, PAYLOAD, 9);
        n = emit_and_collect(&p, rep, sizeof(rep), ev, 64);
        check_ds4_events(ev, n, "A");
    }

    printf("== B: [0xA1, id, payload]  (HIDP frame) ==\n");
    {
        unsigned char rep[11];
        rep[0] = 0xA1; rep[1] = 0x01; memcpy(rep + 2, PAYLOAD, 9);
        n = emit_and_collect(&p, rep, sizeof(rep), ev, 64);
        check_ds4_events(ev, n, "B");
    }

    printf("== C: unknown full-mode report 0x11 -> silence ==\n");
    {
        unsigned char rep[13];
        memset(rep, 0x55, sizeof(rep));
        rep[0] = 0xA1; rep[1] = 0x11;
        n = emit_and_collect(&p, rep, sizeof(rep), ev, 64);
        CHECK(n == 0, "no events for undeclared report id");
    }

    printf("== D: no-report-id mouse with HIDP header ==\n");
    {
        struct hid_profile mp;
        /* buttons=0x01 (left), dx=+5, dy=-5 */
        unsigned char bare[3] = { 0x01, 0x05, 0xFB };
        unsigned char framed[4] = { 0xA1, 0x01, 0x05, 0xFB };
        CHECK(hid_parse(DESC_MOUSE, sizeof(DESC_MOUSE), &mp) == 0, "mouse parses");
        CHECK(hid_report_payload_bits(&mp, 0) == 24, "mouse payload = 24 bits");
        n = emit_and_collect(&mp, bare, sizeof(bare), ev, 64);
        CHECK(find_ev(ev, n, EV_REL, REL_X, &v) && v == 5, "bare: REL_X = +5");
        n = emit_and_collect(&mp, framed, sizeof(framed), ev, 64);
        CHECK(find_ev(ev, n, EV_KEY, BTN_LEFT, &v) && v == 1, "framed: BTN_LEFT pressed");
        CHECK(find_ev(ev, n, EV_REL, REL_Y, &v) && v == -5, "framed: REL_Y = -5");
    }

    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
