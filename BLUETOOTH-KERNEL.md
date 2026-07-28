# Running the Linux Bluetooth stack on a stock webOS TouchPad

Status: **most of the way there.** The kernel side works completely; the last
gap is that userspace writes to `/dev/bt_uart` never reach the chip.

This matters because Palm's closed stack only translates *keyboard* HID reports
(see `README.md`), so Bluetooth gamepads can never work through it. The kernel's
HIDP driver has no such limitation — it creates a real `/dev/input/eventN` for
any HID device, which `padkeys` then turns into system-wide input.

## What works

**Kernel modules build and load on the stock kernel.** Nobody appears to have
done this before — LuneOS never got Bluetooth working on tenderloin, and the
stock `tenderloin_defconfig` has `# CONFIG_BT is not set`.

- Source: `linuxkernel-2.6.35.tgz` + `linuxkernel-2.6.35.patch.tar.gz` from
  [archive.org/details/hp-webos-oss](https://archive.org/details/hp-webos-oss).
  The patch applies cleanly and yields exactly `2.6.35-palm-tenderloin`.
- Toolchain: gcc 4.9.4 from
  [kernel.org crosstool](https://mirrors.edge.kernel.org/pub/tools/crosstool/)
  (modern GCC can't build 2.6.35). It needs `libmpfr.so.4`, which no longer ships
  on current distros — build MPFR 3.1.6 from source and set `LD_LIBRARY_PATH`.
- Config: pull the device's own `/proc/config.gz` (`CONFIG_IKCONFIG_PROC=y`), then
  set `CONFIG_BT=m`, `BT_L2CAP=m`, `BT_HIDP=m`, `BT_HCIVHCI=m`, `BT_HCIUART=m`,
  `BT_HCIUART_BCSP=y`.
- `CONFIG_MODVERSIONS` is **off**, so only the vermagic string must match:
  `2.6.35-palm-tenderloin SMP preempt mod_unload ARMv7`. It matches exactly.
- `CONFIG_CRC_CCITT=y` is already built in (BCSP needs it).

Loading them gives a fully working stack:

```
Bluetooth: Core ver 2.15
Bluetooth: HCI device and connection manager initialized
Bluetooth: L2CAP ver 2.14
Bluetooth: HIDP (Human Interface Emulation) ver 1.2
Bluetooth: Virtual HCI driver ver 1.3
Bluetooth: HCI BCSP protocol initialized
```

**The radio is a CSR BlueCore6 speaking BCSP** on `/dev/bt_uart`. Palm runs
`PmBtStack -C /dev/bt_uart -X <bdaddr> -F 26000 -U 3686400` (26 MHz crystal,
3686400 baud target; the chip's ROM rate is 115200).

**`hci0` registers.** `/dev/bt_uart` is a *misc* char device, not a tty, so
`hci_uart`'s `N_HCI` line discipline can't attach to it. `btbridge.c` works
around this: it opens a pty, puts `N_HCI` + BCSP on the pty slave (so the
kernel's `hci_bcsp` does all framing, CRC and retries), and shovels raw bytes
between the pty master and `/dev/bt_uart`. `/sys/class/bluetooth/hci0` appears.

**The chip is alive and talking.** It continuously transmits BCSP SYNC:

```
c0 40 41 00 7e  da dc ed ed  a9 7a c0
   |  |  |  |   ^^^^^^^^^^^  ^^^^^ CRC-CCITT (bitrev16, big-endian)
   |  |  |  checksum         SYNC payload
   |  |  length high
   |  channel 1 (link establishment) | length low nibble
   reliable=0, CRC=1
```

Our CRC implementation reproduces `a9 7a` exactly, and our transmitted frames
are byte-identical to the chip's own — so framing, checksum and CRC are all
provably correct.

## The remaining blocker: TX never drains

Writes to `/dev/bt_uart` are accepted (byte counts rise in
`HSUART_IOCTL_GET_STATS`) but never reach the wire: `HSUART_IOCTL_TX_DRAIN`
**blocks forever**, and the chip never reacts to anything we send — it just keeps
repeating SYNC. Receive works perfectly at the same time.

Ruled out:
- All baud rates (38400/115200/230400/460800/921600/1228800/3686400). Note the
  MSM driver silently coerces unsupported rates to 115200, so 38400 is a no-op.
- All three flow-control modes. `FLOW_CTRL_NONE` is actively harmful: it
  un-muxes RTS (GPIO 56) to a plain output driven **high**, which mutes the chip.
  `FLOW_CTRL_SW | FLOW_STATE_ASSERT` (flags `0x2`) is the right bring-up setting.
- `HSUART_IOCTL_RX_FLOW` takes flow-control *mode bits*, not a boolean — passing
  `1` silently switches to hardware flow control.
- Correct power sequencing: `bluetooth_power` only acts on a *change*, so it must
  be bounced 0→1; reset (GPIO 138, active low) toggled 1→0→1 with ~100 ms.
- Asserting BT_WAKE (GPIO 131) by hand via `/sys/class/gpio`.
- `HSUART_IOCTL_RESET_UART`.

Also ruled out (tested after this doc's first draft):
- **A fresh reboot with the BT radio left off**, so `PmBtStack` never opened the
  port and `hsuart_uart_port_init()` ran for the first time under our control.
  Identical result. This kills the "Palm left the port in a bad state" theory.
- **Local echo** — we transmit 10-byte CRC-less frames (`c0 00 41 00 be da dc ed
  ed c0`) and receive 12-byte CRC-bearing ones (`c0 40 41 00 7e … a9 7a c0`), so
  the SYNC stream is unambiguously coming from the chip, not looped back.
- The ioctl path *does* configure TX flow: `msm_hsuart_set_flow()` applies to both
  directions when the direction bits are `RX_TX` (0), and `set_tx_flow(0)` clears
  `UART_MR1_CTS_CTL`. So TX is not CTS-gated. (`HSUART_IOCTL_RESET_UART` is a
  no-op — the handler only logs.)

The picture is therefore: **chip→host works perfectly, host→chip goes nowhere.**
The most likely remaining cause is the TX pin (GPIO 53) not being muxed to its
UART function, or the PIO transmit path (`HSUART_OPTION_TX_PIO`) needing
something the read path doesn't.

Since `PmBtStack` drives the same UART successfully, the hardware is fine — the
difference has to be in how the port is set up.

**Next step for whoever picks this up:** capture what `PmBtStack` actually does to
the port and diff it against `btbridge`. Two practical ways in:
- Replace `/usr/bin/PmBtStack` with a wrapper script that sets `LD_PRELOAD` to a
  shim hooking `open`/`ioctl`/`write`, logging every call and argument.
- `CONFIG_KPROBES` is off, but `CONFIG_FTRACE=y` — or simply rebuild `hsuart` with
  `printk`s. (It is built-in, `=y`, so that means booting a custom kernel; the
  module route is not available for this driver.)

The specific question to answer: what mux/register state does GPIO 53 (UART TX)
end up in? Everything points at bytes leaving the driver but never reaching the
pin.

## Tools in this repo

| File | Purpose |
|---|---|
| `btbridge/btbridge.c` | pty↔`/dev/bt_uart` bridge; BCSP link establishment; `--loopback`, `--flow hw\|sw\|none`, `--speed`, `--nocrc`, `--letimeout` |
| `btbridge/btctl.c` | minimal HCI control (`up`/`down`/`info`) — no BlueZ userspace on device |
| `btbridge/sniffdec.py` | decoder for Palm's own HCI sniffer capture |

Palm's built-in HCI sniffer is genuinely useful and needs no extra software:

```
luna-send -i palm://com.palm.bluetooth/dbg/palmsniffer '{"start":true}'
# writes /var/log/palmsniffer.log ; decode with sniffdec.py
```

That is how the earlier "DS4 won't connect" mystery was settled — every attempt
was `HCI Connection Complete status=0x04 (Page Timeout)`, i.e. the controller was
simply asleep, not a stack limitation.
