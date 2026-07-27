/*
 * padkeys — game controller to keyboard shim for webOS (HP TouchPad)
 *
 * webOS consumes keyboards but ignores gamepads: hidd's HidInputDev plugin
 * inotify-watches /dev/input and forwards events to LunaSysMgr, but a pad
 * emits BTN_ and ABS_ codes the system has no meaning for. padkeys reads the
 * pad, translates to KEY_* codes, and injects them through /dev/input/uinput
 * as a virtual keyboard — which hidd then picks up like any real one, so
 * ordinary apps and games receive the input with no changes.
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -march=armv7-a -o padkeys padkeys.c
 * Run:   ./padkeys [seconds]      (0 = run until killed; default 300)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define MAXDEV 8

/* classic joystick button range (Logitech Precision et al) */
static const int map_120[16] = {
    KEY_ENTER, KEY_SPACE, KEY_Z, KEY_X,          /* 0x120..0x123 */
    KEY_A, KEY_S, KEY_Q, KEY_W,                  /* 0x124..0x127 */
    KEY_TAB, KEY_ESC, KEY_LEFTSHIFT, KEY_LEFTCTRL,
    KEY_1, KEY_2, KEY_3, KEY_4
};
/* gamepad button range (DualShock 4 via generic HID) */
static const int map_130[16] = {
    KEY_X,      /* 0x130 Square   */ KEY_ENTER,  /* 0x131 Cross    */
    KEY_ESC,    /* 0x132 Circle   */ KEY_SPACE,  /* 0x133 Triangle */
    KEY_A,      /* 0x134 L1       */ KEY_S,      /* 0x135 R1       */
    KEY_Q,      /* 0x136 L2       */ KEY_W,      /* 0x137 R2       */
    KEY_TAB,    /* 0x138 Share    */ KEY_ENTER,  /* 0x139 Options  */
    KEY_LEFTSHIFT, KEY_LEFTCTRL,                 /* 0x13a/b L3/R3  */
    KEY_ESC,    /* 0x13c PS       */ KEY_BACKSPACE,
    KEY_5, KEY_6
};
static const int dirkeys[4] = { KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN };

struct pad {
    int fd;
    int lo[4], hi[4];       /* ABS_X, ABS_Y, HAT0X, HAT0Y ranges */
    int have[4];
};

static int ui;
static int dirstate[4];     /* current virtual arrow-key state */

static void emit(int type, int code, int val)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type; ev.code = code; ev.value = val;
    if (write(ui, &ev, sizeof(ev)) != sizeof(ev))
        perror("padkeys: uinput write");
}

static void key(int code, int val)
{
    if (!code) return;
    emit(EV_KEY, code, val);
    emit(EV_SYN, SYN_REPORT, 0);
}

static void setdir(int idx, int on)   /* idx: 0=L 1=R 2=U 3=D */
{
    if (dirstate[idx] == on) return;
    dirstate[idx] = on;
    key(dirkeys[idx], on);
}

/* map an analog/hat axis to a pair of direction keys */
static void axis_to_dirs(int v, int lo, int hi, int negidx, int posidx)
{
    int mid = (lo + hi) / 2, dead = (hi - lo) / 4;
    if (dead < 1) dead = 1;
    setdir(negidx, v < mid - dead);
    setdir(posidx, v > mid + dead);
}

static int open_uinput(void)
{
    struct uinput_user_dev ud;
    int fd, i;

    fd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("padkeys: uinput"); return -1; }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REP);          /* keyboards advertise autorepeat */
    for (i = 0; i < 16; i++) {
        ioctl(fd, UI_SET_KEYBIT, map_120[i]);
        ioctl(fd, UI_SET_KEYBIT, map_130[i]);
    }
    for (i = 0; i < 4; i++) ioctl(fd, UI_SET_KEYBIT, dirkeys[i]);

    memset(&ud, 0, sizeof(ud));
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "padkeys Virtual Keyboard");
    ud.id.bustype = BUS_VIRTUAL;
    ud.id.vendor = 0x1209; ud.id.product = 0x0AD0; ud.id.version = 1;
    if (write(fd, &ud, sizeof(ud)) != sizeof(ud)) {
        perror("padkeys: uinput dev write"); close(fd); return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("padkeys: UI_DEV_CREATE"); close(fd); return -1;
    }
    return fd;
}

static int is_pad(int fd)
{
    unsigned long kb[(KEY_MAX + 1) / (8 * sizeof(long)) + 1];
    memset(kb, 0, sizeof(kb));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(kb)), kb) < 0) return 0;
    #define HAS(k) (kb[(k) / (8 * sizeof(long))] >> ((k) % (8 * sizeof(long))) & 1)
    return HAS(0x120) || HAS(0x130);
}

int main(int argc, char **argv)
{
    static const int absmap[4] = { ABS_X, ABS_Y, ABS_HAT0X, ABS_HAT0Y };
    struct pad pads[MAXDEV];
    int npad = 0, i, secs = 300;
    time_t end;
    char path[32];

    if (argc > 1) secs = atoi(argv[1]);

    for (i = 0; i < 16 && npad < MAXDEV; i++) {
        int fd, a;
        char name[64] = "?";
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        if (!is_pad(fd)) { close(fd); continue; }
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        memset(&pads[npad], 0, sizeof(pads[0]));
        pads[npad].fd = fd;
        for (a = 0; a < 4; a++) {
            struct input_absinfo ai;
            if (ioctl(fd, EVIOCGABS(absmap[a]), &ai) == 0 && ai.maximum > ai.minimum) {
                pads[npad].lo[a] = ai.minimum;
                pads[npad].hi[a] = ai.maximum;
                pads[npad].have[a] = 1;
            }
        }
        fprintf(stderr, "padkeys: reading %s (%s)\n", path, name);
        npad++;
    }
    if (!npad) { fprintf(stderr, "padkeys: no gamepad found\n"); return 1; }

    ui = open_uinput();
    if (ui < 0) return 1;
    fprintf(stderr, "padkeys: virtual keyboard created; %d pad(s), %ds\n",
            npad, secs);

    end = time(NULL) + secs;
    while (secs == 0 || time(NULL) < end) {
        fd_set rs;
        struct timeval tv = { 1, 0 };
        int mx = 0;
        FD_ZERO(&rs);
        for (i = 0; i < npad; i++) {
            FD_SET(pads[i].fd, &rs);
            if (pads[i].fd > mx) mx = pads[i].fd;
        }
        if (select(mx + 1, &rs, 0, 0, &tv) <= 0) continue;

        for (i = 0; i < npad; i++) {
            struct input_event ev[32];
            int n, k;
            if (!FD_ISSET(pads[i].fd, &rs)) continue;
            n = read(pads[i].fd, ev, sizeof(ev));
            if (n <= 0) continue;
            for (k = 0; k < n / (int)sizeof(ev[0]); k++) {
                int c = ev[k].code, v = ev[k].value;
                if (ev[k].type == EV_KEY) {
                    if (c >= 0x120 && c < 0x130) key(map_120[c - 0x120], v);
                    else if (c >= 0x130 && c < 0x140) key(map_130[c - 0x130], v);
                } else if (ev[k].type == EV_ABS) {
                    if (c == ABS_X && pads[i].have[0])
                        axis_to_dirs(v, pads[i].lo[0], pads[i].hi[0], 0, 1);
                    else if (c == ABS_Y && pads[i].have[1])
                        axis_to_dirs(v, pads[i].lo[1], pads[i].hi[1], 2, 3);
                    else if (c == ABS_HAT0X && pads[i].have[2])
                        axis_to_dirs(v, pads[i].lo[2], pads[i].hi[2], 0, 1);
                    else if (c == ABS_HAT0Y && pads[i].have[3])
                        axis_to_dirs(v, pads[i].lo[3], pads[i].hi[3], 2, 3);
                }
            }
        }
    }
    ioctl(ui, UI_DEV_DESTROY);
    close(ui);
    return 0;
}
