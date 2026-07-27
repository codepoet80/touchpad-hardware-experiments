/*
 * padview — live game-controller visualizer for webOS (HP TouchPad)
 *
 * Reads evdev gamepad/joystick devices and draws their state directly to
 * /dev/fb0: analog sticks as dots in boxes, triggers as bars, d-pad as
 * arrows, buttons as a grid. No SDL, no OS support needed — static binary.
 *
 * Build:  arm-linux-gnueabi-gcc -static -O2 -march=armv7-a -o padview padview.c
 * Run:    ./padview [seconds] [/dev/input/eventN ...]
 *         (no device args = autodetect anything with gamepad/joystick buttons)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/fb.h>
#include <linux/input.h>

#define MAXDEV 8
#define NBTN 20            /* BTN_JOYSTICK 0x120 .. 0x133.. covers both pads */
#define BTN_BASE_A 0x120   /* classic joystick range */
#define BTN_BASE_B 0x130   /* gamepad range (DS4) */

static uint32_t *fb;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static int fbfd, stride_px, W, H;

static int axes[8] = {128,128,128,128,128,128,0,0}; /* 0..5 analog, 6..7 hat */
static int amin[8] = {0,0,0,0,0,0,-1,-1}, amax[8] = {255,255,255,255,255,255,1,1};
static int hat_seen = 0, btns[NBTN];

static uint32_t *page(void)
{
    /* draw on the currently displayed pan page */
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vi) == 0 && vi.yoffset)
        return fb + (size_t)vi.yoffset * stride_px;
    return fb;
}

static void rect(uint32_t *p, int x, int y, int w, int h, uint32_t c)
{
    int i, j;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            p[(y + j) * stride_px + x + i] = c;
}

static void frame(uint32_t *p, int x, int y, int w, int h, int t, uint32_t c)
{
    rect(p, x, y, w, t, c); rect(p, x, y + h - t, w, t, c);
    rect(p, x, y, t, h, c); rect(p, x + w - t, y, t, h, c);
}

static int norm(int v, int lo, int hi, int out) /* map v to 0..out */
{
    if (hi <= lo) return out / 2;
    v = (v - lo) * out / (hi - lo);
    return v < 0 ? 0 : (v > out ? out : v);
}

static void draw(void)
{
    uint32_t *p = page();
    const uint32_t BG = 0xFF101018, BOX = 0xFF404058, DOT = 0xFF40C0FF,
                   ON = 0xFFFFB020, OFF = 0xFF303040, BAR = 0xFF40FF90,
                   DP = 0xFFFF5060;
    int i, sb = 320, gap = 60;      /* stick box size / layout */
    int top = 120, lx = 80, rx = W - 80 - sb;

    rect(p, 0, 0, W, H, BG);

    /* left stick: axes 0,1 — right stick: axes 2,5 (DS4) or 2,3-ish */
    frame(p, lx, top, sb, sb, 4, BOX);
    frame(p, rx, top, sb, sb, 4, BOX);
    {
        int dx = lx + norm(axes[0], amin[0], amax[0], sb - 24),
            dy = top + norm(axes[1], amin[1], amax[1], sb - 24);
        rect(p, dx, dy, 24, 24, DOT);
        dx = rx + norm(axes[2], amin[2], amax[2], sb - 24);
        dy = top + norm(axes[5], amin[5], amax[5], sb - 24);
        rect(p, dx, dy, 24, 24, DOT);
    }

    /* triggers: axes 3,4 as vertical bars inside the middle gap */
    {
        int bx = lx + sb + gap, bw = 48, bh = sb;
        int v3 = norm(axes[3], amin[3], amax[3], bh),
            v4 = norm(axes[4], amin[4], amax[4], bh);
        frame(p, bx, top, bw, bh, 4, BOX);
        rect(p, bx + 4, top + bh - v3, bw - 8, v3 ? v3 - 4 : 0, BAR);
        bx = rx - gap - bw;
        frame(p, bx, top, bw, bh, 4, BOX);
        rect(p, bx + 4, top + bh - v4, bw - 8, v4 ? v4 - 4 : 0, BAR);
    }

    /* d-pad (hat axes 6,7): four arrows below left stick */
    {
        int cx = lx + sb / 2, cy = top + sb + 140, a = 44;
        rect(p, cx - a / 2, cy - a - a / 2, a, a, axes[7] < 0 ? DP : OFF); /* up */
        rect(p, cx - a / 2, cy + a / 2,     a, a, axes[7] > 0 ? DP : OFF); /* down */
        rect(p, cx - a - a / 2, cy - a / 2, a, a, axes[6] < 0 ? DP : OFF); /* left */
        rect(p, cx + a / 2, cy - a / 2,     a, a, axes[6] > 0 ? DP : OFF); /* right */
    }

    /* buttons: grid below right stick, 2 rows of 10 */
    for (i = 0; i < NBTN; i++) {
        int bx = rx - 60 + (i % 10) * 44, by = top + sb + 90 + (i / 10) * 60;
        rect(p, bx, by, 36, 48, btns[i] ? ON : OFF);
    }
}

static int is_pad(int fd)
{
    unsigned long kb[(KEY_MAX + 1) / (8 * sizeof(long)) + 1];
    memset(kb, 0, sizeof(kb));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(kb)), kb) < 0) return 0;
    #define HASKEY(k) (kb[(k) / (8 * sizeof(long))] >> ((k) % (8 * sizeof(long))) & 1)
    return HASKEY(BTN_BASE_A) || HASKEY(BTN_BASE_B);
}

static void take_absinfo(int fd)
{
    static const int map[8] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ,
                               ABS_HAT0X, ABS_HAT0Y};
    struct input_absinfo ai;
    int i;
    for (i = 0; i < 8; i++)
        if (ioctl(fd, EVIOCGABS(map[i]), &ai) == 0 && ai.maximum > ai.minimum) {
            amin[i] = ai.minimum; amax[i] = ai.maximum; axes[i] = ai.value;
        }
}

int main(int argc, char **argv)
{
    int fds[MAXDEV], nfd = 0, secs = 120, i;
    time_t end;

    if (argc > 1 && atoi(argv[1]) > 0) secs = atoi(argv[1]);

    for (i = 2; i < argc && nfd < MAXDEV; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd >= 0) { take_absinfo(fd); fds[nfd++] = fd; }
    }
    if (!nfd) { /* autodetect */
        char path[32];
        for (i = 0; i < 16 && nfd < MAXDEV; i++) {
            int fd;
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            fd = open(path, O_RDONLY);
            if (fd < 0) continue;
            if (is_pad(fd)) {
                char name[64] = "?";
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                fprintf(stderr, "padview: using %s (%s)\n", path, name);
                take_absinfo(fd);
                fds[nfd++] = fd;
            } else close(fd);
        }
    }
    if (!nfd) { fprintf(stderr, "padview: no gamepad found\n"); return 1; }

    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("fb0"); return 1; }
    ioctl(fbfd, FBIOGET_VSCREENINFO, &vi);
    ioctl(fbfd, FBIOGET_FSCREENINFO, &fi);
    W = vi.xres; H = vi.yres; stride_px = fi.line_length / 4;
    fb = mmap(0, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fb == MAP_FAILED) { perror("mmap"); return 1; }
    fprintf(stderr, "padview: fb %dx%d @%dbpp, %d pad(s), %ds\n",
            W, H, vi.bits_per_pixel, nfd, secs);

    end = time(NULL) + secs;
    while (time(NULL) < end) {
        fd_set rs;
        struct timeval tv = {0, 33000};
        int mx = 0;
        FD_ZERO(&rs);
        for (i = 0; i < nfd; i++) {
            FD_SET(fds[i], &rs);
            if (fds[i] > mx) mx = fds[i];
        }
        if (select(mx + 1, &rs, 0, 0, &tv) > 0)
            for (i = 0; i < nfd; i++)
                if (FD_ISSET(fds[i], &rs)) {
                    struct input_event ev[32];
                    int n = read(fds[i], ev, sizeof(ev)), k;
                    if (n <= 0) continue;   /* device may vanish; keep going */
                    for (k = 0; k < n / (int)sizeof(ev[0]); k++) {
                        if (ev[k].type == EV_ABS) {
                            switch (ev[k].code) {
                            case ABS_X: axes[0] = ev[k].value; break;
                            case ABS_Y: axes[1] = ev[k].value; break;
                            case ABS_Z: axes[2] = ev[k].value; break;
                            case ABS_RX: axes[3] = ev[k].value; break;
                            case ABS_RY: axes[4] = ev[k].value; break;
                            case ABS_RZ: axes[5] = ev[k].value; break;
                            case ABS_HAT0X: axes[6] = ev[k].value; hat_seen = 1; break;
                            case ABS_HAT0Y: axes[7] = ev[k].value; hat_seen = 1; break;
                            }
                        } else if (ev[k].type == EV_KEY) {
                            int c = ev[k].code;
                            if (c >= BTN_BASE_A && c < BTN_BASE_A + 16)
                                btns[c - BTN_BASE_A] = ev[k].value;
                            else if (c >= BTN_BASE_B && c < BTN_BASE_B + NBTN - 16)
                                btns[16 + c - BTN_BASE_B] = ev[k].value;
                        }
                    }
                }
        /* digital pads report the d-pad on ABS_X/Y; without a hat, mirror it */
        if (!hat_seen) {
            axes[6] = axes[0] > (amin[0] + amax[0]) / 2 + 40 ? 1 :
                      axes[0] < (amin[0] + amax[0]) / 2 - 40 ? -1 : 0;
            axes[7] = axes[1] > (amin[1] + amax[1]) / 2 + 40 ? 1 :
                      axes[1] < (amin[1] + amax[1]) / 2 - 40 ? -1 : 0;
        }
        draw();
    }
    munmap(fb, fi.smem_len);
    close(fbfd);
    return 0;
}
