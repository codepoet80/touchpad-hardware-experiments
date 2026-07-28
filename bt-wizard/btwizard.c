/*
 * btwizard — Bluetooth controller setup + test app for webOS (HP TouchPad)
 *
 * THE PATTERN THIS DEMONSTRATES
 * ----------------------------
 * A Bluetooth gamepad on webOS arrives as a *keyboard*: Palm's stack runs its
 * HID reports through a boot-keyboard parser, so the pad's report bytes become
 * keycodes. If the launcher is the focused window when that happens, those
 * keycodes drive the launcher — opening apps, changing volume, and (before we
 * disabled sysrq) panicking the device.
 *
 * Racing to EVIOCGRAB the device after the fact does not fix this: the stack
 * also injects into hidd's private sockets, and it destroys/recreates the
 * uinput device on every reconnect, silently invalidating any existing grab.
 *
 * So the app drives the connection instead. It takes the screen first, then
 * walks the user through pairing and connecting, so the controller is never
 * connected while the launcher is in focus. Games wanting controller support
 * should follow the same shape.
 *
 * Build:  ./build.sh     (needs /opt/PalmPDK + Linaro 4.9.4 toolchain)
 * Run:    as root from a novacom shell — the launcher jails PDK apps as uid
 *         5003, which cannot open /dev/input/event*.
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
#define PAD_ADDR "84:17:66:d5:ff:51"

/* ------------------------------- tiny font ------------------------------ */
/* 3x5 glyphs, enough to walk someone through setup. */
static const char *GLYPH[][5] = {
 {"111","101","111","101","101"}, /*A*/ {"110","101","110","101","110"}, /*B*/
 {"111","100","100","100","111"}, /*C*/ {"110","101","101","101","110"}, /*D*/
 {"111","100","111","100","111"}, /*E*/ {"111","100","111","100","100"}, /*F*/
 {"111","100","101","101","111"}, /*G*/ {"101","101","111","101","101"}, /*H*/
 {"111","010","010","010","111"}, /*I*/ {"001","001","001","101","111"}, /*J*/
 {"101","101","110","101","101"}, /*K*/ {"100","100","100","100","111"}, /*L*/
 {"101","111","111","101","101"}, /*M*/ {"110","101","101","101","101"}, /*N*/
 {"111","101","101","101","111"}, /*O*/ {"111","101","111","100","100"}, /*P*/
 {"111","101","101","111","001"}, /*Q*/ {"111","101","110","101","101"}, /*R*/
 {"111","100","111","001","111"}, /*S*/ {"111","010","010","010","010"}, /*T*/
 {"101","101","101","101","111"}, /*U*/ {"101","101","101","101","010"}, /*V*/
 {"101","101","111","111","101"}, /*W*/ {"101","101","010","101","101"}, /*X*/
 {"101","101","010","010","010"}, /*Y*/ {"111","001","010","100","111"}, /*Z*/
 {"111","101","101","101","111"}, /*0*/ {"010","110","010","010","111"}, /*1*/
 {"111","001","111","100","111"}, /*2*/ {"111","001","111","001","111"}, /*3*/
 {"101","101","111","001","001"}, /*4*/ {"111","100","111","001","111"}, /*5*/
 {"111","100","111","101","111"}, /*6*/ {"111","001","001","001","001"}, /*7*/
 {"111","101","111","101","111"}, /*8*/ {"111","101","111","001","111"}  /*9*/
};

static SDL_Surface *scr;
static Uint32 C_BG, C_TXT, C_DIM, C_OK, C_WARN, C_BOX, C_DOT, C_ON, C_OFF, C_DP;

static void box(int x, int y, int w, int h, Uint32 c)
{ SDL_Rect r; r.x=x; r.y=y; r.w=w; r.h=h; SDL_FillRect(scr,&r,c); }

static void outline(int x,int y,int w,int h,int t,Uint32 c)
{ box(x,y,w,t,c); box(x,y+h-t,w,t,c); box(x,y,t,h,c); box(x+w-t,y,t,h,c); }

static int glyph_index(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a';
    if (ch >= '0' && ch <= '9') return 26 + (ch - '0');
    return -1;
}

static void text(int x, int y, int s, Uint32 c, const char *str)
{
    int gx = x, r, col, gi;
    for (; *str; str++) {
        if (*str == ' ') { gx += 4 * s; continue; }
        gi = glyph_index(*str);
        if (gi >= 0)
            for (r = 0; r < 5; r++)
                for (col = 0; col < 3; col++)
                    if (GLYPH[gi][r][col] == '1')
                        box(gx + col * s, y + r * s, s, s, c);
        gx += 4 * s;
    }
}

static void centre(int y, int s, Uint32 c, const char *str)
{
    int n = 0; const char *p = str;
    for (; *p; p++) n++;
    text((SCRW - n * 4 * s) / 2, y, s, c, str);
}

/* ------------------------- luna / shell helpers ------------------------- */
/* luna-send only prints responses in -i mode, and -i never exits on its own,
   so every call is backgrounded and reaped (see webos-mcp gotchas). */
static void luna(const char *uri, const char *json)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "(luna-send -i %s '%s' & P=$!; sleep 2; kill $P) >/dev/null 2>&1",
        uri, json);
    system(cmd);
}

/* radioon sometimes returns "Message status unknown" if btmonitor is not ready
   yet, leaving the radio off and the wizard waiting forever for a connection
   that cannot arrive. Check for the stack processes rather than trusting it. */
static int radio_up(void)
{
    FILE *f = popen("ps | grep -c '[P]mBtEngine'", "r");
    char buf[16] = {0};
    int n = 0;
    if (!f) return 0;
    if (fgets(buf, sizeof(buf), f)) n = atoi(buf);
    pclose(f);
    return n > 0;
}

static int pad_present(void)
{
    FILE *f = fopen("/proc/bus/input/devices", "r");
    char line[256];
    int found = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, "Wireless Controller")) { found = 1; break; }
    fclose(f);
    return found;
}

/* -------------------------- pad decode (see README) --------------------- */
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

static int btfd = -1;
static unsigned char byte0 = 127;
static int bbyte = 0x08;
/* Per-read-batch flags used to infer d-pad UP. Button byte 0x00 is the HID
   "empty slot" marker, so the parser drops it before any table lookup and UP is
   never transmitted. But releasing the previous value still emits a release, and
   for any *other* new value a press follows in the same batch. So: a release
   with no accompanying press means the byte became 0x00, i.e. d-pad UP. */
static int batch_cand[24], batch_ncand = 0, batch_release = 0, bbyte_valid = 0;
static int awaiting_replacement = 0;
static unsigned long nreports = 0;

/* With the full table patch installed, every one of the 144 hat+face
   combinations has its own keycode, unique across the entire table - so this is
   an exact reverse lookup rather than a guess. 0 means "not a button byte".
   Generated by tools/fullpatch.py; must match the installed library. */
static const unsigned char kc2byte[256] = {
      0,   0,   0,   1,  32,  33,  34,  35,  36,  37,  38,  39,   2,   3,  40,  50,
     20,  70,   8,  21,  23,  72,  24,  88,  18,  19, 101,  48,   0,   0,   4,  22,
      7, 102, 117, 120, 128, 129, 130,  51,  52,  53,   0,  49, 131, 132,   6, 134,
      5,  17,  16,  54,  55,  56,   0,  85,   0,   0, 149, 150, 151, 152, 160, 161,
    162,  64,  65,  66,  67,  83,  71, 163,  96,  97,  86, 164, 165, 166,  87, 167,
    168, 176,  98,  99, 177, 148, 100,  68,  69, 135, 146, 147, 178, 136, 179, 180,
      0,   0,  84,   0,   0, 181, 182,  82, 184,  80, 192, 193,  81, 194, 195, 196,
    197,   0,   0,   0,   0, 103, 198,   0,   0, 133, 144, 145, 199,   0,   0,   0,
      0, 200, 118, 208, 119, 209, 116, 210, 244, 211,   0,   0,   0, 212,   0,   0,
    213, 214, 215, 216, 232, 240,   0, 241,   0, 242, 243,   0, 245, 246,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0, 183, 247, 248, 104,   0,   0,   0,   0,   0,   0,   0, 112,
    113, 114, 115,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static int keycode_to_usage(int kc)
{
    if (kc < 0 || kc > 255) return -1;
    if (kc2byte[kc] || kc == 0) {
        int v = kc2byte[kc];
        return v ? v : -1;
    }
    return -1;
}

static void feed(int code, int val)
{
    int i, u;
    for (i = 0; i < 8; i++)
        if (mod_keys[i] == code) {
            if (val) byte0 |= (1 << i); else byte0 &= ~(1 << i);
            return;
        }
    u = keycode_to_usage(code);
    if (u < 0) return;
    /* Non-latching: the newest value whose low nibble is a released hat (8)
       wins; a d-pad press only shifts the nibble; releasing the tracked value
       returns to neutral. Preserving the face bits on release latched buttons
       on permanently, which was the "buttons get stuck" bug. */
    /* Any value whose low nibble is a legal hat (0-8) could be the button byte.
       Collect them and resolve once the batch is complete - deciding per-event
       was wrong: holding a direction and adding a face button changes the hat
       AND the face bits at once (0x06 -> 0x26), which a per-event rule rejected. */
    if (val) {
        if ((u & 0x0f) <= 8 && batch_ncand < 24) batch_cand[batch_ncand++] = u;
    } else if (u == bbyte) {
        batch_release = 1;
    }
}

static int popcount8(int v)
{ int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

/* Resolve one read batch into the new button byte.
 *
 * The decisive rule: the button byte has only changed if the value we were
 * tracking was RELEASED in this batch. Analog bytes also produce press/release
 * traffic and some of their values look like legal button bytes (a centred
 * stick sits at 0x81, which reads as Triangle+NE) - but they never release
 * *our* value, so gating on that keeps them out. Judging candidates on
 * similarity alone let the tracker drift onto the stick and stick there. */
static void resolve_batch(void)
{
    int i, best = -1, bestd = 99;

    if (!bbyte_valid) {                    /* acquire: hat-released, no faces */
        for (i = 0; i < batch_ncand; i++)
            if (batch_cand[i] == 0x08) { bbyte = 0x08; bbyte_valid = 1; return; }
        return;
    }

    for (i = 0; i < batch_ncand; i++) {
        int dsc = popcount8(batch_cand[i] ^ bbyte);
        if (dsc < bestd) { bestd = dsc; best = batch_cand[i]; }
    }

    /* A release and its replacement press do not always land in the same read.
       Previously that made us latch: we inferred UP on the release, then
       rejected the press because it arrived without a release of its own. So
       remember that we are mid-change and accept the next candidate. */
    if (batch_release && best >= 0) {
        bbyte = best; awaiting_replacement = 0;
    } else if (batch_release) {
        bbyte = 0x00;                      /* untransmittable UP - provisional */
        awaiting_replacement = 1;
    } else if (awaiting_replacement && best >= 0) {
        bbyte = best; awaiting_replacement = 0;
    }
}

static int grab_pad(void)
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
                unsigned char keys[(KEY_MAX + 7) / 8];
                int kc;
                ioctl(fd, EVIOCGRAB, &one);
                fprintf(stderr, "grabbed %s (%s)\n", path, name);

                /* Sync to the pad's CURRENT state. evdev only reports changes,
                   so if we attach while it sits at rest no event ever announces
                   the resting button byte and the decoder never initialises -
                   it looked like "stuck until you cycle focus", because that
                   finally delivered a backlog containing one. */
                memset(keys, 0, sizeof(keys));
                byte0 = 127; bbyte = 0x08; bbyte_valid = 1;
                if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
                    byte0 = 0;
                    for (kc = 0; kc < 256; kc++) {
                        if (!(keys[kc >> 3] & (1 << (kc & 7)))) continue;
                        for (i = 0; i < 8; i++)
                            if (mod_keys[i] == kc) byte0 |= (1 << i);
                        if (kc2byte[kc]) bbyte = kc2byte[kc];
                    }
                    fprintf(stderr, "synced: byte0=0x%02x button=0x%02x\n",
                            byte0, bbyte);
                }
                return fd;
            }
        }
        close(fd);
    }
    return -1;
}

/* --------------------------------- states -------------------------------- */
enum { ST_INTRO, ST_PAIRMODE, ST_PAIRING, ST_PREPARE, ST_PRESSPS, ST_RUN };

int main(int argc, char **argv)
{
    int state = ST_INTRO, running = 1, tick = 0;
    time_t tstate;

    PDL_Init(0);
    if (SDL_Init(SDL_INIT_VIDEO) < 0) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1; }
    scr = SDL_SetVideoMode(SCRW, SCRH, 0, SDL_SWSURFACE | SDL_FULLSCREEN);
    if (!scr) { fprintf(stderr,"video: %s\n",SDL_GetError()); return 1; }
    SDL_ShowCursor(SDL_DISABLE);

    C_BG  = SDL_MapRGB(scr->format,0x10,0x10,0x18);
    C_TXT = SDL_MapRGB(scr->format,0xF0,0xF0,0xFF);
    C_DIM = SDL_MapRGB(scr->format,0x60,0x60,0x78);
    C_OK  = SDL_MapRGB(scr->format,0x40,0xFF,0x90);
    C_WARN= SDL_MapRGB(scr->format,0xFF,0xB0,0x20);
    C_BOX = SDL_MapRGB(scr->format,0x40,0x40,0x58);
    C_DOT = SDL_MapRGB(scr->format,0x40,0xC0,0xFF);
    C_ON  = SDL_MapRGB(scr->format,0xFF,0xB0,0x20);
    C_OFF = SDL_MapRGB(scr->format,0x30,0x30,0x40);
    C_DP  = SDL_MapRGB(scr->format,0xFF,0x50,0x60);

    /* sysrq: a pad's report bytes can land on KEY_SYSRQ and issue kernel
       commands, including Crash. Never run a BT pad with this enabled. */
    system("echo 0 > /proc/sys/kernel/sysrq 2>/dev/null");

    /* webOS wants to banner "Connected to Wireless Controller" at exactly the
       moment the pad starts streaming keycodes. If that steals input focus, the
       pad drives the system instead of us - the whole reason this app owns the
       connection. Suppress banners for the duration. */
    PDL_BannerMessagesEnable(PDL_FALSE);

    /* If a cached SDP record already exists the controller is paired, so skip
       straight to configuring it. A game shipping this flow wants exactly that:
       full setup the first time, quick reconnect thereafter. */
    {
        FILE *f = fopen("/var/hid.j", "r");
        if (f) {
            fclose(f);
            state = ST_PREPARE;
            fprintf(stderr, "already paired - skipping to configure\n");
        }
    }
    if (argc > 1 && !strcmp(argv[1], "--pair")) state = ST_INTRO;   /* force */

    tstate = time(NULL);
    while (running) {
        SDL_Event ev;
        int held = (int)(time(NULL) - tstate);

        while (SDL_PollEvent(&ev))
            if (ev.type == SDL_QUIT) running = 0;

        SDL_FillRect(scr, NULL, C_BG);
        centre(40, 6, C_DIM, "BLUETOOTH CONTROLLER SETUP");
        {   /* focus telltale: if this ever reads NO the launcher is receiving
               the pad's keycodes instead of us */
            int st = SDL_GetAppState();
            int focused = (st & SDL_APPINPUTFOCUS) != 0;
            static int lost = 0;
            if (!focused) lost++;
            text(20, SCRH-40, 3, focused ? C_DIM : C_WARN,
                 focused ? "FOCUS OK" : "FOCUS LOST");
            if (lost) {
                char b[32]; snprintf(b, sizeof(b), "LOSSES %d", lost);
                text(200, SCRH-40, 3, C_WARN, b);
            }
        }

        switch (state) {
        case ST_INTRO:
            centre(220, 8, C_TXT, "PREPARING RADIO");
            centre(320, 5, C_DIM, "KEEP THIS APP IN FOCUS");
            centre(380, 5, C_DIM, "SO THE LAUNCHER NEVER SEES THE PAD");
            if (held >= 2) {
                luna("palm://com.palm.btmonitor/monitor/radioon",
                     "{\"visible\":true,\"connectable\":true}");
                state = ST_PAIRMODE; tstate = time(NULL);
            }
            break;

        case ST_PAIRMODE:
            centre(200, 9, C_WARN, "HOLD SHARE AND PS");
            centre(310, 5, C_TXT, "UNTIL THE LIGHT BAR DOUBLE FLASHES");
            centre(420, 5, C_DIM, "SEARCHING");
            box(300 + ((tick / 6) % 8) * 55, 470, 45, 14, C_DOT);
            if (held >= 6) { state = ST_PAIRING; tstate = time(NULL); }
            break;

        case ST_PAIRING:
            centre(220, 8, C_TXT, "PAIRING");
            centre(330, 5, C_DIM, "DO NOT PRESS ANYTHING");
            if (held == 1)
                system("nohup sh /tmp/btpair.sh >/dev/null 2>&1 &");
            if (held >= 55 || pad_present()) { state = ST_PREPARE; tstate = time(NULL); }
            break;

        case ST_PREPARE:
            centre(220, 8, C_TXT, "CONFIGURING");
            centre(330, 5, C_DIM, "RESTARTING BLUETOOTH");
            if (held == 1) {
                /* The stack reads /var/hid.j at startup, so the subClass patch
                   only takes effect after a radio cycle. 64 sets the keyboard
                   bit so reports are dispatched instead of dropped with
                   "unknown hidDevType 0x0". */
                system("sed -i 's/\"subClass\":8/\"subClass\":64/' /var/hid.j 2>/dev/null");
                system("sync");
                luna("palm://com.palm.btmonitor/monitor/radiooff", "{}");
            }
            if (held == 6 || (held > 6 && held % 6 == 0 && !radio_up()))
                luna("palm://com.palm.btmonitor/monitor/radioon",
                     "{\"visible\":true,\"connectable\":true}");
            if (held >= 18 && radio_up()) { state = ST_PRESSPS; tstate = time(NULL); }
            if (held > 6 && !radio_up())
                centre(430, 4, C_WARN, "RESTARTING RADIO");
            break;

        case ST_PRESSPS:
            centre(200, 10, C_WARN, "PRESS THE PS BUTTON");
            centre(330, 5, C_TXT, "ON THE CONTROLLER");
            centre(430, 4, radio_up() ? C_DIM : C_WARN,
                   radio_up() ? "WAITING FOR CONNECTION" : "RADIO DOWN RETRYING");
            if (!radio_up() && (tick % 180) == 0)
                luna("palm://com.palm.btmonitor/monitor/radioon",
                     "{\"visible\":true,\"connectable\":true}");
            if ((tick / 20) % 2) box(SCRW/2 - 40, 500, 80, 80, C_WARN);
            if (pad_present()) {
                btfd = grab_pad();
                if (btfd >= 0) { state = ST_RUN; tstate = time(NULL); }
            }
            break;

        case ST_RUN: {
            static const signed char hatx[9]={0,1,1,1,0,-1,-1,-1,0};
            static const signed char haty[9]={-1,-1,0,1,1,1,0,-1,0};
            int hat = bbyte & 0x0f, i;
            if (hat > 8) hat = 8;

            centre(30, 5, C_OK, "CONNECTED");

            /* recovered analog axis */
            outline(70, 110, 300, 300, 4, C_BOX);
            box(70 + (byte0 * (300 - 24)) / 255, 245, 24, 24, C_DOT);
            text(70, 425, 4, C_DIM, "STICK");

            /* d-pad */
            { int cx=220, cy=560, a=40;
              box(cx-a/2, cy-a-a/2, a,a, haty[hat]<0?C_DP:C_OFF);
              box(cx-a/2, cy+a/2,   a,a, haty[hat]>0?C_DP:C_OFF);
              box(cx-a-a/2, cy-a/2, a,a, hatx[hat]<0?C_DP:C_OFF);
              box(cx+a/2, cy-a/2,   a,a, hatx[hat]>0?C_DP:C_OFF); }

            /* face buttons */
            { static const int mask[4]={0x10,0x20,0x40,0x80};
              static const char *nm[4]={"SQ","X","O","TRI"};
              for (i=0;i<4;i++) {
                  outline(560+i*105, 150, 85, 85, 3, C_BOX);
                  box(566+i*105, 156, 73, 73, (bbyte&mask[i])?C_ON:C_OFF);
                  text(566+i*105, 250, 4, C_DIM, nm[i]);
              } }

            /* raw button byte, for diagnosing the decode */
            text(560, 400, 4, C_DIM, "BYTE");
            for (i=0;i<8;i++)
                box(560+i*42, 450, 34, 34, (bbyte&(1<<(7-i)))?C_ON:C_OFF);

            /* Read the pad's ACTUAL state every frame instead of tracking
               press/release events. Event bookkeeping wedges permanently if a
               single release is ever missed - which is what made buttons stick
               until you cycled focus. Polling cannot get stuck: whatever we
               miss, the next frame reports the truth. */
            if (btfd >= 0) {
                unsigned char keys[(KEY_MAX + 7) / 8];
                int kc, i2, best = -1, bestd = 99, b0 = 0;
                memset(keys, 0, sizeof(keys));
                if (ioctl(btfd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
                    for (kc = 0; kc < 256; kc++) {
                        if (!(keys[kc >> 3] & (1 << (kc & 7)))) continue;
                        for (i2 = 0; i2 < 8; i2++)
                            if (mod_keys[i2] == kc) b0 |= (1 << i2);
                        if (kc2byte[kc]) {          /* a legal button byte */
                            int dsc = popcount8(kc2byte[kc] ^ bbyte);
                            if (dsc < bestd) { bestd = dsc; best = kc2byte[kc]; }
                        }
                    }
                    byte0 = (unsigned char)b0;
                    /* nothing held that looks like a button byte means the byte
                       is 0x00 - d-pad UP, which HID cannot transmit (0x00 is
                       the empty-slot marker) */
                    bbyte = (best >= 0) ? best : 0x00;
                }
            }

            if (btfd >= 0) {
                fd_set rs; struct timeval tv={0,10000};
                FD_ZERO(&rs); FD_SET(btfd,&rs);
                if (select(btfd+1,&rs,0,0,&tv) > 0) {
                    struct input_event iev[32];
                    int n = read(btfd, iev, sizeof(iev)), k;
                    if (n <= 0 && errno != EAGAIN && errno != EINTR) {
                        close(btfd); btfd = -1;      /* pad vanished */
                        state = ST_PRESSPS; tstate = time(NULL);
                    }
                    for (k=0;k<n/(int)sizeof(iev[0]);k++)
                        if (iev[k].type == EV_KEY) nreports++;   /* drained only */
                }
            }
            break; }
        }

        SDL_Flip(scr);
        SDL_Delay(16);
        tick++;
    }
    PDL_BannerMessagesEnable(PDL_TRUE);        /* leave the system as we found it */
    if (btfd >= 0) close(btfd);
    SDL_Quit(); PDL_Quit();
    return 0;
}
