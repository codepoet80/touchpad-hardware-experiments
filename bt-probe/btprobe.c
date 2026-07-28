/*
 * btprobe — talk HCI directly to the TouchPad's CSR BlueCore over /dev/bt_uart
 *
 * Palm's stack (PmBtStack) normally owns this UART; run only with the BT radio
 * off. Sends an H4-framed HCI Reset and dumps whatever comes back. If we get a
 * Command Complete, we can drive the radio ourselves.
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -march=armv7-a -o btprobe btprobe.c
 * Run:   ./btprobe [--reset] [--send]
 *          --reset  toggle the chip's reset GPIO first
 *          --send   actually transmit HCI Reset (otherwise just listens)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>

/* from the tenderloin hsuart driver (macros recovered from PmBtStack asserts) */
struct hsuart_mode { int speed; int flags; };
#define HSUART_IOCTL_GET_UARTMODE  _IOR('h', 0x04, struct hsuart_mode)
#define HSUART_IOCTL_SET_UARTMODE  _IOW('h', 0x05, struct hsuart_mode)
#define HSUART_IOCTL_CLEAR_FIFO    _IOW('h', 0x06, int)
#define HSUART_IOCTL_TX_DRAINED    _IOW('h', 0x07, int)

static void wr(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) { printf("  ! open %s: %s\n", path, strerror(errno)); return; }
    if (write(fd, val, strlen(val)) < 0)
        printf("  ! write %s: %s\n", path, strerror(errno));
    else
        printf("  %s <- %s\n", path, val);
    close(fd);
}

static void dump(const unsigned char *b, int n, const char *tag)
{
    int i;
    printf("%s [%d bytes]:", tag, n);
    for (i = 0; i < n; i++) {
        if (i % 16 == 0) printf("\n  ");
        printf("%02x ", b[i]);
    }
    printf("\n");
    /* decode an HCI Command Complete for Reset if present */
    for (i = 0; i + 6 < n; i++)
        if (b[i] == 0x04 && b[i+1] == 0x0e && b[i+4] == 0x03 && b[i+5] == 0x0c)
            printf("  >>> HCI Command Complete for RESET, status=0x%02x %s\n",
                   b[i+6], b[i+6] ? "(error)" : "(SUCCESS)");
}

int main(int argc, char **argv)
{
    int fd, i, doreset = 0, dosend = 0, n, total = 0;
    struct hsuart_mode m;
    unsigned char buf[4096];
    /* H4: 0x01 = command packet; HCI_Reset = OGF 0x03 OCF 0x0003 -> 0x0c03 */
    const unsigned char hci_reset[] = { 0x01, 0x03, 0x0c, 0x00 };

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--reset")) doreset = 1;
        if (!strcmp(argv[i], "--send"))  dosend = 1;
    }

    if (doreset) {
        printf("== toggling BT reset GPIO\n");
        wr("/sys/user_hw/pins/bt/reset/level", "0");
        usleep(200000);
        wr("/sys/user_hw/pins/bt/reset/level", "1");
        usleep(500000);
    }

    printf("== opening /dev/bt_uart (nonblocking)\n"); fflush(stdout);
    fd = open("/dev/bt_uart", O_RDWR | O_NONBLOCK);
    if (fd < 0) { printf("  ! %s\n", strerror(errno)); return 1; }
    printf("  opened, fd=%d\n", fd); fflush(stdout);

    memset(&m, 0, sizeof(m));
    if (ioctl(fd, HSUART_IOCTL_GET_UARTMODE, &m) == 0)
        printf("  uart mode: speed=%d flags=0x%x\n", m.speed, m.flags);
    else
        printf("  ! GET_UARTMODE: %s\n", strerror(errno));

    if (dosend) {
        int fl = 1;
        ioctl(fd, HSUART_IOCTL_CLEAR_FIFO, &fl);
        printf("== sending HCI Reset (H4: 01 03 0c 00)\n");
        n = write(fd, hci_reset, sizeof(hci_reset));
        printf("  wrote %d (%s)\n", n, n < 0 ? strerror(errno) : "ok");
    }

    printf("== listening 3s\n");
    for (i = 0; i < 30 && total < (int)sizeof(buf); i++) {
        fd_set rs;
        struct timeval tv = { 0, 100000 };
        FD_ZERO(&rs); FD_SET(fd, &rs);
        if (select(fd + 1, &rs, 0, 0, &tv) > 0) {
            n = read(fd, buf + total, sizeof(buf) - total);
            if (n > 0) total += n;
        }
    }
    if (total) dump(buf, total, "RX");
    else printf("  (nothing received)\n");

    close(fd);
    return total ? 0 : 2;
}
