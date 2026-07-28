#!/usr/bin/env python3
"""Decode PalmSniffer captures (/var/log/palmsniffer.log) from webOS PmBtEngine.

Record framing, recovered by inspection:
    0x0a <seq:u32le> 0x0a 0x02 <2 bytes> <len:u16le> <unixtime:u32le>
    0x3a <len:u32le> 0x3a <chan> 0x3a <dir> 0x3a <payload:len>
where payload is a raw HCI packet (command or event), dir 0 = host->controller.
"""
import sys, struct, datetime

HCI_EV = {0x03: "Connection Complete", 0x04: "Connection Request",
          0x05: "Disconnection Complete", 0x06: "Auth Complete",
          0x07: "Remote Name Req Complete", 0x08: "Encryption Change",
          0x0e: "Command Complete", 0x0f: "Command Status",
          0x13: "Number Of Completed Packets", 0x16: "PIN Code Request",
          0x17: "Link Key Request", 0x18: "Link Key Notification",
          0x2f: "Extended Inquiry Result", 0x31: "IO Capability Request",
          0x32: "IO Capability Response", 0x33: "User Confirmation Request",
          0x36: "Simple Pairing Complete", 0x0d: "QoS Setup Complete",
          0x1b: "Max Slots Change", 0x20: "Page Scan Rep Mode Change",
          0x22: "Sync Conn Complete", 0x2d: "Link Supervision Timeout Changed"}

HCI_CMD = {0x0401: "Inquiry", 0x0405: "Create Connection", 0x0406: "Disconnect",
           0x0409: "Accept Connection Request", 0x040b: "Link Key Request Reply",
           0x040c: "Link Key Request Negative Reply", 0x0419: "Remote Name Request",
           0x041b: "Read Remote Supported Features", 0x042b: "IO Capability Request Reply",
           0x042c: "User Confirmation Request Reply", 0x0c03: "Reset",
           0x0c05: "Set Event Filter", 0x0c13: "Change Local Name",
           0x0c1a: "Write Scan Enable", 0x0c35: "Write Authentication Enable",
           0x0c56: "Write Simple Pairing Mode", 0x0805: "Write Link Policy Settings",
           0x2013: "LE Create Connection"}

# Bluetooth Core spec error codes (the ones that matter here)
ERRS = {0x00: "Success", 0x02: "Unknown Connection Identifier",
        0x04: "PAGE TIMEOUT (device not responding/asleep)",
        0x05: "Authentication Failure",
        0x06: "PIN or Key Missing (link key rejected -> NOT PAIRED)",
        0x07: "Memory Capacity Exceeded", 0x08: "Connection Timeout",
        0x0b: "ACL Connection Already Exists", 0x0c: "Command Disallowed",
        0x0d: "Conn Rejected: Limited Resources", 0x0e: "Conn Rejected: Security",
        0x0f: "Conn Rejected: Unacceptable BD_ADDR", 0x10: "Conn Accept Timeout",
        0x13: "Remote User Terminated", 0x16: "Conn Terminated By Local Host",
        0x22: "LMP Response Timeout", 0x28: "Instant Passed"}


def bdaddr(b):
    return ":".join("%02x" % x for x in reversed(b))


def decode(data):
    i, out = 0, []
    while i < len(data):
        j = data.find(b"\x3a", i)
        if j < 0 or j + 5 > len(data):
            break
        ln = struct.unpack_from("<I", data, j + 1)[0]
        if ln == 0 or ln > 4096 or j + 5 >= len(data) or data[j + 5] != 0x3a:
            i = j + 1
            continue
        chan = data[j + 6]
        if j + 7 >= len(data) or data[j + 7] != 0x3a:
            i = j + 1
            continue
        direction = data[j + 8]
        p = j + 10                       # skip 0x3a before payload
        pkt = data[p:p + ln]
        i = p + ln
        if len(pkt) < 2:
            continue
        out.append((chan, direction, pkt))
    return out


def describe(chan, direction, p):
    d = "HOST->CTRL" if direction == 0 else "CTRL->HOST"
    # events arrive from the controller
    if direction != 0:
        ev, plen = p[0], p[1]
        name = HCI_EV.get(ev, "Event 0x%02x" % ev)
        s = f"{d}  {name}"
        if ev == 0x03 and len(p) >= 11:                    # Connection Complete
            st = p[2]
            s += f"  status=0x{st:02x} [{ERRS.get(st,'?')}]  addr={bdaddr(p[5:11])}"
        elif ev == 0x04 and len(p) >= 11:                  # Connection Request
            s += f"  from={bdaddr(p[2:8])} class={p[8:11].hex()}"
        elif ev == 0x05 and len(p) >= 6:                   # Disconnection Complete
            s += f"  status=0x{p[2]:02x} handle={p[3]|p[4]<<8} reason=0x{p[5]:02x} [{ERRS.get(p[5],'?')}]"
        elif ev == 0x0e and len(p) >= 7:                 # Command Complete
            op = p[4] | (p[5] << 8)
            st = p[6]
            s += f"  cmd={HCI_CMD.get(op, hex(op))} status=0x{st:02x} [{ERRS.get(st,'?')}]"
        elif ev == 0x0f and len(p) >= 7:                 # Command Status
            op = p[5] | (p[6] << 8)
            s += f"  cmd={HCI_CMD.get(op, hex(op))} status=0x{p[2]:02x} [{ERRS.get(p[2],'?')}]"
        elif ev == 0x17 and len(p) >= 8:
            s += f"  addr={bdaddr(p[2:8])}"
        return s
    op = p[0] | (p[1] << 8)
    s = f"{d}  {HCI_CMD.get(op, 'Cmd 0x%04x' % op)}"
    if op == 0x0405 and len(p) >= 9:
        s += f"  to={bdaddr(p[3:9])}"
    elif op in (0x040b, 0x040c, 0x0419, 0x042b) and len(p) >= 9:
        s += f"  addr={bdaddr(p[3:9])}"
    return s


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    recs = decode(data)
    print(f"{len(recs)} HCI packets\n")
    for chan, direction, p in recs:
        print(describe(chan, direction, p))
