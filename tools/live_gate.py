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
for want in (6, 1, 15):                 # '5'=6, POSKey 95 vendor value {Esc}=1, Tab=15
    downs = sum(1 for t, c, v in ev if t == 1 and c == want and v == 1)
    ups = sum(1 for t, c, v in ev if t == 1 and c == want and v == 0)
    if fd is not None and not (downs == 1 and ups == 1):
        ok = False
# --- stage 2: REAL read path (FIFO feeds the ppoll loop; --feed-hex BYPASSES it and
#     hid the round-9 POLLIN bug). Runs regardless of pktd.service via TEST HOOK
#     PKT_SKIP_PKTD_SCAN (gate-only; pktm prints a warning about service env then).
not_tested = False
os.system("rm -f /tmp/gate_fifo")
subprocess.run(["mkfifo", "/tmp/gate_fifo"])
with open("/tmp/gate_ar.pmap", "w") as f:
    f.write("<NAME></NAME>\n<MAP>\n  <POSKey>313234</POSKey>\n"
            "  <LAYER><Type>74657874</Type><Value>78</Value>"
            "<Dependency></Dependency><AutoRepeat>74727565</AutoRepeat></LAYER>\n</MAP>\n")
writer = subprocess.Popen([sys.executable, "-c",
    "import time\nf=open('/tmp/gate_fifo','wb',buffering=0)\n"
    "f.write(b'\\x04\\xe0\\x40\\x00\\x00'); f.flush(); time.sleep(4)"])
env2 = dict(os.environ, PKT_MAX_HOLD_S="1", PKT_SKIP_PKTD_SCAN="1")
r2 = subprocess.run([BIN, "/tmp/gate_ar.pmap", "--live", "--device", "/tmp/gate_fifo",
                     "--calib", "/home/nonrmy/MK.calib.json", "--seconds", "3"],
                    capture_output=True, text=True, env=env2, timeout=15)
try:
    writer.wait(8)
except subprocess.TimeoutExpired:
    writer.kill()
    writer.wait()
print("REAL-PATH:", r2.stdout.strip())
if "refusing" in r2.stdout:                 # pktd calibrate/watch/raw holds the flock
    print("REAL-PATH: SKIP (pktd calibrate/watch/raw holds the flock — read path NOT tested)")
    not_tested = True
elif not ("AutoRepeat press" in r2.stdout and "max-hold" in r2.stdout):
    ok = False
    print("REAL-PATH: missing AutoRepeat press / max-hold release -> RED")
print("GATE:", ("GREEN" if ok else "RED") + (" (read path NOT tested)" if not_tested else ""))
