#!/usr/bin/env python3
"""gapA_named_swap.py — dump POS-record content for the failing named-token
swaps on 01.mwf: POSMSR2<->POSMSR3 and POSKey001<->POSMSR3. Print, for the
affected slots plus their neighbours, the record slice and the index entry,
oracle vs pktxc, so the slot<->content binding rule can be pinned.
"""
import subprocess, os, re

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
D = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"
SRC = f"{D}/01.mwf"
REF_RE = re.compile(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS[A-Za-z0-9]*)\}"')

def strip(b):
    return b[4:] if b and b[:4] == b"Preh" else b

def occ(b):
    return [(k + 1, (b[4 + 2 * k] << 8) | b[5 + 2 * k]) for k in range(142)
            if (b[4 + 2 * k] << 8) | b[5 + 2 * k]]

def slices(b, want):
    o = occ(b)
    nxt = {}
    for i, (p, off) in enumerate(o):
        end = o[i + 1][1] if i + 1 < len(o) else len(b)
        nxt[p] = (off, end)
    return {p: (nxt.get(p, (None, None))[0], (b[nxt[p][0]:nxt[p][1]] if p in nxt else None)) for p in want}

def show(tag, b, want):
    if b is None:
        print(f"  {tag}: rej"); return
    s = slices(b, want)
    print(f"  {tag}: len={len(b)}")
    for p in want:
        off, rec = s[p]
        print(f"    POS{p}: off={off if off is None else format(off,'04x')} rec={rec.hex() if rec else None}")

lines0 = open(SRC, "rb").read().decode("cp1252").splitlines()
ref = [i for i, l in enumerate(lines0) if REF_RE.match(l)]
toks = {REF_RE.match(lines0[i]).group(4): i for i in ref}

def run(tag, a, b):
    ia, ib = toks[a], toks[b]
    v = lines0[:]
    v[ia] = lines0[ia].replace(a, b, 1)
    v[ib] = lines0[ib].replace(b, a, 1)
    print(f"  lines: {v[ia]!r} | {v[ib]!r}")
    print(f"  caps:  {REF_RE.match(lines0[ia]).group(1)} <-> {REF_RE.match(lines0[ib]).group(1)}")
    p = f"/tmp/ns_{tag}.mwf"
    open(p, "wb").write(("\r\n".join(v) + "\r\n").encode("cp1252"))
    ob = strip(oracle(p, f"/tmp/ns_o_{tag}.mwx"))
    nb = strip(native(p, f"/tmp/ns_n_{tag}.mwx"))
    want = [1, 2, 3, 129, 130, 131, 132, 133, 134, 135, 136, 137, 140, 141, 142]
    show("or", ob, want); show("pk", nb, want)
    if ob and nb:
        d = [i for i in range(min(len(ob), len(nb))) if ob[i] != nb[i]]
        print(f"  diffs {len(d)} {[hex(x) for x in d[:14]]}")

def native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([PKTXC, p, out], capture_output=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, D, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

if __name__ == "__main__":
    base = strip(oracle(SRC, "/tmp/ns_base.mwx"))
    show("or_base", base, [1, 2, 3, 129, 130, 131, 131, 133, 134, 141, 142])
    run("msr23", "POSMSR2", "POSMSR3")
    run("k001_msr3", "POSKey001", "POSMSR3")
    run("k001_msr1", "POSKey001", "POSMSR1")
    run("msr1_bar", "POSMSR1", "POSBarcode")
