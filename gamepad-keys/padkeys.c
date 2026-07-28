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
#include <errno.h>
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


/* ------------------------------------------------------------------ *
 * Bluetooth pad support.
 *
 * Palm's BT stack has no gamepad path: it only dispatches HID reports it
 * believes are keyboard or mouse. Patching "subClass" to 64 in /var/hid.j
 * makes it accept a gamepad's reports and run them through its *keyboard*
 * parser, injecting the result into a uinput keyboard it creates itself.
 *
 * That mangles the pad's report into a boot-keyboard report:
 *     report byte 0  -> the 8 modifier bits   (fully recoverable)
 *     report bytes 2+ -> HID usages -> keycodes (lossy: the usage->keycode
 *                        table folds many distinct byte values onto
 *                        KEY_UNKNOWN, so only some values survive)
 *
 * We grab that device exclusively (otherwise webOS acts on the raw
 * keycodes — a DS4's centred sticks read as 127/128, i.e. MUTE and
 * VOLUMEUP, so the volume jumps around constantly), reconstruct what we
 * can, and re-emit clean keys.
 * ------------------------------------------------------------------ */

/* Linux's HID usage -> keycode table (drivers/hid/hid-input.c). We invert it
   to turn observed keycodes back into the report byte that produced them. */
static const unsigned char hid_keyboard[256] = {
      0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
     50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
      4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
     27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
     65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
    105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
     72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
    191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
    115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
    122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,111,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
      0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     29, 42, 56,125, 97, 54,100,126,  0,  0,  0,  0,  0,  0,  0,  0,
    150,158,159,128,136,177,178,176,142,152,173,140,  0,  0,  0,  0
};

/* modifier keycodes, in report-byte-0 bit order */
static const int mod_keys[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

struct btpad {
    int  fd;
    unsigned char byte0;        /* reconstructed report byte 0 */
    unsigned char usage[8];     /* HID usages currently "pressed" */
    int  nusage;
    unsigned char prev[8];      /* previous set, to spot newly-appeared values */
    int  nprev;
    int  bbyte;                 /* the usage value identified as the button byte */
};

static int keycode_to_usage(int kc)
{
    int u;
    for (u = 4; u < 232; u++)          /* skip 0..3 (reserved/errors) */
        if (hid_keyboard[u] == kc) return u;
    return -1;
}

/* Is this the uinput keyboard Palm's BT stack made for a pad?
   Bus 0x0005 = BUS_BLUETOOTH. Real BT keyboards are excluded by name. */
static int is_bt_pad(int fd, char *name, int namelen)
{
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) < 0) return 0;
    if (id.bustype != 0x0005) return 0;
    name[0] = 0;
    ioctl(fd, EVIOCGNAME(namelen), name);
    if (strstr(name, "Wireless Controller")) return 1;   /* DualShock 4 */
    if (id.vendor == 0x054c) return 1;                   /* Sony */
    return 0;
}

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


static struct btpad bt[MAXDEV];
static int nbt = 0, btdump = 0;

/* Feed one key event from the BT pad's mangled keyboard stream and keep our
   reconstruction of the original report in sync. */
static int btn_prev[8];
static void btn_state(int idx, int on, int keycode)
{
    on = !!on;
    if (btn_prev[idx] == on) return;
    btn_prev[idx] = on;
    key(keycode, on);
}

static void bt_key(struct btpad *b, int code, int val)
{
    int i, u;
    for (i = 0; i < 8; i++)
        if (mod_keys[i] == code) {                 /* a bit of report byte 0 */
            if (val) b->byte0 |=  (1 << i);
            else     b->byte0 &= ~(1 << i);
            return;
        }
    u = keycode_to_usage(code);                    /* a value from bytes 2..7 */
    if (u < 0) return;
    if (val) {
        for (i = 0; i < b->nusage; i++) if (b->usage[i] == u) return;
        if (b->nusage < 8) b->usage[b->nusage++] = u;
    } else {
        for (i = 0; i < b->nusage; i++)
            if (b->usage[i] == u) {
                memmove(&b->usage[i], &b->usage[i + 1], b->nusage - i - 1);
                b->nusage--;
                return;
            }
    }
}

/* Translate the reconstructed report into clean key output.
 *
 * What survives the mangling, established by calibration against a DS4:
 *   - report byte 0  -> a full 8-bit analog axis (0..255, rests near 127)
 *   - one usage value is the DS4 *button byte*: low nibble = d-pad hat
 *     (0..7 = N,NE,E,SE,S,SW,W,NW; 8 = released), high nibble = face buttons
 *     Square 0x10, Cross 0x20, Circle 0x40, Triangle 0x80.
 *     Observed exactly: 0x08 at rest, 0x18/0x28/0x48/0x88 per face button.
 *   - other usage values are further axes, but the usage->keycode table folds
 *     many byte values onto KEY_UNKNOWN so they are not uniquely invertible.
 */
#define BTN_SQUARE   0x10
#define BTN_CROSS    0x20
#define BTN_CIRCLE   0x40
#define BTN_TRIANGLE 0x80

static void bt_apply(struct btpad *b)
{
    /* d-pad hat -> which of L,R,U,D are active */
    static const unsigned char hat_dirs[9] = {
        /*0 N */ 0x4, /*1 NE*/ 0x6, /*2 E */ 0x2, /*3 SE*/ 0xa,
        /*4 S */ 0x8, /*5 SW*/ 0x9, /*6 W */ 0x1, /*7 NW*/ 0x5,
        /*8 released*/ 0x0
    };
    int i, buttons = -1, dirs = 0;

    /* Pick out the button byte. Several usage values can *look* valid (a stick
       resting at 0x81 has a legal-looking hat nibble), so:
       1. keep tracking the byte we already identified while it is still present;
       2. otherwise prefer a value with a neutral hat nibble (0x_8);
       3. otherwise take a newly-appeared value with a legal hat nibble.
       This survives d-pad presses, which change the tracked byte's low nibble. */
    for (i = 0; i < b->nusage; i++)
        if (b->usage[i] == b->bbyte) { buttons = b->bbyte; break; }

    if (buttons < 0)
        for (i = 0; i < b->nusage; i++)
            if ((b->usage[i] & 0x0f) == 8) { buttons = b->usage[i]; break; }

    if (buttons < 0)
        for (i = 0; i < b->nusage; i++) {
            int v = b->usage[i], k, wasthere = 0;
            if ((v & 0x0f) > 8) continue;
            for (k = 0; k < b->nprev; k++) if (b->prev[k] == v) wasthere = 1;
            if (!wasthere) { buttons = v; break; }
        }

    if (buttons >= 0) b->bbyte = buttons;
    memcpy(b->prev, b->usage, sizeof(b->usage));
    b->nprev = b->nusage;

    if (btdump) {
        printf("byte0=0x%02x (%3d) usages:", b->byte0, b->byte0);
        for (i = 0; i < b->nusage; i++) printf(" 0x%02x", b->usage[i]);
        if (buttons >= 0) printf("   -> hat=%d face=%s%s%s%s",
                buttons & 0x0f,
                (buttons & BTN_SQUARE)   ? "Sq " : "",
                (buttons & BTN_CROSS)    ? "X "  : "",
                (buttons & BTN_CIRCLE)   ? "O "  : "",
                (buttons & BTN_TRIANGLE) ? "Tri" : "");
        printf("\n");
        fflush(stdout);
    }

    if (buttons >= 0) {
        int hat = buttons & 0x0f;
        if (hat <= 8) dirs = hat_dirs[hat];
        btn_state(0, buttons & BTN_CROSS,    KEY_ENTER);
        btn_state(1, buttons & BTN_CIRCLE,   KEY_ESC);
        btn_state(2, buttons & BTN_SQUARE,   KEY_X);
        btn_state(3, buttons & BTN_TRIANGLE, KEY_SPACE);
    }

    /* the recoverable analog axis also drives left/right, but only when the
       d-pad is not already asking for a direction */
    if (!(dirs & 0x3)) {
        if (b->byte0 < 64)  dirs |= 0x1;
        if (b->byte0 > 192) dirs |= 0x2;
    }
    setdir(0, !!(dirs & 0x1));
    setdir(1, !!(dirs & 0x2));
    setdir(2, !!(dirs & 0x4));
    setdir(3, !!(dirs & 0x8));
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

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--btdump")) btdump = 1;
        else if (atoi(argv[i]) > 0 || !strcmp(argv[i], "0")) secs = atoi(argv[i]);
    }

    for (i = 0; i < 16 && npad < MAXDEV; i++) {
        int fd, a;
        char name[64] = "?";
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        {
            char btname[64];
            if (is_bt_pad(fd, btname, sizeof(btname))) {
                int one = 1;
                if (ioctl(fd, EVIOCGRAB, &one) < 0)
                    fprintf(stderr, "padkeys: WARNING could not grab %s (%s) — "
                                    "webOS will still see its raw keys\n",
                            path, strerror(errno));
                memset(&bt[nbt], 0, sizeof(bt[0]));
                bt[nbt].fd = fd;
                bt[nbt].bbyte = 0x08;      /* DS4 rest value: hat released */
                fprintf(stderr, "padkeys: bluetooth pad on %s (%s) — grabbed\n",
                        path, btname);
                nbt++;
                continue;                      /* handled as a BT pad, not evdev */
            }
        }
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
    if (!npad && !nbt) { fprintf(stderr, "padkeys: no gamepad found\n"); return 1; }

    ui = open_uinput();
    if (ui < 0) return 1;
    fprintf(stderr, "padkeys: virtual keyboard created; %d usb + %d bt pad(s), %ds\n",
            npad, nbt, secs);

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
        for (i = 0; i < nbt; i++) {
            FD_SET(bt[i].fd, &rs);
            if (bt[i].fd > mx) mx = bt[i].fd;
        }
        if (select(mx + 1, &rs, 0, 0, &tv) <= 0) continue;

        for (i = 0; i < nbt; i++) {
            struct input_event ev[32];
            int n, k, changed = 0;
            if (!FD_ISSET(bt[i].fd, &rs)) continue;
            n = read(bt[i].fd, ev, sizeof(ev));
            if (n <= 0) continue;
            for (k = 0; k < n / (int)sizeof(ev[0]); k++)
                if (ev[k].type == EV_KEY) {
                    bt_key(&bt[i], ev[k].code, ev[k].value);
                    changed = 1;
                }
            if (changed) bt_apply(&bt[i]);
        }

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
