#!/usr/bin/env python3
"""Ground truth: decode wire payloads from the 4 USBPcap captures; compare vs pktxc(.mwf)."""
import subprocess, sys
sys.path.insert(0, '/home/nonrmy/hermes-workspace')
exec(open('/home/nonrmy/hermes-workspace/pkt-editor-cpp/tools/pcap_codes.py').read().split('if __name__')[0])
import pkt_protocol as P

D = "/run/media/nonrmy/MULTIBOOT/Wireshark"
PKTXC = "/home/nonrmy/hermes-workspace/pkt-editor-cpp/b/pktxc"

def true_payload(fn):
    c = codes(fn)
    j = 0
    while j < len(c) and not (c[j] == 0xEF and c[j + 1] == 0x00): j += 1
    j += 2
    payload = b''; nb = 0
    while j + 18 <= len(c) and c[j] == 0xEF and c[j + 1] not in (0x00, 0x12, 0x0E):
        blk = c[j:j+18]
        addr, cid, data, ok = P.decode_block(blk)
        payload += data; nb += 1; j += 18
    return payload

for n in ("01", "02", "03", "04"):
    t = true_payload(f"{D}/{n}.pcapng")
    out = f"/tmp/pktxc_{n}.mwx"
    rc = subprocess.run([PKTXC, f"{D}/{n}.mwf", out], check=False, capture_output=True)
    if rc.returncode > 3: raise SystemExit(f"pktxc failed {rc.returncode}")
    comp = open(out, 'rb').read()
    if comp[:4] == b'Preh': comp = comp[4:]
    while len(comp) % 8: comp += b'\0'
    diffs = [i for i in range(min(len(t), len(comp))) if t[i] != comp[i]]
    print(f"{n}: true={len(t)} pktxc={len(comp)} diffs={len(diffs)}")
    for i in diffs[:12]:
        print(f"  @{i:04x}: true={t[i]:02x} pktxc={comp[i]:02x}")
    open(f'/tmp/true_payload_{n}.bin', 'wb').write(t)
