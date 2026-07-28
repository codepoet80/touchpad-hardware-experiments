/*
 * btbridge — attach the TouchPad's CSR BlueCore6 to the Linux Bluetooth stack.
 *
 * The chip speaks BCSP over Palm's custom /dev/bt_uart, which is a *misc* char
 * device, not a tty — so hci_uart's N_HCI line discipline cannot be attached to
 * it directly. This bridges the gap: it opens a pty, puts N_HCI + BCSP on the
 * pty slave (so the kernel's hci_bcsp does all the framing, CRC and retries),
 * and then just shovels raw bytes between the pty master and /dev/bt_uart.
 *
 * Result: hci0 appears and the normal Bluetooth stack works, including HIDP —
 * which creates real /dev/input/eventN nodes for HID devices of any class.
 *
 * Palm's stack must be stopped first (it owns the UART and the chip):
 *     luna-send -i palm://com.palm.btmonitor/monitor/radiooff '{}'
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -march=armv7-a -o btbridge btbridge.c
 * Run:   ./btbridge [--speed N] [--flow hw|none] [--noreset]
 */
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>

/* Palm hsuart (include/linux/hsuart.h) */
struct hsuart_mode { int speed; int flags; };
struct hsuart_stat { unsigned long tx_bytes, rx_bytes, rx_dropped; };
#define HSUART_IOCTL_GET_UARTMODE  _IOR('h', 0x04, struct hsuart_mode)
#define HSUART_IOCTL_SET_UARTMODE  _IOW('h', 0x05, struct hsuart_mode)
#define HSUART_IOCTL_RESET_UART    _IO ('h', 0x06)
#define HSUART_IOCTL_GET_STATS     _IOW('h', 0x08, struct hsuart_stat)
#define HSUART_IOCTL_TX_DRAIN      _IOW('h', 0x0b, int)
#define HSUART_IOCTL_RX_FLOW       _IOW('h', 0x0d, int)
#define HSUART_IOCTL_FLUSH         _IOW('h', 0x0e, int)
#define HSUART_RX_FLOW_ON          1
#define HSUART_MODE_LOOPBACK       (1 << 8)
#define HSUART_RX_FIFO   (1 << 0)
#define HSUART_TX_FIFO   (1 << 1)
#define HSUART_TX_QUEUE  (1 << 2)
#define HSUART_RX_QUEUE  (1 << 3)
#define HSUART_MODE_FLOW_CTRL_NONE (0 << 0)
#define HSUART_MODE_FLOW_CTRL_HW   (1 << 0)
#define HSUART_MODE_FLOW_CTRL_SW   (2 << 0)
#define HSUART_MODE_FLOW_STATE_ASSERT (0 << 9)
#define HSUART_MODE_PARITY_NONE    (0 << 4)

/* hci_uart (drivers/bluetooth/hci_uart.h) */
#define HCIUARTSETPROTO   _IOW('U', 200, int)
#define HCIUARTGETDEVICE  _IOR('U', 202, int)
#define HCI_UART_BCSP     1
#ifndef N_HCI
#define N_HCI 15
#endif


/* ---- BCSP link establishment (what hciattach does before attaching N_HCI) ----
 * The kernel's hci_bcsp only *answers* conf packets; it never starts the
 * handshake. Link-establishment packets are unreliable, un-CRC'd, channel 1,
 * 4-byte payload, SLIP-framed.
 */
static const unsigned char LE_SYNC[4]      = { 0xda, 0xdc, 0xed, 0xed };
static const unsigned char LE_SYNC_RESP[4] = { 0xac, 0xaf, 0xef, 0xee };
static const unsigned char LE_CONF[4]      = { 0xad, 0xef, 0xac, 0xed };
static const unsigned char LE_CONF_RESP[4] = { 0xde, 0xad, 0xd0, 0xd0 };

static void slip_put(unsigned char *out, int *n, unsigned char b)
{
    if (b == 0xc0)      { out[(*n)++] = 0xdb; out[(*n)++] = 0xdc; }
    else if (b == 0xdb) { out[(*n)++] = 0xdb; out[(*n)++] = 0xdd; }
    else                  out[(*n)++] = b;
}

/* BCSP CRC-CCITT, exactly as drivers/bluetooth/hci_bcsp.c computes it */
static const unsigned short bcsp_crc_tbl[] = {
    0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
    0x8408, 0x9489, 0xa50a, 0xb58b, 0xc60c, 0xd68d, 0xe70e, 0xf78f
};
static void crc_update(unsigned short *crc, unsigned char d)
{
    unsigned short reg = *crc;
    reg = (reg >> 4) ^ bcsp_crc_tbl[(reg ^ d) & 0x000f];
    reg = (reg >> 4) ^ bcsp_crc_tbl[(reg ^ (d >> 4)) & 0x000f];
    *crc = reg;
}
static unsigned short bitrev16(unsigned short x)
{
    unsigned short r = 0; int i;
    for (i = 0; i < 16; i++) { r = (r << 1) | (x & 1); x >>= 1; }
    return r;
}

static int le_use_crc = 1;
static void send_le(int fd, const unsigned char *payload)
{
    unsigned char hdr[4], frame[64];
    int n = 0, i;
    unsigned short crc = 0xffff;
    hdr[0] = le_use_crc ? 0x40 : 0x00; /* unreliable; CRC optional */
    hdr[1] = (4 << 4) | 0x01;         /* len low nibble | channel 1 */
    hdr[2] = 0x00;                    /* len high bits */
    hdr[3] = ~(hdr[0] + hdr[1] + hdr[2]);
    frame[n++] = 0xc0;
    for (i = 0; i < 4; i++) { slip_put(frame, &n, hdr[i]); crc_update(&crc, hdr[i]); }
    for (i = 0; i < 4; i++) { slip_put(frame, &n, payload[i]); crc_update(&crc, payload[i]); }
    if (le_use_crc) {
        crc = bitrev16(crc);
        slip_put(frame, &n, (crc >> 8) & 0xff);
        slip_put(frame, &n, crc & 0xff);
    }
    frame[n++] = 0xc0;
    {
        int w;
        static int shown_tx = 0;
        if (shown_tx < 2) {
            int k; printf("   TX[%d]:", n);
            for (k = 0; k < n; k++) printf(" %02x", frame[k]);
            printf("\n"); shown_tx++;
        }
        w = write(fd, frame, n);
        {
        static int complained = 0;
        if (w < 0 && !complained) { printf("   ! send_le write: %s\n", strerror(errno)); complained = 1; }
        }
    }
}

/* Feed raw bytes; returns 1 and fills pkt[4] when a complete 4-byte LE payload
   frame has been unslipped. */
static int le_feed(unsigned char byte, unsigned char *pkt)
{
    static unsigned char buf[64];
    static int len = 0, esc = 0, in = 0;
    if (byte == 0xc0) {
        int done = 0;
        /* header is 4 bytes; payload length lives in hdr[1]>>4 | hdr[2]<<4;
           a trailing 2-byte CRC is present when hdr[0] & 0x40. */
        if (in && len >= 8) {
            int plen = (buf[1] >> 4) | (buf[2] << 4);
            if (plen == 4 && len >= 8) { memcpy(pkt, buf + 4, 4); done = 1; }
        }
        in = 1; len = 0; esc = 0;
        return done;
    }
    if (!in) return 0;
    if (esc) { byte = (byte == 0xdc) ? 0xc0 : (byte == 0xdd) ? 0xdb : byte; esc = 0; }
    else if (byte == 0xdb) { esc = 1; return 0; }
    if (len < (int)sizeof(buf)) buf[len++] = byte;
    return 0;
}

static int bcsp_link_establish(int uart, int timeout_s)
{
    int state = 0;            /* 0 = shy (want sync), 1 = curious (want conf) */
    int i, ticks = 0, dumped = 0;
    printf("== BCSP link establishment\n"); fflush(stdout);
    for (ticks = 0; ticks < timeout_s * 10; ticks++) {
        unsigned char buf[512], pkt[4];
        fd_set rs; struct timeval tv = { 0, 100000 };
        int n;
        send_le(uart, state == 0 ? LE_SYNC : LE_CONF);
        if (ticks % 10 == 0) { printf("   .. t=%ds state=%s\n", ticks/10, state ? "curious" : "shy"); fflush(stdout); }
        FD_ZERO(&rs); FD_SET(uart, &rs);
        if (select(uart + 1, &rs, 0, 0, &tv) > 0) {
            n = read(uart, buf, sizeof(buf));
            if (n > 0 && dumped < 6) {
                int k; printf("   RX[%d]:", n);
                for (k = 0; k < n && k < 32; k++) printf(" %02x", buf[k]);
                printf("\n"); fflush(stdout); dumped++;
            }
            for (i = 0; i < n; i++) {
                if (!le_feed(buf[i], pkt)) continue;
                if (!memcmp(pkt, LE_SYNC, 4)) {
                    printf("   <- SYNC (peer reset); replying SYNC RESP\n");
                    send_le(uart, LE_SYNC_RESP);
                } else if (!memcmp(pkt, LE_SYNC_RESP, 4)) {
                    if (state == 0) { printf("   <- SYNC RESP; link synced\n"); state = 1; }
                } else if (!memcmp(pkt, LE_CONF, 4)) {
                    printf("   <- CONF; replying CONF RESP\n");
                    send_le(uart, LE_CONF_RESP);
                } else if (!memcmp(pkt, LE_CONF_RESP, 4)) {
                    printf("   <- CONF RESP; >>> BCSP LINK ACTIVE\n");
                    return 0;
                }
            }
        }
    }
    {
        struct hsuart_stat st;
        memset(&st, 0, sizeof(st));
        if (ioctl(uart, HSUART_IOCTL_GET_STATS, &st) == 0)
            printf("   uart stats: tx=%lu rx=%lu rx_dropped=%lu\n",
                   st.tx_bytes, st.rx_bytes, st.rx_dropped);
        else
            printf("   (GET_STATS failed: %s)\n", strerror(errno));
    }
    printf("   ! timed out (chip never answered SYNC)\n");
    return -1;
}

static volatile int running = 1;
static void onsig(int s) { (void)s; running = 0; }

static void sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) { printf("  (skip %s: %s)\n", path, strerror(errno)); return; }
    if (write(fd, val, strlen(val)) < 0) printf("  ! %s: %s\n", path, strerror(errno));
    else printf("  %s <- %s\n", path, val);
    close(fd);
}

int main(int argc, char **argv)
{
    int uart, mfd, sfd, i, ldisc = N_HCI, proto = HCI_UART_BCSP, devid = -1;
    int speed = 115200, flow = (HSUART_MODE_FLOW_CTRL_SW | HSUART_MODE_FLOW_STATE_ASSERT), doreset = 1;
    int skip_le = 0, force = 0, letmo = 10, loopback = 0;
    struct hsuart_mode hm;
    struct termios t;
    char *slave;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--speed") && i + 1 < argc) speed = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--flow") && i + 1 < argc)
        {
            const char *f = argv[++i];
            flow = !strcmp(f, "hw")   ? HSUART_MODE_FLOW_CTRL_HW :
                   !strcmp(f, "sw")   ? (HSUART_MODE_FLOW_CTRL_SW | HSUART_MODE_FLOW_STATE_ASSERT) :
                                        HSUART_MODE_FLOW_CTRL_NONE;
        }
        else if (!strcmp(argv[i], "--noreset")) doreset = 0;
        else if (!strcmp(argv[i], "--skip-le")) skip_le = 1;
        else if (!strcmp(argv[i], "--loopback")) loopback = 1;
        else if (!strcmp(argv[i], "--nocrc")) le_use_crc = 0;
        else if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--letimeout") && i + 1 < argc) letmo = atoi(argv[++i]);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, onsig);
    signal(SIGTERM, onsig);

    printf("== powering the radio\n");
    if (doreset) {
        /* bluetooth_power only acts on a *change*, so bounce it */
        sysfs_write("/sys/user_hw/pins/bt/reset/level", "0");
        sysfs_write("/sys/devices/platform/bt_power.0/bluetooth_power", "0");
        usleep(100000);
        sysfs_write("/sys/devices/platform/bt_power.0/bluetooth_power", "1");
        usleep(100000);
        sysfs_write("/sys/user_hw/pins/bt/reset/level", "1");
        usleep(200000);
    }

    printf("== opening /dev/bt_uart\n");
    uart = open("/dev/bt_uart", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (uart < 0) { printf("  ! %s (is Palm's stack still running?)\n", strerror(errno)); return 1; }
    if (ioctl(uart, HSUART_IOCTL_RESET_UART) < 0)
        printf("  ! RESET_UART: %s\n", strerror(errno));
    else
        printf("  uart reset\n");
    memset(&hm, 0, sizeof(hm));
    hm.speed = speed;
    hm.flags = flow | HSUART_MODE_PARITY_NONE;
    if (ioctl(uart, HSUART_IOCTL_SET_UARTMODE, &hm) < 0)
        printf("  ! SET_UARTMODE: %s\n", strerror(errno));
    if (ioctl(uart, HSUART_IOCTL_GET_UARTMODE, &hm) == 0)
        printf("  uart: speed=%d flags=0x%x\n", hm.speed, hm.flags);
    i = HSUART_RX_FIFO | HSUART_TX_FIFO | HSUART_TX_QUEUE | HSUART_RX_QUEUE;
    ioctl(uart, HSUART_IOCTL_FLUSH, &i);
    /* RX_FLOW takes flow-control MODE bits, not a boolean — passing 1 here
       would silently switch to HW flow control and stall TX on CTS. */
    i = flow;
    if (ioctl(uart, HSUART_IOCTL_RX_FLOW, &i) < 0)
        printf("  ! RX_FLOW(0x%x): %s\n", flow, strerror(errno));
    else
        printf("  rx flow set to 0x%x\n", flow);
    if (ioctl(uart, HSUART_IOCTL_GET_UARTMODE, &hm) == 0)
        printf("  uart now: speed=%d flags=0x%x\n", hm.speed, hm.flags);

    if (loopback) {
        unsigned char probe[8] = { 0xc0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0xc0 };
        unsigned char rb[64]; int n, k;
        fd_set rs; struct timeval tv = { 1, 0 };
        printf("== LOOPBACK test (proves the TX path)\n");
        memset(&hm, 0, sizeof(hm));
        hm.speed = speed; hm.flags = flow | HSUART_MODE_LOOPBACK;
        if (ioctl(uart, HSUART_IOCTL_SET_UARTMODE, &hm) < 0)
            printf("  ! set loopback: %s\n", strerror(errno));
        i = HSUART_RX_FIFO | HSUART_TX_FIFO | HSUART_TX_QUEUE | HSUART_RX_QUEUE;
        ioctl(uart, HSUART_IOCTL_FLUSH, &i);
        n = write(uart, probe, sizeof(probe));
        printf("  wrote %d bytes\n", n);
        { int t = 1000; int r = ioctl(uart, HSUART_IOCTL_TX_DRAIN, &t);
          printf("  TX_DRAIN(1000ms) -> %d %s\n", r, r < 0 ? strerror(errno) : "(drained: TX REACHES THE WIRE)"); }
        /* TX_DRAIN can block forever if the transmitter is wedged; skip it */
        FD_ZERO(&rs); FD_SET(uart, &rs);
        if (select(uart + 1, &rs, 0, 0, &tv) > 0) {
            n = read(uart, rb, sizeof(rb));
            printf("  read back %d:", n);
            for (k = 0; k < n && k < 16; k++) printf(" %02x", rb[k]);
            printf("\n  >>> TX PATH WORKS\n");
        } else {
            printf("  nothing looped back — TX PATH IS DEAD\n");
        }
        return 0;
    }

    if (!skip_le && bcsp_link_establish(uart, letmo) < 0 && !force)
        { printf("   (use --force to attach anyway)\n"); return 1; }

    printf("== creating pty for the kernel's BCSP driver\n");
    mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if (mfd < 0 || grantpt(mfd) || unlockpt(mfd)) {
        printf("  ! pty: %s\n", strerror(errno)); return 1;
    }
    slave = ptsname(mfd);
    printf("  pty slave: %s\n", slave);
    sfd = open(slave, O_RDWR | O_NOCTTY);
    if (sfd < 0) { printf("  ! open slave: %s\n", strerror(errno)); return 1; }

    /* raw both ends so nothing mangles the byte stream */
    if (tcgetattr(sfd, &t) == 0) { cfmakeraw(&t); tcsetattr(sfd, TCSANOW, &t); }
    if (tcgetattr(mfd, &t) == 0) { cfmakeraw(&t); tcsetattr(mfd, TCSANOW, &t); }

    printf("== attaching N_HCI + BCSP to the pty\n");
    if (ioctl(sfd, TIOCSETD, &ldisc) < 0) {
        printf("  ! TIOCSETD N_HCI: %s (is hci_uart.ko loaded?)\n", strerror(errno));
        return 1;
    }
    if (ioctl(sfd, HCIUARTSETPROTO, proto) < 0) {
        printf("  ! HCIUARTSETPROTO BCSP: %s\n", strerror(errno));
        return 1;
    }
    if (ioctl(sfd, HCIUARTGETDEVICE, &devid) == 0)
        printf("  >>> attached as hci%d\n", devid);
    else
        printf("  attached (device id unknown)\n");

    printf("== bridging (Ctrl-C to stop)\n");
    fflush(stdout);
    {
    unsigned long to_chip = 0, from_chip = 0; int ticks = 0, shown = 0;
    while (running) {
        unsigned char buf[1024];
        fd_set rs;
        struct timeval tv = { 1, 0 };
        int mx = uart > mfd ? uart : mfd, n;
        FD_ZERO(&rs); FD_SET(uart, &rs); FD_SET(mfd, &rs);
        if (select(mx + 1, &rs, 0, 0, &tv) <= 0) {
            if (++ticks % 5 == 0) {
                printf("   [stats] to_chip=%lu from_chip=%lu\n", to_chip, from_chip);
                fflush(stdout);
            }
            continue;
        }
        if (FD_ISSET(uart, &rs)) {                 /* chip -> kernel */
            n = read(uart, buf, sizeof(buf));
            if (n > 0) {
                from_chip += n;
                if (shown < 3) { int k; printf("   CHIP->:"); for (k=0;k<n&&k<24;k++) printf(" %02x", buf[k]); printf("\n"); fflush(stdout); shown++; }
                if (write(mfd, buf, n) < 0) break;
            }
            else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        }
        if (FD_ISSET(mfd, &rs)) {                  /* kernel -> chip */
            n = read(mfd, buf, sizeof(buf));
            if (n > 0) {
                to_chip += n;
                if (to_chip <= 64) { int k; printf("   ->CHIP:"); for (k=0;k<n&&k<24;k++) printf(" %02x", buf[k]); printf("\n"); fflush(stdout); }
                if (write(uart, buf, n) < 0) break;
            }
            else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        }
    }
    }
    printf("== shutting down\n");
    close(sfd); close(mfd); close(uart);
    return 0;
}
