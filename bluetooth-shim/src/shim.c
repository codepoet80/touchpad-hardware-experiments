/*
 * shim.c -- LD_PRELOAD interposers for the three HID->uinput bridge functions in
 * libPmBtBsaif.so.  Because those symbols are called through the library's own
 * PLT/GOT (see docs/ARCHITECTURE.md ?6), preloading same-named symbols captures
 * every call, including the internal one from PmBtBsaifHandleHidhPrim.
 *
 *   PmBtBsaifHidOpenUInput(dev, remdev)  -- create the input node
 *   PmBtBsaifHidSendToInput(dev, msg)    -- translate a report
 *   PmBtBsaifHidCloseUInput(dev)         -- tear down
 *
 * Policy:
 *   - keyboards (and anything with no mappable non-keyboard fields) are handed
 *     straight to the original implementation via RTLD_NEXT, so stock keyboard
 *     behaviour is preserved byte-for-byte;
 *   - mice / gamepads (which the stock code classifies but then DROPS) are taken
 *     over: we build a properly-capable uinput node from the device's own HID
 *     report descriptor and translate every report ourselves.
 *
 * WEBOS_BT_SHIM_DUMP=1 logs each device's descriptor and every raw report,
 * regardless of who ends up handling it -- the on-device validation tool.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>

#include "devinst.h"
#include "hid_parser.h"
#include "uinput_dev.h"
#include "wiimote.h"
#include "log.h"

/* remote-device info block offsets (param_2 of OpenUInput) */
#define REMDEV_VENDOR  0x04
#define REMDEV_PRODUCT 0x06
#define REMDEV_VERSION 0x08
#define REMDEV_NAME    0x12

#define EXPORT __attribute__((visibility("default")))

typedef void (*open_fn)(void *dev, void *remdev);
typedef void (*send_fn)(void *dev, void *msg);
typedef void (*close_fn)(void *dev);
typedef int  (*passkey_fn)(void *addr, void *pin, unsigned char pinlen);
typedef int  (*ssp_fn)(int accept, void *addr, unsigned int passkey);
typedef int  (*getzone_fn)(int zone);
typedef void (*scpasskey_fn)(void *msg);
typedef void (*fromcsraddr_fn)(void *dst, unsigned int lap, unsigned int uapnap);

static open_fn        real_open;
static send_fn        real_send;
static close_fn       real_close;
static passkey_fn     real_passkey;
static ssp_fn         real_sspaccept;
static getzone_fn     real_getzone;
typedef int  (*debond_fn)(void *addr);
static scpasskey_fn   real_scpasskeyind;
static scpasskey_fn   real_scssppasskeyind;
static scpasskey_fn   real_scbondcfm;
static debond_fn      p_debond;
static fromcsraddr_fn p_fromcsraddr;

/* GOT slot offsets (r_offset) of intra-libPmBtBsaif calls we must redirect.
 * From `readelf -r libPmBtBsaif.so` (webOS 3.0.5 topaz). The old loader binds
 * these internal PLT calls to the local definition, so LD_PRELOAD symbol
 * interposition is ignored -- we overwrite the GOT slots at runtime instead. */
#define GOT_HidOpenUInput     0xe3794
#define GOT_HidSendToInput    0xe373c
#define GOT_HidCloseUInput    0xe39f8
#define GOT_handleScPasskey   0xe3e54
#define GOT_handleScSspPasskey 0xe3a1c
#define GOT_handleScBondCfm   0xe37c0

/* Overwrite one GOT slot at base+offset with newfn, but only if it currently
 * holds expect_real (guards against a wrong offset corrupting the table). */
static void got_patch(void *base, unsigned long off, void *expect_real,
                      void *newfn, const char *name)
{
    void **slot = (void **)((char *)base + off);
    long ps = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)slot & ~((uintptr_t)ps - 1));
    if (mprotect(page, ps, PROT_READ | PROT_WRITE) != 0) {
        shim_log("got_patch %s: mprotect failed", name);
        return;
    }
    /* With lazy binding the slot may still hold the resolver stub (not the real
     * function) until first call; patching before that is fine -- it just means
     * the resolver never runs.  Log old vs expected for visibility, patch either
     * way (the offset is fixed for this exact binary). */
    shim_log("got_patch %s: slot=%p old=%p expect_real=%p -> %p",
             name, (void *)slot, *slot, expect_real, newfn);
    *slot = newfn;
}

#define MAX_MANAGED 8
struct managed {
    void *dev;
    int   fd;
    int   active;
    int   is_wiimote;
    int   handle;
    struct hid_profile prof;
};
static struct managed g_tab[MAX_MANAGED];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* forward decls (our interposers, defined below) so the constructor can take
 * their addresses for GOT patching */
void PmBtBsaifHidOpenUInput(void *dev, void *remdev);
void PmBtBsaifHidSendToInput(void *dev, void *msg);
void PmBtBsaifHidCloseUInput(void *dev);
void handleScPasskeyInd(void *msg);
void handleScSspPasskeyInd(void *msg);
void handleScBondCfm(void *msg);

__attribute__((constructor))
static void shim_init(void)
{
    /* Default dump ON (debugging) so it works via ld.so.preload without our env;
     * WEBOS_BT_SHIM_DUMP=0 disables it.  The env is baked into the upstart job
     * (changing it needs a reboot), so a file flag overrides it at runtime:
     * `touch /var/btshim-nodump` + killall PmBtEngine turns dumping off. */
    const char *d = getenv("WEBOS_BT_SHIM_DUMP");
    g_shim_dump = (d == 0) ? 1 : (d[0] == '1');
    if (access("/var/btshim-nodump", F_OK) == 0) g_shim_dump = 0;
    real_open      = (open_fn)    dlsym(RTLD_NEXT, "PmBtBsaifHidOpenUInput");
    real_send      = (send_fn)    dlsym(RTLD_NEXT, "PmBtBsaifHidSendToInput");
    real_close     = (close_fn)   dlsym(RTLD_NEXT, "PmBtBsaifHidCloseUInput");
    real_passkey   = (passkey_fn) dlsym(RTLD_NEXT, "PmBtBsaifPassKey");
    real_sspaccept = (ssp_fn)     dlsym(RTLD_NEXT, "PmBtBsaifSspAccept");
    real_getzone   = (getzone_fn) dlsym(RTLD_NEXT, "PmBtDbgGetZoneState");
    real_scpasskeyind    = (scpasskey_fn)   dlsym(RTLD_NEXT, "handleScPasskeyInd");
    real_scssppasskeyind = (scpasskey_fn)   dlsym(RTLD_NEXT, "handleScSspPasskeyInd");
    real_scbondcfm       = (scpasskey_fn)   dlsym(RTLD_NEXT, "handleScBondCfm");
    p_debond             = (debond_fn)      dlsym(RTLD_DEFAULT, "PmBtBsaifDebond");
    p_fromcsraddr        = (fromcsraddr_fn) dlsym(RTLD_DEFAULT, "PmBtHelpFromCsrAddrCpy");

    if (real_open || real_send || real_close) {
        shim_log("webos-bt-shim loaded (dump=%d, real open=%p send=%p close=%p)",
                 g_shim_dump, (void *)real_open, (void *)real_send, (void *)real_close);

        /* This loader binds libPmBtBsaif's calls to its OWN functions locally,
         * so LD_PRELOAD can't intercept them.  Overwrite the GOT slots so the
         * intra-library PLT calls land in our interposers instead. */
        {
            Dl_info info;
            if (dladdr((void *)real_open, &info) && info.dli_fbase) {
                void *base = info.dli_fbase;
                shim_log("GOT-patching libPmBtBsaif at base=%p", base);
                got_patch(base, GOT_HidOpenUInput,  (void *)real_open,  (void *)PmBtBsaifHidOpenUInput,  "HidOpenUInput");
                got_patch(base, GOT_HidSendToInput, (void *)real_send,  (void *)PmBtBsaifHidSendToInput, "HidSendToInput");
                got_patch(base, GOT_HidCloseUInput, (void *)real_close, (void *)PmBtBsaifHidCloseUInput, "HidCloseUInput");
                got_patch(base, GOT_handleScPasskey,    (void *)real_scpasskeyind,    (void *)handleScPasskeyInd,    "handleScPasskeyInd");
                got_patch(base, GOT_handleScSspPasskey, (void *)real_scssppasskeyind, (void *)handleScSspPasskeyInd, "handleScSspPasskeyInd");
                got_patch(base, GOT_handleScBondCfm,    (void *)real_scbondcfm,       (void *)handleScBondCfm,       "handleScBondCfm");
            } else {
                shim_log("dladdr failed -- cannot GOT-patch; intra-lib hooks inactive");
            }
        }
    }
}

static struct managed *find(void *dev)
{
    int i;
    for (i = 0; i < MAX_MANAGED; i++)
        if (g_tab[i].active && g_tab[i].dev == dev)
            return &g_tab[i];
    return NULL;
}
static struct managed *alloc_slot(void *dev)
{
    int i;
    for (i = 0; i < MAX_MANAGED; i++)
        if (!g_tab[i].active) {
            memset(&g_tab[i], 0, sizeof(g_tab[i]));
            g_tab[i].dev = dev;
            g_tab[i].fd = -1;
            g_tab[i].active = 1;
            return &g_tab[i];
        }
    return NULL;
}

/* ---------------------------------------------------------------- */

/* ---- descriptor recovery ----------------------------------------
 * The stack persists HID report descriptors to /var/hid.j as UNPADDED %x text
 * and reads them back with a parser that zeroes every hex letter (observed on
 * device: a1->01, c3->03, ff->00, plus expansion to 16-bit elements).  So on
 * any reconnect that uses the cached record, dev+0x6a0 holds garbage and the
 * parse finds nothing.  Recovery ladder:
 *   1. parse the bytes as delivered
 *   2. if they look u16-expanded, compact (drop the 00 high bytes) and re-parse
 *   3. shim's own cache (/var/btshim.d/<bdaddr>.rdesc), written whenever a
 *      descriptor from the device itself parsed cleanly
 *   4. built-in descriptor matched by vendor/product id
 */
#define SHIM_CACHE_DIR "/var/btshim.d"

/* DS4 (BT report-1 "simple mode"): X,Y,Z,Rz sticks; 4-bit hat w/ null; 14
 * buttons; 6-bit vendor counter; Rx,Ry analog triggers.  Verified against the
 * device's own SDP record (decoded from hid.j) -- identical field layout. */
static const uint8_t BUILTIN_DS4[] = {
    0x05,0x01, 0x09,0x05, 0xA1,0x01,
    0x85,0x01,
    0x09,0x30, 0x09,0x31, 0x09,0x32, 0x09,0x35,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x04, 0x81,0x02,
    0x09,0x39, 0x15,0x00, 0x25,0x07, 0x35,0x00, 0x46,0x3B,0x01, 0x65,0x14,
    0x75,0x04, 0x95,0x01, 0x81,0x42, 0x65,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x0E,
    0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x0E, 0x81,0x02,
    0x06,0x00,0xFF, 0x09,0x20, 0x75,0x06, 0x95,0x01, 0x15,0x00, 0x25,0x7F, 0x81,0x02,
    0x05,0x01, 0x09,0x33, 0x09,0x34,
    0x15,0x00, 0x26,0xFF,0x00, 0x75,0x08, 0x95,0x02, 0x81,0x02,
    0xC0
};

static const struct { uint16_t vid, pid; const uint8_t *desc; int len; const char *name; }
BUILTIN_DESC[] = {
    { 0x054c, 0x05c4, BUILTIN_DS4, (int)sizeof(BUILTIN_DS4), "DualShock 4" },
    { 0x054c, 0x09cc, BUILTIN_DS4, (int)sizeof(BUILTIN_DS4), "DualShock 4 v2" },
};

/* parse + require something we'd actually take over */
static int try_parse(const uint8_t *desc, int len, struct hid_profile *prof)
{
    if (len <= 0 || len > DEV_RDESC_MAX) return 0;
    if (hid_parse(desc, len, prof) != 0) return 0;
    return (prof->is_mouse || prof->is_gamepad) && prof->nfields > 0;
}

/* u16-expansion check: nearly all odd bytes are 0 */
static int looks_u16_expanded(const uint8_t *d, int len)
{
    int i, zeros = 0, n = 0;
    if (len < 8) return 0;
    for (i = 1; i < len; i += 2) { n++; if (d[i] == 0) zeros++; }
    return n > 0 && zeros * 10 >= n * 9;
}

static void cache_path(const uint8_t *bd, char *buf, int buflen)
{
    snprintf(buf, buflen, SHIM_CACHE_DIR "/%02x%02x%02x%02x%02x%02x.rdesc",
             bd[0], bd[1], bd[2], bd[3], bd[4], bd[5]);
}

static void cache_save(const uint8_t *bd, const uint8_t *desc, int len)
{
    char path[96];
    int fd;
    mkdir(SHIM_CACHE_DIR, 0755);
    cache_path(bd, path, sizeof(path));
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    if (write(fd, desc, len) != len) shim_log("cache_save %s: short write", path);
    close(fd);
}

static int cache_load(const uint8_t *bd, uint8_t *desc, int maxlen)
{
    char path[96];
    int fd, n;
    cache_path(bd, path, sizeof(path));
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    n = read(fd, desc, maxlen);
    close(fd);
    return n;
}

EXPORT void PmBtBsaifHidOpenUInput(void *dev, void *remdev)
{
    uint16_t desclen;
    const uint8_t *desc;
    struct hid_profile prof;
    char descr[128];
    struct input_id id;
    const char *name;
    struct managed *m;
    int fd;

    desclen = U16(dev, DEV_RDESC_LEN);
    desc    = FIELD(dev, DEV_RDESC);
    name    = (const char *)FIELD(remdev, REMDEV_NAME);

    if (g_shim_dump) {
        const uint8_t *bd = FIELD(dev, DEV_BDADDR);
        shim_dbg("OpenUInput dev=%p name='%s' bd=%02x:%02x:%02x:%02x:%02x:%02x "
                 "subclass=0x%02x id=%u rdesc_len=%u", dev, name,
                 bd[0],bd[1],bd[2],bd[3],bd[4],bd[5],
                 U8(dev, DEV_SUBCLASS), U8(dev, DEV_ID), desclen);
        if (desclen > 0 && desclen <= DEV_RDESC_MAX)
            shim_hexdump("report-descriptor", desc, desclen);
    }

    /* ---- Wii Remote: dedicated path (custom protocol, not standard HID) ---- */
    {
        uint8_t written[6];
        if (wiimote_name_matches(name) ||
            wiimote_is_nintendo(FIELD(dev, DEV_BDADDR), written)) {
            int handle = U8(dev, DEV_ID);
            int wfd = wiimote_create_uinput(name,
                          U16(remdev, REMDEV_VENDOR), U16(remdev, REMDEV_PRODUCT));
            if (wfd < 0) { if (real_open) real_open(dev, remdev); return; }

            pthread_mutex_lock(&g_lock);
            m = find(dev);
            if (m) { uinput_destroy(m->fd); m->active = 0; }
            m = alloc_slot(dev);
            if (m) { m->fd = wfd; m->is_wiimote = 1; m->handle = handle; }
            pthread_mutex_unlock(&g_lock);
            if (!m) { uinput_destroy(wfd); return; }

            U8(dev, DEV_UINPUT_FLAG) = 1;
            U32(dev, DEV_UINPUT_FD)  = (uint32_t)wfd;
            shim_log("took over dev=%p as Wii Remote (fd=%d handle=%d)", dev, wfd, handle);
            wiimote_start_reporting(handle);
            return;
        }
    }

    /* Descriptor recovery ladder (see comment above BUILTIN_DS4). */
    {
        static uint8_t buf[DEV_RDESC_MAX];
        const char *src = NULL;
        const uint8_t *bd = FIELD(dev, DEV_BDADDR);
        uint16_t vid = U16(remdev, REMDEV_VENDOR);
        uint16_t pid = U16(remdev, REMDEV_PRODUCT);

        if (try_parse(desc, desclen, &prof)) {
            src = "device";
            cache_save(bd, desc, desclen);          /* good bytes -> remember */
        } else if (desclen > 0 && desclen <= DEV_RDESC_MAX &&
                   hid_parse(desc, desclen, &prof) == 0 &&
                   prof.is_keyboard && !prof.is_mouse && !prof.is_gamepad) {
            shim_log("keyboard -> delegating to stock OpenUInput");
            if (real_open) real_open(dev, remdev);
            return;
        }
        if (!src && desclen > 0 && desclen <= DEV_RDESC_MAX &&
            looks_u16_expanded(desc, desclen)) {
            int n = desclen / 2, i;
            for (i = 0; i < n; i++) buf[i] = desc[2 * i];
            if (try_parse(buf, n, &prof)) { src = "compacted"; cache_save(bd, buf, n); }
        }
        if (!src) {
            int n = cache_load(bd, buf, (int)sizeof(buf));
            if (n > 0 && try_parse(buf, n, &prof)) src = "shim-cache";
        }
        if (!src) {
            unsigned bi;
            for (bi = 0; bi < sizeof(BUILTIN_DESC) / sizeof(BUILTIN_DESC[0]); bi++)
                if (BUILTIN_DESC[bi].vid == vid && BUILTIN_DESC[bi].pid == pid &&
                    try_parse(BUILTIN_DESC[bi].desc, BUILTIN_DESC[bi].len, &prof)) {
                    src = BUILTIN_DESC[bi].name;
                    break;
                }
        }
        if (!src) {
            shim_log("no usable descriptor (len=%u vid=%04x pid=%04x) -> delegating to stock",
                     desclen, vid, pid);
            if (real_open) real_open(dev, remdev);
            return;
        }
        hid_profile_describe(&prof, descr, sizeof(descr));
        shim_log("device: %s (descriptor source: %s)", descr, src);
    }

    /* We take over this device. */
    memset(&id, 0, sizeof(id));
    id.bustype = BUS_BLUETOOTH;
    id.vendor  = U16(remdev, REMDEV_VENDOR);
    id.product = U16(remdev, REMDEV_PRODUCT);
    id.version = U16(remdev, REMDEV_VERSION);
    name = (const char *)FIELD(remdev, REMDEV_NAME);

    fd = uinput_create(&prof, name, &id);
    if (fd < 0) {
        shim_log("uinput_create failed -> delegating to stock (input will drop)");
        if (real_open) real_open(dev, remdev);
        return;
    }

    pthread_mutex_lock(&g_lock);
    m = find(dev);
    if (m) { uinput_destroy(m->fd); m->active = 0; }
    m = alloc_slot(dev);
    if (m) { m->fd = fd; m->prof = prof; }
    pthread_mutex_unlock(&g_lock);

    if (!m) { shim_log("managed table full -> closing node"); uinput_destroy(fd); return; }

    /* Keep the device struct consistent: mark uinput up, record fd. */
    U8(dev, DEV_UINPUT_FLAG) = 1;
    U32(dev, DEV_UINPUT_FD)  = (uint32_t)fd;
    shim_log("took over dev=%p as %s (fd=%d)", dev,
             prof.is_gamepad ? "gamepad" : "mouse", fd);
}

EXPORT void PmBtBsaifHidSendToInput(void *dev, void *msg)
{
    struct managed *m;
    uint8_t  rtype;
    uint16_t rlen;
    const unsigned char *rptr;

    rtype = U8(msg, MSG_REPORT_TYPE);
    rlen  = U16(msg, MSG_REPORT_LEN);
    rptr  = (const unsigned char *)PTR(msg, MSG_REPORT_PTR);

    if (g_shim_dump) {
        shim_dbg("SendToInput dev=%p type=%u len=%u", dev, rtype, rlen);
        if (rtype == REPORT_TYPE_INPUT && rptr && rlen)
            shim_hexdump("report", rptr, rlen);
    }

    pthread_mutex_lock(&g_lock);
    m = find(dev);
    pthread_mutex_unlock(&g_lock);

    if (m) {
        if (rtype == REPORT_TYPE_INPUT && rptr && rlen) {
            if (m->is_wiimote) wiimote_decode(m->fd, rptr, rlen);
            else               uinput_emit_report(m->fd, &m->prof, rptr, rlen);
        }
        return;                       /* managed: never fall through to stock */
    }

    if (real_send) real_send(dev, msg);   /* keyboards, consumer remote */
}

EXPORT void PmBtBsaifHidCloseUInput(void *dev)
{
    struct managed *m;

    pthread_mutex_lock(&g_lock);
    m = find(dev);
    if (m) {
        int fd = m->fd;
        m->active = 0;
        pthread_mutex_unlock(&g_lock);
        shim_log("closing managed dev=%p fd=%d", dev, fd);
        uinput_destroy(fd);
        U8(dev, DEV_UINPUT_FLAG) = 0;
        return;
    }
    pthread_mutex_unlock(&g_lock);

    if (real_close) real_close(dev);
}

/* Interpose the legacy PIN response.  A Wii Remote paired via 1+2 wants a PIN
 * equal to its own BD_ADDR reversed -- 6 raw bytes that can't be typed into the
 * pairing dialog.  When we see a Nintendo address here, substitute the correct
 * PIN (the user can type any dummy value in the dialog).  Everything else is
 * passed through untouched. */
EXPORT int PmBtBsaifPassKey(void *addr, void *pin, unsigned char pinlen)
{
    uint8_t written[6];

    if (g_shim_dump && addr) {
        const uint8_t *a = (const uint8_t *)addr;
        shim_dbg("PassKey addr=%02x:%02x:%02x:%02x:%02x:%02x pinlen=%u",
                 a[0],a[1],a[2],a[3],a[4],a[5], pinlen);
    }

    if (addr && wiimote_is_nintendo((const uint8_t *)addr, written)) {
        uint8_t wpin[6];
        wiimote_make_pin(written, wpin);
        shim_log("wiimote: injecting PIN for %02x:%02x:%02x:%02x:%02x:%02x",
                 written[0],written[1],written[2],written[3],written[4],written[5]);
        if (real_passkey) return real_passkey(addr, wpin, 6);
    }
    if (real_passkey) return real_passkey(addr, pin, pinlen);
    return 0;
}

/* Interpose the SSP acceptance path.  A Wii Remote Plus does SSP (not legacy
 * PIN), so the pairing response comes through here rather than PmBtBsaifPassKey.
 * A Wiimote is no-input/no-output -> the correct association model is Just
 * Works, so for a Nintendo address we force accept=1.  We also always log which
 * SSP model + address the stack chose, to see how the negotiation resolved. */
EXPORT int PmBtBsaifSspAccept(int accept, void *addr, unsigned int passkey)
{
    int is_nin = 0;
    uint8_t written[6];

    if (addr) is_nin = wiimote_is_nintendo((const uint8_t *)addr, written);

    if (addr) {
        const uint8_t *a = (const uint8_t *)addr;
        shim_log("SspAccept accept=%d passkey=%u nintendo=%d addr=%02x:%02x:%02x:%02x:%02x:%02x",
                 accept, passkey, is_nin, a[0],a[1],a[2],a[3],a[4],a[5]);
    }
    if (is_nin && !accept) {
        shim_log("wiimote: forcing SSP accept (Just Works)");
        accept = 1;
    }
    return real_sspaccept ? real_sspaccept(accept, addr, passkey) : 0;
}

/* Interpose the debug-zone gate.  The security (4) / GAP (3) / HIDH (27) trace
 * is compiled in but off by default, hiding the pairing negotiation.  In dump
 * mode, force those zones verbose so bt.log shows the full SSP/authentication
 * handshake.  Everything else defers to the real zone state. */
EXPORT int PmBtDbgGetZoneState(int zone)
{
    if (g_shim_dump && (zone == 3 || zone == 4 || zone == 0x1b))
        return 5;                       /* > any "if (N < level)" log threshold */
    return real_getzone ? real_getzone(zone) : 0;
}

/* Locate a Nintendo BD_ADDR inside a CSR indication struct: try a contiguous
 * 6-byte scan first, then the CSR-encoded form (lap@a, uap/nap@b) at the offset
 * pairs the passkey-ind (8,0xc) and bond-cfm (0xc,0x10) use.  Fills written[]. */
static int find_nintendo(const uint8_t *p, int len, uint8_t written[6])
{
    static const int pairs[][2] = { {8, 0xc}, {0xc, 0x10} };
    int off; unsigned pi;
    for (off = 0; off + 6 <= len; off++)
        if (wiimote_is_nintendo(p + off, written)) return 1;
    for (pi = 0; pi < 2; pi++) {
        uint32_t a = *(uint32_t *)(p + pairs[pi][0]);
        uint32_t b = *(uint32_t *)(p + pairs[pi][1]);
        uint32_t lap = a & 0xffffff;
        int k;
        for (k = 0; k < 2; k++) {
            uint8_t cand[6];
            uint32_t uap = b & 0xff;
            uint32_t nap = (k == 0) ? ((b >> 8) & 0xffff) : ((b >> 16) & 0xffff);
            cand[0]=(nap>>8)&0xff; cand[1]=nap&0xff; cand[2]=uap;
            cand[3]=(lap>>16)&0xff; cand[4]=(lap>>8)&0xff; cand[5]=lap&0xff;
            if (wiimote_is_nintendo(cand, written)) return 1;
        }
    }
    return 0;
}

/* The TouchPad's own BD_ADDR (from PmBtStack -X), written order. Used as a PIN
 * candidate for the sync-button style pairing. */
static const uint8_t HOST_ADDR[6] = { 0x00, 0x1d, 0xfe, 0x7e, 0x83, 0x05 };

/* Wii Remote legacy-PIN answer.  The correct PIN depends on the model + who
 * initiated + byte order, and our first guess (Wiimote addr, LSB-first) gets
 * Authentication Failure 0x5.  Cycle candidates across successive pairing
 * attempts and log each, so retrying a few times finds the one that
 * authenticates.  written[] = the Wiimote BD_ADDR in written order. */
static void wii_answer_pin(const uint8_t written[6])
{
    static int attempt = 0;
    /* Order matters: attempt #1 is the only one free of debond interference, so
     * put the most-likely candidate first.  webOS does a persistent bond =
     * sync-button semantics -> HOST address, LSB-first (per WiiBrew).  The 1+2
     * candidate (wiimote addr, LSB-first) already got a clean auth-fail 0x5. */
    static const int order[4] = { 2, 0, 3, 1 };
    uint8_t pin[6];
    int v = order[attempt % 4], i;
    const char *desc;
    attempt++;
    switch (v) {
    case 0:  for (i=0;i<6;i++) pin[i]=written[5-i];   desc="wiimote-LSBfirst(1+2)"; break;
    case 1:  for (i=0;i<6;i++) pin[i]=written[i];     desc="wiimote-MSBfirst";      break;
    case 2:  for (i=0;i<6;i++) pin[i]=HOST_ADDR[5-i]; desc="host-LSBfirst(sync)";   break;
    default: for (i=0;i<6;i++) pin[i]=HOST_ADDR[i];   desc="host-MSBfirst";         break;
    }
    shim_log("wiimote: PIN attempt #%d variant %d (%s) = "
             "%02x %02x %02x %02x %02x %02x", attempt, v, desc,
             pin[0],pin[1],pin[2],pin[3],pin[4],pin[5]);
    if (real_passkey) real_passkey((void *)written, pin, 6);
}

/* Interpose the legacy PIN *request* handler.  The stock flow raises a UI dialog
 * and only sends the PIN after the user types it -- far too slow for a Wii
 * Remote, whose PIN-request window is short, so bonding fails 0x18 before the
 * response is ever sent.  Here we answer instantly: for a Nintendo address we
 * compute the address-derived PIN and send it straight to the stack via
 * PmBtBsaifPassKey (a direct CsrPutMessage, no engine queue), skipping the
 * dialog entirely.  Non-Nintendo devices keep the normal flow. */
EXPORT void handleScPasskeyInd(void *msg)
{
    void *ind = msg ? *(void **)((char *)msg + 4) : 0;
    shim_log("handleScPasskeyInd FIRED msg=%p ind=%p", msg, ind);

    if (ind && real_passkey) {
        uint8_t written[6];
        if (g_shim_dump) shim_hexdump("passkey-ind", (const unsigned char *)ind, 0x30);
        if (find_nintendo((const uint8_t *)ind, 0x30, written)) {
            shim_log("wiimote: passkey-ind for %02x:%02x:%02x:%02x:%02x:%02x",
                     written[0],written[1],written[2],written[3],written[4],written[5]);
            wii_answer_pin(written);
            return;                     /* skip the slow dialog entirely */
        }
    }
    if (real_scpasskeyind) real_scpasskeyind(msg);
}

/* Bond confirm.  A failed bond leaves a stale/bad link key cached, so the next
 * pairing re-authenticates with it and fails 0x5 instantly -- no fresh PIN
 * request, which blocks our PIN cycling.  When a bond to a Nintendo device
 * FAILS, forget it (PmBtBsaifDebond) so the next attempt starts clean and the
 * next PIN candidate is tried. */
EXPORT void handleScBondCfm(void *msg)
{
    void *ind = msg ? *(void **)((char *)msg + 4) : 0;
    if (ind && p_debond) {
        const uint8_t *p = (const uint8_t *)ind;
        uint16_t result = *(uint16_t *)(p + 2);   /* 0 = success */
        uint8_t written[6];
        if (find_nintendo(p, 0x30, written)) {
            shim_log("handleScBondCfm: Nintendo %02x:%02x:%02x:%02x:%02x:%02x result=0x%x",
                     written[0],written[1],written[2],written[3],written[4],written[5], result);
            if (result != 0) {
                if (real_scbondcfm) real_scbondcfm(msg);   /* report failure normally */
                shim_log("wiimote: bond failed -> debonding to clear cached key");
                p_debond(written);
                return;
            }
            shim_log("wiimote: bond SUCCEEDED");
        }
    }
    if (real_scbondcfm) real_scbondcfm(msg);
}

/* SSP passkey indication.  The Wii Remote uses legacy PIN (above), but interpose
 * this too so we can see if any device takes the SSP path here. */
EXPORT void handleScSspPasskeyInd(void *msg)
{
    void *ind = msg ? *(void **)((char *)msg + 4) : 0;
    shim_log("handleScSspPasskeyInd FIRED msg=%p ind=%p", msg, ind);
    if (g_shim_dump && ind) shim_hexdump("ssp-passkey-ind", (const unsigned char *)ind, 0x30);
    if (real_scssppasskeyind) real_scssppasskeyind(msg);
}
