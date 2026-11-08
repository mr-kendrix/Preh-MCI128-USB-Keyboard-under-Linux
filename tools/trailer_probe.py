#!/usr/bin/env python3
"""trailer_probe.py — model the POS-record trailer byte.

Base 01.mwf: swap the POSKey001/POSKey002 assignments on the pos-ref lines,
compile base + swapped with BOTH the vendor oracle and pktxc, and dump the
record region so the trailer semantics can be compared against the .mwf
state (KEYPRINT lines, pos-ref lines, cap->POS coupling).
"""
import os, re, subprocess, sys

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"

def oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def native(p, out):
    subprocess.run([PKTXC, p, out], capture_output=True, text=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def run_pktxc_dir(p, out):
    """pktxc from a scratch dir variant (path absolute; cwd must stay REPO)."""
    return native(p, out)

base_p = f"{ORACLE_DIR}/01.mwf"
base = open(base_p, "rb").read().decode("cp1252")

# ---- variant A: swap the two pos-refs between cap labels A01/A02 ----
lines = base.splitlines(keepends=True)
i01 = next(i for i, l in enumerate(lines) if l.startswith('A01-L+') )
i02 = next(i for i, l in enumerate(lines) if l.startswith('A02-L+') )
va = lines[:]
va[i01] = lines[i01].replace("POSKey001", "POSKey002")
va[i02] = lines[i02].replace("POSKey002", "POSKey001")
pa = "/tmp/tp_swap_ref.mwf"
open(pa, "wb").write("".join(va).encode("cp1252"))

# ---- variant B: swap the two KEYPRINT label fields ----
vb = lines[:]
k01 = next(i for i, l in enumerate(lines) if l.startswith('!@KEYPRINT:A01'))
k02 = next(i for i, l in enumerate(lines) if l.startswith('!@KEYPRINT:A02'))
p1 = re.search(r'"([^"]*)"', lines[k01]); p2 = re.search(r'"([^"]*)"', lines[k02])
vb[k01] = lines[k01].replace(p1.group(1), p2.group(1))
vb[k02] = lines[k02].replace(p2.group(1), p1.group(1))
pb = "/tmp/tp_swap_print.mwf"
open(pb, "wb").write("".join(vb).encode("cp1252"))

def show(tag, data):
    if data is None:
        print(f"  {tag}: <rejected>"); return
    b = data[4:] if data[:4] == b"Preh" else data
    print(f"  {tag}: len={len(b)}  region 0x130-0x170:")
    lo, hi = 0x130, min(0x178, len(b))
    row = " ".join(f"{b[i]:02x}" for i in range(lo, hi))
    print("   " + row)

for tag, p in (("base", base_p), ("swap_ref", pa), ("swap_print", pb)):
    ob = oracle(p, f"/tmp/tp_o_{tag}.mwx")
    nb = native(p, f"/tmp/tp_n_{tag}.mwx")
    print(f"== {tag}")
    show("oracle", ob)
    show("pktxc", nb)
    if ob and nb:
        a = ob[4:] if ob[:4] == b"Preh" else ob
        c = nb[4:] if nb[:4] == b"Preh" else nb
        d = [i for i in range(min(len(a), len(c))) if a[i] != c[i]]
        print(f"  diffs={len(d)} len_o={len(a)} len_n={len(c)}" + (f" first={[hex(x) for x in d[:12]]}" if d else ""))
