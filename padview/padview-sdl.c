/*
 * padview-sdl — live game-controller visualizer for webOS (HP TouchPad)
 *
 * 2026-07-27: rewritten for the webos-bt-shim era. The Bluetooth pad is now a
 * REAL gamepad evdev node ("Wireless Controller", BUS_BLUETOOTH, 054c:05c4):
 * BTN_SOUTH..0x13d buttons, ABS_X/Y (left stick), ABS_Z/ABS_RZ (right stick),
 * ABS_RX/ABS_RY (analog triggers), ABS_HAT0X/Y (d-pad). No more decoding
 * Palm's mangled keyboard channel — that era is preserved in git history.
 *
 * SDL is the only context owner that integrates with the TouchPad's 3-layer
 * compositor, so we render through SDL (fullscreen) rather than /dev/fb0.
 *
 * Run it as root straight from a novacom shell (NOT from the launcher) — the
 * launcher jails PDK apps as uid 5003, which cannot open /dev/input/event*.
 *
 * Build — MUST use the Linaro toolchain; the distro arm-linux-gnueabi-gcc
 * stamps a min-kernel of 3.2.0 and the loader refuses it on 2.6.35:
 *   /opt/gcc-linaro-4.9.4-2017.01-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-gcc \
 *     -O2 -mcpu=cortex-a8 -mfloat-abi=softfp \
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

/* ---- pad state (DS4 via webos-bt-shim) -------------------------------- */
/* sticks + triggers, 0..255 */
static int ax_lx = 128, ax_ly = 128, ax_rx = 128, ax_ry = 128;
static int ax_l2 = 0,  ax_r2 = 0;
static int hatx = 0, haty = 0;                    /* -1..1 */
#define NBTN 14
/* index = BTN code - 0x130; DS4 order via shim's pad_btn table:
 * 0 Square 1 Cross 2 Circle 3 Triangle 4 L1 5 R1 6 L2 7 R2
 * 8 Share 9 Options 10 L3 11 R3 12 PS 13 Touchpad-click */
static int btn[NBTN];

static int btfd = -1;

static void pad_reset(void)
{
    ax_lx = ax_ly = ax_rx = ax_ry = 128;
    ax_l2 = ax_r2 = 0;
    hatx = haty = 0;
    memset(btn, 0, sizeof(btn));
}

static void pad_feed(const struct input_event *e)
{
    if (e->type == EV_KEY) {
        int i = e->code - 0x130;                  /* BTN_SOUTH.. */
        if (i >= 0 && i < NBTN) btn[i] = e->value;
    } else if (e->type == EV_ABS) {
        switch (e->code) {
        case ABS_X:     ax_lx = e->value; break;
        case ABS_Y:     ax_ly = e->value; break;
        case ABS_Z:     ax_rx = e->value; break;  /* right stick X */
        case ABS_RZ:    ax_ry = e->value; break;  /* right stick Y */
        case ABS_RX:    ax_l2 = e->value; break;  /* L2 analog */
        case ABS_RY:    ax_r2 = e->value; break;  /* R2 analog */
        case ABS_HAT0X: hatx  = e->value; break;
        case ABS_HAT0Y: haty  = e->value; break;
        }
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
                /* grab is optional now (nothing else consumes BTN events),
                   but keeps other experiments from interfering */
                if (ioctl(fd, EVIOCGRAB, &one) < 0)
                    fprintf(stderr, "grab failed on %s: %s (continuing)\n",
                            path, strerror(errno));
                fprintf(stderr, "gamepad on %s (%s)\n", path, name);
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
    Uint32 BG, BOX, DOT, ON, OFF, DP, LIVE, DEAD;
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

    BG   = SDL_MapRGB(scr->format, 0x10, 0x10, 0x18);
    BOX  = SDL_MapRGB(scr->format, 0x40, 0x40, 0x58);
    DOT  = SDL_MapRGB(scr->format, 0x40, 0xC0, 0xFF);
    ON   = SDL_MapRGB(scr->format, 0xFF, 0xB0, 0x20);
    OFF  = SDL_MapRGB(scr->format, 0x30, 0x30, 0x40);
    DP   = SDL_MapRGB(scr->format, 0xFF, 0x50, 0x60);
    LIVE = SDL_MapRGB(scr->format, 0x30, 0xD0, 0x60);
    DEAD = SDL_MapRGB(scr->format, 0xC0, 0x30, 0x30);

    btfd = bt_open();
    if (btfd < 0) fprintf(stderr, "no gamepad found (press PS?)\n");

    end = time(NULL) + (secs ? secs : 100000);
    while (running && time(NULL) < end) {
        SDL_Event ev;
        fd_set rs;
        struct timeval tv = { 0, 16000 };
        int i;

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
                    /* pad slept / engine restarted: the shim tears the uinput
                       node down and re-creates it on reconnect. Drop the stale
                       fd and re-acquire below. */
                    fprintf(stderr, "pad went away (%s) - will re-grab\n",
                            strerror(errno));
                    close(btfd); btfd = -1;
                }
                for (k = 0; k < n / (int)sizeof(iev[0]); k++)
                    pad_feed(&iev[k]);
            }
        } else {
            static time_t last_try;
            SDL_Delay(16);
            if (time(NULL) != last_try) {        /* retry about once a second */
                last_try = time(NULL);
                btfd = bt_open();
                if (btfd >= 0) pad_reset();
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

        SDL_FillRect(scr, NULL, BG);

        /* connection pip, top-right */
        box(SCRW - 60, 24, 36, 36, btfd >= 0 ? LIVE : DEAD);

        /* left stick */
        outline(60, 110, 300, 300, 4, BOX);
        box(60 + (ax_lx * (300 - 24)) / 255,
            110 + (ax_ly * (300 - 24)) / 255, 24, 24, DOT);

        /* right stick */
        outline(380, 110, 300, 300, 4, BOX);
        box(380 + (ax_rx * (300 - 24)) / 255,
            110 + (ax_ry * (300 - 24)) / 255, 24, 24, DOT);

        /* analog triggers as vertical fill bars: L2, R2 */
        outline(730, 110, 50, 300, 4, BOX);
        box(730, 110 + 300 - (ax_l2 * 300) / 255, 50, (ax_l2 * 300) / 255, ON);
        outline(810, 110, 50, 300, 4, BOX);
        box(810, 110 + 300 - (ax_r2 * 300) / 255, 50, (ax_r2 * 300) / 255, ON);

        /* d-pad (hat) */
        {
            int cx = 210, cy = 580, a = 48;
            box(cx - a/2, cy - a - a/2, a, a, haty < 0 ? DP : OFF);  /* up */
            box(cx - a/2, cy + a/2,     a, a, haty > 0 ? DP : OFF);  /* down */
            box(cx - a - a/2, cy - a/2, a, a, hatx < 0 ? DP : OFF);  /* left */
            box(cx + a/2, cy - a/2,     a, a, hatx > 0 ? DP : OFF);  /* right */
        }

        /* face buttons in diamond: Square(0) Cross(1) Circle(2) Triangle(3) */
        {
            int cx = 500, cy = 580, a = 56;
            outline(cx - a - a/2 - 4, cy - a/2 - 4, a + 8, a + 8, 3, BOX);
            box(cx - a - a/2, cy - a/2, a, a, btn[0] ? ON : OFF);   /* Square: left */
            outline(cx - a/2 - 4, cy + a/2 - 4, a + 8, a + 8, 3, BOX);
            box(cx - a/2, cy + a/2, a, a, btn[1] ? ON : OFF);       /* Cross: bottom */
            outline(cx + a/2 - 4, cy - a/2 - 4, a + 8, a + 8, 3, BOX);
            box(cx + a/2, cy - a/2, a, a, btn[2] ? ON : OFF);       /* Circle: right */
            outline(cx - a/2 - 4, cy - a - a/2 - 4, a + 8, a + 8, 3, BOX);
            box(cx - a/2, cy - a - a/2, a, a, btn[3] ? ON : OFF);   /* Triangle: top */
        }

        /* remaining buttons, one labeled-by-position row:
           L1 R1 L2 R2 | Share Options | L3 R3 | PS TP  (btn[4..13]) */
        for (i = 4; i < NBTN; i++) {
            int x = 660 + (i - 4) * 36;
            /* small gaps between groups */
            x += (i >= 8) * 12 + (i >= 10) * 12 + (i >= 12) * 12;
            outline(x - 2, 556, 32 + 4, 32 + 4, 2, BOX);
            box(x, 558, 32, 32, btn[i] ? ON : OFF);
        }

        SDL_Flip(scr);
    }

    if (btfd >= 0) close(btfd);
    SDL_Quit();
    PDL_Quit();
    return 0;
}
