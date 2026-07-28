/*
 * btctl — minimal Bluetooth control for the TouchPad (no BlueZ userspace on device).
 *
 * Talks to the kernel stack through a raw HCI socket. Enough to bring the
 * adapter up, prove the radio works, scan, and later drive HIDP.
 *
 * Build: arm-linux-gnueabi-gcc -static -O2 -march=armv7-a -o btctl btctl.c
 * Usage: btctl up | down | info | scan [secs]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <stdint.h>

#define AF_BLUETOOTH_   31
#define BTPROTO_HCI     1

#define HCIDEVUP        _IOW('H', 201, int)
#define HCIDEVDOWN      _IOW('H', 202, int)
#define HCIDEVRESET     _IOW('H', 203, int)
#define HCIGETDEVINFO   _IOR('H', 211, int)

#define HCI_MAX_DEV     16
#define HCI_CHANNEL_RAW 0

typedef struct { uint8_t b[6]; } __attribute__((packed)) bdaddr_t;

struct sockaddr_hci {
    sa_family_t hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct hci_dev_stats {
    uint32_t err_rx, err_tx, cmd_tx, evt_rx, acl_tx, acl_rx, sco_tx, sco_rx,
             byte_rx, byte_tx;
};

struct hci_dev_info {
    uint16_t dev_id;
    char     name[8];
    bdaddr_t bdaddr;
    uint32_t flags;
    uint8_t  type;
    uint8_t  features[8];
    uint32_t pkt_type, link_policy, link_mode;
    uint16_t acl_mtu; uint16_t acl_pkts;
    uint16_t sco_mtu; uint16_t sco_pkts;
    struct hci_dev_stats stat;
};

/* HCI filter so we can see all events on the raw socket */
struct hci_filter { uint32_t type_mask; uint32_t event_mask[2]; uint16_t opcode; };
#define HCI_FILTER 2
#define SOL_HCI_   0

static void pr_addr(bdaddr_t *a)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           a->b[5], a->b[4], a->b[3], a->b[2], a->b[1], a->b[0]);
}

static int hci_socket(int dev)
{
    struct sockaddr_hci sa;
    int s = socket(AF_BLUETOOTH_, SOCK_RAW, BTPROTO_HCI);
    if (s < 0) { perror("socket(AF_BLUETOOTH)"); return -1; }
    if (dev < 0) return s;
    memset(&sa, 0, sizeof(sa));
    sa.hci_family = AF_BLUETOOTH_;
    sa.hci_dev = dev;
    sa.hci_channel = HCI_CHANNEL_RAW;
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind"); close(s); return -1;
    }
    return s;
}

static int cmd_info(int dev)
{
    struct hci_dev_info di;
    int s = hci_socket(-1);
    if (s < 0) return 1;
    memset(&di, 0, sizeof(di));
    di.dev_id = dev;
    if (ioctl(s, HCIGETDEVINFO, &di) < 0) { perror("HCIGETDEVINFO"); close(s); return 1; }
    printf("hci%d  %s  addr=", di.dev_id, di.name);
    pr_addr(&di.bdaddr);
    printf("\n  flags=0x%08x %s\n", di.flags, (di.flags & 1) ? "UP" : "DOWN");
    printf("  acl_mtu=%d:%d sco_mtu=%d:%d\n", di.acl_mtu, di.acl_pkts, di.sco_mtu, di.sco_pkts);
    printf("  stats: cmd_tx=%u evt_rx=%u err_rx=%u err_tx=%u bytes rx/tx=%u/%u\n",
           di.stat.cmd_tx, di.stat.evt_rx, di.stat.err_rx, di.stat.err_tx,
           di.stat.byte_rx, di.stat.byte_tx);
    close(s);
    return 0;
}

int main(int argc, char **argv)
{
    const char *op = argc > 1 ? argv[1] : "info";
    int dev = 0, s, r;

    if (!strcmp(op, "up") || !strcmp(op, "down") || !strcmp(op, "reset")) {
        int req = !strcmp(op, "up") ? HCIDEVUP :
                  !strcmp(op, "down") ? HCIDEVDOWN : HCIDEVRESET;
        s = hci_socket(-1);
        if (s < 0) return 1;
        printf("== hci%d %s ...\n", dev, op);
        fflush(stdout);
        r = ioctl(s, req, dev);
        if (r < 0) {
            printf("   FAILED: %s\n", strerror(errno));
            if (errno == ETIMEDOUT)
                printf("   (chip did not answer HCI reset — BCSP link not established)\n");
            close(s);
            return 1;
        }
        printf("   OK\n");
        close(s);
        return cmd_info(dev);
    }
    if (!strcmp(op, "info")) return cmd_info(dev);

    printf("usage: btctl up|down|reset|info\n");
    return 1;
}
