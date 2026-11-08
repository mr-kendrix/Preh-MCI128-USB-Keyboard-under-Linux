#!/usr/bin/env python3
"""pktm --live headless gate: feed synthetic report-04 hex -> expect injected
lines for POSKey 124 ('5') / 95 / 77 + UNKNOWN-scan skipped, AND (when ydotoold
is live) the key events arriving at the uinput node = real end-to-end proof.
Harness only (python here is the OBSERVER, not imported by pktm)."""
import glob
import os
import select
import struct
import subprocess
import sys
import time

BIN = "b_pktm/pktm" if len(sys.argv) < 2 else sys.argv[1]
PMAP = "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/MappingLists/Mr.Kendrix.pmap"
# scancodes per MK.calib.json: 0xe040->124, 0xe013->95, 0x0058->77, 0xee unknown
FEED = ("04 e0 40 00 00  04 fe e0 40 00 00"
        " 04 e0 13 00 00  04 fe e0 13 00 00"
        " 04 58 00 00 00  04 fe 58 00 00 00"
        " 04 ee 00 00 00")

# uinput node: whichever "ydotoold virtual device" the daemon exported
NODE = None
info = open("/proc/bus/input/devices").read()
for blk in info.split("\n\n"):
    if "ydotoold" in blk:
        for line in blk.splitlines():
            if line.startswith("H:"):
                for tok in line.split():
                    if tok.startswith("event"):
                        NODE = "/dev/input/" + tok

fd = os.open(NODE, os.O_RDONLY) if NODE else None
if fd is not None:                       # drain leftovers via select-guarded loop
    end = time.time() + 0.3
    while time.time() < end:
        if select.select([fd], [], [], 0.1)[0]:
            try:
                os.read(fd, 65536)
                break
            except OSError:
                break
        time.sleep(0.05)

p = subprocess.run([BIN, PMAP, "--live", "--feed-hex", FEED, "--calib", "/home/nonrmy/MK.calib.json"],
                   capture_output=True, text=True)
print("rc", p.returncode)
print(p.stdout)
print("STDERR:", p.stderr)

data = b""
if fd is not None:
    end = time.time() + 1.2
    while time.time() < end:
        if select.select([fd], [], [], 0.2)[0]:
            try:
                data += os.read(fd, 65536)
            except OSError:
                pass
        time.sleep(0.05)
    os.close(fd)
ev = [(t, c, v) for _, _, t, c, v in
      (struct.unpack_from("=qqHHi", data, i)
       for i in range(0, len(data) // 24 * 24, 24))] if data else []
print("uinput events:", ev)

ok = True
for pk in ("124", "95", "77"):
    if f"POSKey {pk} " not in p.stdout or "-> rule match -> injected" not in p.stdout:
        ok = False
if "UNKNOWN scancode 0x00ee -> skipped" not in p.stdout:
    ok = False
for want in (6, 111, 15):               # '5' del tab — keydown+keyup each
    downs = sum(1 for t, c, v in ev if t == 1 and c == want and v == 1)
    ups = sum(1 for t, c, v in ev if t == 1 and c == want and v == 0)
    if fd is not None and not (downs == 1 and ups == 1):
        ok = False
print("GATE:", "GREEN" if ok else "RED")
