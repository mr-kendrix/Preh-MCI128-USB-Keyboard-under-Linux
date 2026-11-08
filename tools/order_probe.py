#!/usr/bin/env python3
"""order_probe.py — pin the record-stream order rule.

Emits .mwf variants that change the (cap-label, POSKey-token) pairing / line
order of the pos-ref block and decodes the oracle record stream per variant:
  far_swap      POSKey001 <-> POSKey128 tokens on first/last ref lines
  label_swap    ref-line LABELS A01 <-> A02 (tokens stay)
  move_first_last  ref lines 1 <-> 128 moved (pairs unchanged)
  shuf          rotate whole ref lines (pairs unchanged)
"""
import os
import re
import subprocess

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"

BASE = open(f"{ORACLE_DIR}/01.mwf", "rb").read().split(b"\r\n")

def tok(line):
    """the {POSxxx} token of a ref line (decoded), or None."""
    m = re.search(rb'"\{(POS\w+)\}"', line)
    return m.group(1).decode() if m else None

REF = [i for i, l in enumerate(BASE) if tok(l)]
POSRI = [i for i in REF if tok(BASE[i]).startswith("POSKey") and not tok(BASE[i]).startswith("POSKeylock")]
NAMED = [i for i in REF if i not in POSRI]

def oracle(p, out):
    subprocess.run(["bash", f"{REPO}/oracle/run_oracle.sh", ORACLE_DIR, p, out],
                   capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def decode(tag, path):
    d = oracle(path, f"/tmp/o3_{tag}.mwx")
    if d is None:
        print(tag, "REJECT")
        return
    b = d[4:] if d[:4] == b"Preh" else d
    s = b[4 + 284 + 34:]
    recs = []
    i = 0
    while i < len(s):
        if s[i] == 0x1D:
            L = 9 if s[i + 2] >= 0x80 else 8
        elif s[i] == 0x1E:
            L = 7
        else:
            print("DESYNC at", i, hex(s[i]))
            return
        recs.append(s[i:i + L].hex())
        i += L
    print(f"{tag}: nrec={len(recs)} first8={[r[:10] for r in recs[:8]]} "
          f"pos002_at={next((j for j, r in enumerate(recs) if r.startswith('1d6802')), None)} "
          f"pos001_at={next((j for j, r in enumerate(recs) if r.startswith('1d6801')), None)}")

def mk(name, fn):
    v = BASE[:]
    fn(v)
    p = f"/tmp/o3_{name}.mwf"
    open(p, "wb").write(b"\r\n".join(v))
    decode(name, p)

def far(v):
    a, b2 = POSRI[0], POSRI[-1]
    x = BASE[a].replace(b"POSKey001", b"POSKey128")
    y = BASE[b2].replace(b"POSKey128", b"POSKey001")
    v[a] = x; v[b2] = y

def label_swap(v):
    a, b2 = POSRI[0], POSRI[1]
    v[a] = BASE[a].replace(b"A02-", b"A01-", 1) if BASE[a][:4] != b"A01-" else BASE[a]
    v[a], v[b2] = BASE[a].replace(b"A01-", b"A02-", 1), BASE[b2].replace(b"A02-", b"A01-", 1)

def move_first_last(v):
    v[POSRI[0]], v[POSRI[-1]] = BASE[POSRI[-1]], BASE[POSRI[0]]

def shuf(v):
    block = [BASE[i] for i in POSRI]
    for j, i in enumerate(POSRI):
        v[i] = block[(j + 7) % len(block)]

if __name__ == "__main__":
    decode("base", f"{ORACLE_DIR}/01.mwf")
    mk("far_swap", far)
    mk("label_swap", label_swap)
    mk("move_first_last", move_first_last)
    mk("shuf", shuf)
