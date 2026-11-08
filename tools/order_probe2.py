#!/usr/bin/env python3
"""order_probe2.py — is the record order driven by the !@KEYPRINT line order,
the !@KEYATTRIB line order, or a canonical cap order?  Permute each section
and decode the oracle record stream.
"""
import re
import subprocess
import os

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
BASE = open(f"{ORACLE_DIR}/01.mwf", "rb").read().split(b"\r\n")

def cap_of(line):
    m = re.match(rb"^([A-Z0-9]{2,4})(-L)?\+", line)
    return m.group(1) if m else None

KP = [i for i, l in enumerate(BASE) if l.startswith(b"!@KEYPRINT:")]
KA = [i for i, l in enumerate(BASE) if l.startswith(b"!@KEYATTRIB:")]
REF = [i for i, l in enumerate(BASE) if cap_of(l)]
print(f"kp={len(KP)} ka={len(KA)} ref={len(REF)}")
print("caps kp:", b" ".join(cap_of(BASE[i]) for i in KP)[:200])
print("caps ka:", b" ".join(cap_of(BASE[i]) for i in KA)[:200])
print("caps ref:", b" ".join(cap_of(BASE[i]) for i in REF)[:200])

def oracle(p, out):
    subprocess.run(["bash", f"{REPO}/oracle/run_oracle.sh", ORACLE_DIR, p, out],
                   capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def decode(tag, path):
    d = oracle(path, f"/tmp/o4_{tag}.mwx")
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
            print("DESYNC", i, hex(s[i])); return
        recs.append(s[i:i + L].hex()); i += L
    print(f"{tag}: first8={[r[:8] for r in recs[:8]]}")

def mk(name, fn):
    v = BASE[:]
    fn(v)
    p = f"/tmp/o4_{name}.mwf"
    open(p, "wb").write(b"\r\n".join(v))
    decode(name, p)

def kp_swap(v):   # swap KEYPRINT lines A01 <-> A16 (caps keep their labels)
    a = next(i for i in KP if BASE[i].startswith(b"!@KEYPRINT:A01"))
    b2 = next(i for i in KP if BASE[i].startswith(b"!@KEYPRINT:A16"))
    v[a], v[b2] = BASE[b2], BASE[a]

def ka_swap(v):
    a = next(i for i in KA if BASE[i].startswith(b"!@KEYATTRIB:A01"))
    b2 = next(i for i in KA if BASE[i].startswith(b"!@KEYATTRIB:A16"))
    v[a], v[b2] = BASE[b2], BASE[a]

def ref_capswap(v):  # swap pos-ref LINES A01 <-> A16 (labels follow lines)
    a = next(i for i in REF if cap_of(BASE[i]) == b"A01")
    b2 = next(i for i in REF if cap_of(BASE[i]) == b"A16")
    v[a], v[b2] = BASE[b2], BASE[a]

if __name__ == "__main__":
    decode("base", f"{ORACLE_DIR}/01.mwf")
    mk("kp_swap", kp_swap)
    mk("ka_swap", ka_swap)
    mk("ref_capswap", ref_capswap)
