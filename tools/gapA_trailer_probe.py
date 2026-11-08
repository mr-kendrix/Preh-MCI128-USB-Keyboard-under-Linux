#!/usr/bin/env python3
"""gapA_trailer_probe.py — hunt the pos-record trailer semantics.

Swap-family variants around POSKey001/002 written BOTH in the ref stream
(A01-..."{POSKeyNNN}") and in the !@KEYPRINT declarations, so cap<->POS
coupling becomes inconsistent in different ways. For each variant print the
oracle record region vs pktxc and flag trailer-byte diffs.
"""
import os, re, subprocess
REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
D = "/home/nonrmy/hermes-workspace/oracle_dir"
BASE = open(f"{D}/01.mwf", "rb").read().split(b"\r\n")

REF_RE = re.compile(rb'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS\w+)\}"')
KP_RE = re.compile(rb'^!@KEYPRINT:(\S+) "([^"]*)"')
REF = [i for i, l in enumerate(BASE) if REF_RE.match(l)]
POSREF = [i for i in REF if REF_RE.match(BASE[i]).group(4) != b"POSKeylock" and BASE[i].startswith(b"A0")]
KP = [i for i, l in enumerate(BASE) if KP_RE.match(l)]
print(f"ref={len(REF)} kp={len(KP)}")
for i in KP[:3] + KP[-3:]:
    print("  KP:", BASE[i].decode())
for i in REF[:2]:
    print("  REF:", BASE[i].decode())

def orc(p, out):
    subprocess.run(["bash", f"{REPO}/oracle/run_oracle.sh", D, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([f"{REPO}/b/pktxc", p, out], capture_output=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def mk(tag, fn):
    v = BASE[:]
    fn(v)
    p = f"/tmp/gat_{tag}.mwf"
    open(p, "wb").write(b"\r\n".join(v))
    o = orc(p, f"/tmp/gat_{tag}_o.mwx")
    n = native(p, f"/tmp/gat_{tag}_n.mwx")
    if o is None or n is None:
        print(f"{tag}: ASYMM oracle={'ok' if o else 'rej'} pktxc={'ok' if n else 'rej'}"); return
    a = o[4:] if o[:4] == b"Preh" else o
    b = n[4:] if n[:4] == b"Preh" else n
    d = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    print(f"{tag}: {'MATCH' if not d and len(a)==len(b) else 'DIFF'} len {len(a)}/{len(b)}")
    for i in d[:12]:
        print(f"   {i:04x}: or {a[i]:02x} pk {b[i]:02x}  (rec idx {(i-0x13c)//9 if i>=0x13c else -1}, slot {(i-0x13c)%9})")
    lo = 0x138
    print(f"   OR[{lo:04x}..]: " + " ".join(f"{x:02x}" for x in a[lo:lo+30]))
    print(f"   PK[{lo:04x}..]: " + " ".join(f"{x:02x}" for x in b[lo:lo+30]))

def kp_swap(i, j):
    def f(v):
        v[i], v[j] = BASE[j], BASE[i]
    return f

def ref_swap(i, j):
    def f(v):
        v[i], v[j] = BASE[j], BASE[i]
    return f

def both_swap(ki, kj, ri, rj):
    def f(v):
        v[ki], v[kj] = BASE[kj], BASE[ki]
        v[ri], v[rj] = BASE[rj], BASE[ri]
    return f

def kp_label_only(i, j):
    # swap only the cap label inside the KEYPRINT line (keeps the POS\lineNNN)
    def f(v):
        la = KP_RE.match(BASE[i]).group(1); lb = KP_RE.match(BASE[j]).group(1)
        v[i] = BASE[i].replace(la, lb, 1); v[j] = BASE[j].replace(lb, la, 1)
    return f

def kp_posline(i, j):
    # swap only the "POS\lineNNN" token inside the KEYPRINT lines
    def f(v):
        pa = KP_RE.match(BASE[i]).group(2); pb = KP_RE.match(BASE[j]).group(2)
        v[i] = BASE[i].replace(pa, pb, 1); v[j] = BASE[j].replace(pb, pa, 1)
    return f

if __name__ == "__main__":
    ki = KP[:2]; ri = [i for i in REF if REF_RE.match(BASE[i]).group(1) in (b"A01", b"A02")]
    print("kp idx", ki, "ref idx", ri)
    mk("kp_swap_first2", kp_swap(ki[0], ki[1]))
    mk("kp_label_only", kp_label_only(ki[0], ki[1]))
    mk("kp_posline", kp_posline(ki[0], ki[1]))
    mk("both_swap", both_swap(ki[0], ki[1], ri[0], ri[1]))
    mk("kp_ref_swap", lambda v: (v.__setitem__(ki[0], BASE[ki[1]]), v.__setitem__(ki[1], BASE[ki[0]]),
                                 v.__setitem__(ri[0], BASE[ri[1]]), v.__setitem__(ri[1], BASE[ri[0]])))
