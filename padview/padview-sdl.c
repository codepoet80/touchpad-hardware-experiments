/*
 * padview-sdl — live game-controller visualizer for webOS (HP TouchPad)
 *
 * The earlier padview drew straight to /dev/fb0, which fights LunaSysMgr for
 * the screen: the TouchPad has a 3-layer compositor (background / application /
 * UI) and anything bypassing it flickers continuously. SDL is the only context
 * owner that integrates correctly with that compositor, so this version renders
 * through SDL instead and sits still.
 *
 * Run it as root straight from a novacom shell (NOT from the launcher) — the
 * launcher jails PDK apps as uid 5003, which cannot open /dev/input/event*.
 *
 * Build (see build-sdl.sh):
 *   arm-linux-gnueabi-gcc -O2 -mcpu=cortex-a8 -mfloat-abi=softfp \
 *     -I/opt/PalmPDK/include -I/opt/PalmPDK/include/SDL \
 *     -L/opt/PalmPDK/device/lib -lSDL -lpdl -o padview-sdl padview-sdl.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/input.h>
#include <SDL.h>
#include <PDL.h>

#define SCRW 1024
#define SCRH 768

/* ---- Bluetooth pad, via Palm's mangled keyboard stream (see README) ------
 * Palm's BT stack runs a gamepad's HID reports through its *keyboard* parser.
 * report byte 0   -> the 8 modifier bits            (full 8-bit value)
 * report bytes 2+ -> HID usages -> keycodes         (partly recoverable)
 * One usage value is the DS4 button byte: low nibble = d-pad hat (8 = released),
 * high nibble = Square 0x10 / Cross 0x20 / Circle 0x40 / Triangle 0x80.
 */
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
static const int mod_keys[8] = { 29, 42, 56, 125, 97, 54, 100, 126 };

static int  btfd = -1;
static unsigned char bt_byte0 = 127;
static int  bt_bbyte = 0x08;

/* Mouse-mode state. With subClass=128 Palm's stack runs the pad's reports
   through its *mouse* parser instead: report byte 0 -> up to 3 button bits,
   bytes 1 and 2 -> X and Y, passed through as full 8-bit values with none of
   the usage->keycode mangling the keyboard path suffers. webOS has no mouse
   support at all, so these events are invisible to the UI - no volume chaos. */
static int bt_relx, bt_rely, bt_mbtn[3];
static int bt_saw_rel = 0;

/* We patch the library's own usage->keycode table on the device (see README):
   the DS4 button-byte values are remapped onto F13..F24 + spares, which webOS
   ignores completely, and the noisy stick-centre values are silenced. That kills
   the volume/launcher chaos at the source AND makes the decode unambiguous —
   no more guessing which usage value is the button byte. */
static int keycode_to_usage(int kc)
{
    static const struct { int kc, usage; } patched[] = {
        {183,0x00},{184,0x01},{185,0x02},{186,0x03},{187,0x04},
        {188,0x05},{189,0x06},{190,0x07},{191,0x08},          /* d-pad hat   */
        {192,0x18},{193,0x28},{194,0x48},{195,0x88},          /* face buttons*/
    };
    int i, u;
    if (kc == 199) return -1;          /* our "quiet" code: deliberate noise */
    for (i = 0; i < (int)(sizeof(patched)/sizeof(patched[0])); i++)
        if (patched[i].kc == kc) return patched[i].usage;
    for (u = 4; u < 232; u++) if (hid_keyboard[u] == kc) return u;
    return -1;
}

static void bt_feed(int code, int val)
{
    int i, u;
    for (i = 0; i < 8; i++)
        if (mod_keys[i] == code) {
            if (val) bt_byte0 |=  (1 << i);
            else     bt_byte0 &= ~(1 << i);
            return;
        }
    u = keycode_to_usage(code);
    if (u < 0) return;
    /* Identify the DS4 button byte among the surviving usage values.
     * At rest, and whenever the d-pad is released, its low nibble is exactly 8
     * (hat = released), so face buttons appear as 0x18/0x28/0x48/0x88. Merely
     * requiring "low nibble <= 8" let an analog axis drifting through 0x81 pose
     * as the button byte, which is why buttons only worked intermittently.
     * A d-pad press drops the nibble below 8 but leaves the face bits alone,
     * so accept that only as a delta from the byte we are already tracking. */
    if (val) {
        if ((u & 0x0f) == 8)                                   /* hat released */
            bt_bbyte = u;
        else if ((u & 0x0f) < 8 && (u & 0xf0) == (bt_bbyte & 0xf0))
            bt_bbyte = u;                                      /* hat pressed  */
    } else if (u == bt_bbyte) {
        /* Releasing the tracked value means the button byte changed. Fall back
           to fully-neutral rather than trying to preserve the face bits:
           (u & 0xf0) | 0x08 returns the *same* value for a face button, which
           left it latched on forever. If a new value is genuinely still held,
           its own press event arrives immediately and re-sets this. */
        bt_bbyte = 0x08;
    }
}

static int bt_open(void)
{
    char path[32], name[64];
    struct input_id id;
    int fd, i, one = 1;
    for (i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGID, &id) == 0 && id.bustype == 0x0005) {
            name[0] = 0;
            ioctl(fd, EVIOCGNAME(sizeof(name)), name);
            if (strstr(name, "Wireless Controller") || id.vendor == 0x054c) {
                if (ioctl(fd, EVIOCGRAB, &one) < 0)
                    fprintf(stderr, "grab FAILED on %s: %s\n", path, strerror(errno));
                else
                    fprintf(stderr, "bluetooth pad on %s (%s) - grabbed\n", path, name);
                return fd;
            }
        }
        close(fd);
    }
    return -1;
}

/* ------------------------------- drawing ------------------------------- */
static SDL_Surface *scr;

static void box(int x, int y, int w, int h, Uint32 c)
{
    SDL_Rect r; r.x = x; r.y = y; r.w = w; r.h = h;
    SDL_FillRect(scr, &r, c);
}

static void outline(int x, int y, int w, int h, int t, Uint32 c)
{
    box(x, y, w, t, c); box(x, y + h - t, w, t, c);
    box(x, y, t, h, c); box(x + w - t, y, t, h, c);
}

int main(int argc, char **argv)
{
    Uint32 BG, BOX, DOT, ON, OFF, DP;
    int running = 1, secs = 0;
    time_t end;

    if (argc > 1) secs = atoi(argv[1]);

    PDL_Init(0);                       /* must precede SDL (see pdk.md) */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    scr = SDL_SetVideoMode(SCRW, SCRH, 0, SDL_SWSURFACE | SDL_FULLSCREEN);
    if (!scr) { fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError()); return 1; }
    SDL_ShowCursor(SDL_DISABLE);

    BG  = SDL_MapRGB(scr->format, 0x10, 0x10, 0x18);
    BOX = SDL_MapRGB(scr->format, 0x40, 0x40, 0x58);
    DOT = SDL_MapRGB(scr->format, 0x40, 0xC0, 0xFF);
    ON  = SDL_MapRGB(scr->format, 0xFF, 0xB0, 0x20);
    OFF = SDL_MapRGB(scr->format, 0x30, 0x30, 0x40);
    DP  = SDL_MapRGB(scr->format, 0xFF, 0x50, 0x60);

    btfd = bt_open();
    if (btfd < 0) fprintf(stderr, "no bluetooth pad found (press PS?)\n");

    end = time(NULL) + (secs ? secs : 100000);
    while (running && time(NULL) < end) {
        SDL_Event ev;
        fd_set rs;
        struct timeval tv = { 0, 16000 };
        static const signed char hatx[9] = { 0, 1, 1, 1, 0,-1,-1,-1, 0 };
        static const signed char haty[9] = {-1,-1, 0, 1, 1, 1, 0,-1, 0 };
        int hat, b;

        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT ||
                (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE))
                running = 0;

        if (btfd >= 0) {
            FD_ZERO(&rs); FD_SET(btfd, &rs);
            if (select(btfd + 1, &rs, 0, 0, &tv) > 0) {
                struct input_event iev[32];
                int n = read(btfd, iev, sizeof(iev)), k;
                if (n <= 0 && errno != EAGAIN && errno != EINTR) {
                    /* The pad slept and reconnected: Palm's stack tears down the
                       uinput device and makes a new one, so our grab died with
                       it and webOS starts seeing the raw keys again. Drop the
                       stale fd and re-acquire below. */
                    fprintf(stderr, "pad went away (%s) - will re-grab\n",
                            strerror(errno));
                    close(btfd); btfd = -1;
                }
                for (k = 0; k < n / (int)sizeof(iev[0]); k++) {
                    if (iev[k].type == EV_KEY) {
                        int c = iev[k].code;
                        if (c == BTN_LEFT)        bt_mbtn[0] = iev[k].value;
                        else if (c == BTN_RIGHT)  bt_mbtn[1] = iev[k].value;
                        else if (c == BTN_MIDDLE) bt_mbtn[2] = iev[k].value;
                        else bt_feed(c, iev[k].value);
                    } else if (iev[k].type == EV_REL) {
                        bt_saw_rel = 1;
                        if (iev[k].code == REL_X) {
                            bt_relx += iev[k].value;
                            if (bt_relx < 0) bt_relx = 0;
                            if (bt_relx > 255) bt_relx = 255;
                        } else if (iev[k].code == REL_Y) {
                            bt_rely += iev[k].value;
                            if (bt_rely < 0) bt_rely = 0;
                            if (bt_rely > 255) bt_rely = 255;
                        }
                    }
                }
            }
        } else {
            static time_t last_try;
            SDL_Delay(16);
            if (time(NULL) != last_try) {        /* retry about once a second */
                last_try = time(NULL);
                btfd = bt_open();
                if (btfd >= 0) { bt_byte0 = 127; bt_bbyte = 0x08; }
            }
        }

        if (btfd >= 0) {                      /* cheap liveness check */
            static time_t last_chk;
            if (time(NULL) != last_chk) {
                struct input_id id;
                last_chk = time(NULL);
                if (ioctl(btfd, EVIOCGID, &id) < 0) {
                    fprintf(stderr, "pad node is stale - re-grabbing\n");
                    close(btfd); btfd = -1;
                }
            }
        }

        b = bt_bbyte;
        hat = b & 0x0f; if (hat > 8) hat = 8;

        SDL_FillRect(scr, NULL, BG);

        /* the one fully-recoverable analog axis, as a dot in a box */
        outline(80, 120, 320, 320, 4, BOX);
        box(80 + (bt_byte0 * (320 - 24)) / 255, 260, 24, 24, DOT);

        /* d-pad */
        {
            int cx = 240, cy = 560, a = 44;
            box(cx - a/2, cy - a - a/2, a, a, haty[hat] < 0 ? DP : OFF);  /* up */
            box(cx - a/2, cy + a/2,     a, a, haty[hat] > 0 ? DP : OFF);  /* down */
            box(cx - a - a/2, cy - a/2, a, a, hatx[hat] < 0 ? DP : OFF);  /* left */
            box(cx + a/2, cy - a/2,     a, a, hatx[hat] > 0 ? DP : OFF);  /* right */
        }

        /* face buttons: Square, Cross, Circle, Triangle */
        {
            int i, bx = 620, by = 300;
            static const int mask[4] = { 0x10, 0x20, 0x40, 0x80 };
            for (i = 0; i < 4; i++) {
                outline(bx + i * 90, by, 70, 70, 3, BOX);
                box(bx + i * 90 + 6, by + 6, 58, 58, (b & mask[i]) ? ON : OFF);
            }
        }

        /* mouse-mode: two clean axes as a second dot, plus its 3 button bits */
        if (bt_saw_rel) {
            outline(80, 120, 320, 320, 4, DOT);          /* mark box as live */
            box(80 + (bt_relx * (320 - 24)) / 255,
                120 + (bt_rely * (320 - 24)) / 255, 24, 24, ON);
            {
                int i;
                for (i = 0; i < 3; i++)
                    box(120 + i * 60, 660, 48, 48, bt_mbtn[i] ? ON : OFF);
            }
        }

        /* raw button byte, as 8 bits, for debugging the decode */
        {
            int i;
            for (i = 0; i < 8; i++)
                box(620 + i * 40, 560, 32, 32, (b & (1 << (7 - i))) ? ON : OFF);
        }

        SDL_Flip(scr);
    }

    if (btfd >= 0) close(btfd);
    SDL_Quit();
    PDL_Quit();
    return 0;
}
