/*
 * test_parser.c -- host-side unit test for the HID parser + emitter.
 * Builds with the system compiler (x86) -- pure logic, no device needed.
 *   make host-test
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

static const char *evtype_name(int t)
{
    switch (t) { case EV_KEY:return"KEY"; case EV_REL:return"REL";
                 case EV_ABS:return"ABS"; case EV_SYN:return"SYN"; }
    return "?";
}

/* run emit into a temp file, read the input_events back */
static int emit_and_collect(const struct hid_profile *p, const unsigned char *rep, int rlen,
                            struct input_event *out, int maxout)
{
    char tmpl[] = "/tmp/btshim_evXXXXXX";
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

static void dump_events(struct input_event *ev, int n)
{
    int i;
    for (i = 0; i < n; i++)
        printf("     %s code=0x%03x value=%d\n", evtype_name(ev[i].type), ev[i].code, ev[i].value);
}

/* ---- descriptor: gamepad, 2x 8-bit abs axes + 8 buttons ---- */
static const unsigned char DESC_GAMEPAD[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01,
      0x05,0x01, 0x09,0x30, 0x09,0x31,
      0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x02, 0x81,0x02,
      0x05,0x09, 0x19,0x01, 0x29,0x08,
      0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0xC0
};

/* ---- descriptor: boot mouse, 3 buttons + rel X/Y ---- */
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

int main(void)
{
    struct hid_profile p;
    struct input_event ev[64];
    char descr[128];
    int n, v;

    printf("== gamepad descriptor ==\n");
    hid_parse(DESC_GAMEPAD, sizeof(DESC_GAMEPAD), &p);
    hid_profile_describe(&p, descr, sizeof(descr));
    printf("  %s\n", descr);
    CHECK(p.is_gamepad, "classified as gamepad");
    CHECK(!p.is_mouse, "not a mouse");
    CHECK(p.nfields == 10, "10 fields (2 axes + 8 buttons)");
    {
        /* report: X=0x80, Y=0x40, buttons=0b00000101 (btn1 + btn3) */
        unsigned char rep[3] = { 0x80, 0x40, 0x05 };
        n = emit_and_collect(&p, rep, sizeof(rep), ev, 64);
        dump_events(ev, n);
        CHECK(find_ev(ev, n, EV_ABS, ABS_X, &v) && v == 0x80, "ABS_X = 0x80");
        CHECK(find_ev(ev, n, EV_ABS, ABS_Y, &v) && v == 0x40, "ABS_Y = 0x40");
        CHECK(find_ev(ev, n, EV_KEY, BTN_SOUTH, &v) && v == 1, "BTN_SOUTH (btn1) pressed");
        CHECK(find_ev(ev, n, EV_KEY, BTN_EAST,  &v) && v == 0, "BTN_EAST (btn2) released");
        CHECK(find_ev(ev, n, EV_KEY, BTN_C,     &v) && v == 1, "BTN_C (btn3) pressed");
        CHECK(find_ev(ev, n, EV_SYN, SYN_REPORT, &v), "SYN_REPORT emitted");
    }

    printf("== mouse descriptor ==\n");
    hid_parse(DESC_MOUSE, sizeof(DESC_MOUSE), &p);
    hid_profile_describe(&p, descr, sizeof(descr));
    printf("  %s\n", descr);
    CHECK(p.is_mouse, "classified as mouse");
    CHECK(!p.is_gamepad, "not a gamepad");
    {
        /* report: buttons=0x01 (left), dx=+5, dy=-5 */
        unsigned char rep[3] = { 0x01, 0x05, 0xFB };
        n = emit_and_collect(&p, rep, sizeof(rep), ev, 64);
        dump_events(ev, n);
        CHECK(find_ev(ev, n, EV_KEY, BTN_LEFT, &v) && v == 1, "BTN_LEFT pressed");
        CHECK(find_ev(ev, n, EV_REL, REL_X, &v) && v == 5,  "REL_X = +5");
        CHECK(find_ev(ev, n, EV_REL, REL_Y, &v) && v == -5, "REL_Y = -5 (signed)");
    }

    printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
    return failures ? 1 : 0;
}
