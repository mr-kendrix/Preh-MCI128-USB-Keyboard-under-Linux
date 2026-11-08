#!/usr/bin/env python3
"""gapA_swap_sweep.py — brute sweep: swap POSKey token assignments on
01.mwf (permutation pairs, incl. named tokens) and report EVERY oracle-vs-
pktxc diff, with offsets. Finds which "swapped assignment" case is broken.
"""
import subprocess, os, re, itertools

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
D = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"
SRC = f"{D}/01.mwf"
REF_RE = re.compile(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS[A-Za-z0-9]*)\}"')

def oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, D, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([PKTXC, p, out], capture_output=True, text=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

lines0 = open(SRC, "rb").read().decode("cp1252").splitlines()
ref = [i for i, l in enumerate(lines0) if REF_RE.match(l)]
toks = [REF_RE.match(lines0[i]).group(4) for i in ref]
print(f"base len oracle: {len(oracle(SRC, '/tmp/base_o.mwx') or b'')}")

def diff(a, b):
    return [i for i in range(min(len(a), len(b))) if a[i] != b[i]]

def strip(b):
    return b[4:] if b and b[:4] == b"Preh" else b

pairs = [(toks[0], toks[1]), (toks[0], toks[2]), (toks[0], toks[-1]), (toks[1], toks[2]),
         (toks[20], toks[21]), (toks[60], toks[61]), (toks[76], toks[77]),
         (toks[-1], toks[-2]), (toks[-3], toks[-2]), (toks[128], toks[129]),
         (toks[130], toks[131], ), ]
for pr in pairs:
    a, b = pr[0], pr[1]
    ia = ref[toks.index(a)]; ib = ref[toks.index(b)]
    v = lines0[:]
    v[ia] = lines0[ia].replace(a, b, 1)
    v[ib] = lines0[ib].replace(b, a, 1)
    p = f"/tmp/sw_{a}_{b}.mwf"
    open(p, "wb").write(("\r\n".join(v) + "\r\n").encode("cp1252"))
    ob = strip(oracle(p, f"/tmp/sw_o_{a}_{b}.mwx"))
    nb = strip(native(p, f"/tmp/sw_n_{a}_{b}.mwx"))
    if ob is None or nb is None:
        print(f"{a}<->{b}: oracle={'ok' if ob else 'rej'} pktxc={'ok' if nb else 'rej'}")
        continue
    d = diff(ob, nb)
    print(f"{a}<->{b}: diffs={len(d)} {[hex(x) for x in d[:12]]}" +
          ("" if not d else " | or " + " ".join(f'{ob[i]:02x}' for i in d[:6]) + " | pk " + " ".join(f'{nb[i]:02x}' for i in d[:6])))
# also: swap cap labels between a POSKey ref and a NAMED ref (A01 -> POSMSR1 etc.)
named = [t for t in toks if not t.startswith("POSKey")]
for t in named[:3]:
    ia = ref[toks.index(toks[0])]; ib = ref[toks.index(t)]
    v = lines0[:]
    v[ia] = lines0[ia].replace(toks[0], t, 1); v[ib] = lines0[ib].replace(t, toks[0], 1)
    p = f"/tmp/sx_{toks[0]}_{t}.mwf"
    open(p, "wb").write(("\r\n".join(v) + "\r\n").encode("cp1252"))
    ob = strip(oracle(p, f"/tmp/sx_o_{t}.mwx")); nb = strip(native(p, f"/tmp/sx_n_{t}.mwx"))
    print(f"poskey001<->{t}: oracle={'ok' if ob else 'rej'} pktxc={'ok' if nb else 'rej'}" +
          (f" diffs={len(diff(ob,nb))} {[hex(x) for x in diff(ob,nb)[:12]]}" if ob and nb else ""))
