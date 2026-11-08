#!/usr/bin/env python3
"""shuf_probe.py — shuffle the pos-ref LINES (labels+tokens travel together).
If the oracle keeps record order == line order, the rule is 'file order',
not 'canonical label sort'.  Also swaps a 9-byte POS (e0xx) against an 8-byte
one to see whether downstream INDEX offsets move with the stream."""
import random, re, subprocess, os
REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
D = "/home/nonrmy/hermes-workspace/oracle_dir"
pat = re.compile(rb'^[A-Z0-9]{2,4}(?:-L)?\+.*"\{POS\w+\}"')

def orc(p, out):
    r = subprocess.run(["bash", f"{REPO}/oracle/run_oracle.sh", D, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def dec(tag, path):
    d = orc(path, f"{D}/_{tag}.mwx")
    if d is None:
        print(tag, "REJECT"); return None
    b = d[4:] if d[:4] == b"Preh" else d
    idx = b[4:4+284]
    s = b[4+284+34:]
    recs = []
    i = 0
    while i < len(s):
        if s[i] == 0x1D: L = 9 if s[i+2] >= 0x80 else 8
        elif s[i] == 0x1E: L = 7
        else: print("DESYNC", i, hex(s[i])); return None
        recs.append(s[i:i+L]); i += L
    codes = [r[2:2+1 if (r[2] if len(r)>2 else 0) < 0x80 else 2].hex() for r in recs]
    print(f"{tag}: n={len(recs)} codes[:10]={[c.hex() for c in [bytes([r[2]]) if r[2]<0x80 else bytes(r[2:4]) for r in recs[:10]]]}")
    return b, recs

lines = open(f"{D}/01.mwf", "rb").read().split(b"\r\n")
ref = [i for i, l in enumerate(lines) if pat.match(l)]
print("ref lines:", len(ref))

def shuf(v):
    block = [lines[i] for i in ref]
    rng = random.Random(5); rng.shuffle(block)
    for j, i in enumerate(ref): v[i] = block[j]

def mixswap(v):  # POSKey017 (8B code 0x10) <-> POSKey078 (9B code e003)
    for i in ref:
        if lines[i].find(b'"{POSKey017}"') >= 0: a = i
        if lines[i].find(b'"{POSKey078}"') >= 0: c = i
    x = lines[a].replace(b'"{POSKey017}"', b'"{POSKey078}"')
    y = lines[c].replace(b'"{POSKey078}"', b'"{POSKey017}"')
    v[a] = x; v[c] = y

for name, fn in (("shuf", shuf), ("mixswap", mixswap)):
    v = lines[:]
    fn(v)
    p = f"{D}/_x_{name}.mwf"
    open(p, "wb").write(b"\r\n".join(v))
    dec(name, p)

# index-table check on mixswap: POS17 offset vs POS78 offset ordering
b, recs = dec("mixswap", f"{D}/_x_mixswap.mwf")
if b:
    idx = b[4:4+284]
    def off(p):
        return int.from_bytes(idx[2*(p-1):2*p], "big")
    print("mixswap idx:", {p: hex(off(p)) for p in (16,17,18,77,78,79)})
    first = next(j for j, r in enumerate(recs) if r[:4] == b'\x1d\x68\x10')
    second = next(j for j, r in enumerate(recs) if r[:4] == b'\x1d\x68\xe0\x03')
    print("stream pos of code10rec:", first, "of e003rec:", second)
