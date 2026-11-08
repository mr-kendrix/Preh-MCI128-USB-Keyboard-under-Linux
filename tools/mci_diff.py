#!/usr/bin/env python3
"""mci_diff.py — dump exact byte diffs (with context) for the MCI-family
template fixtures pos_mci96 and pos_mci128: oracle vs pktxc."""
import subprocess, os, sys

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
TPL = "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"

def run_oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def run_native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([PKTXC, p, out], capture_output=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def show(name, b, off, width=16):
    lo = max(0, off - width); hi = min(len(b), off + width + 1)
    print(f"  ctx[{lo:04x}..{hi-1:04x}]: " + " ".join(f"{x:02x}" for x in b[lo:hi]))

for tag in ("pos_mci96.MWF", "pos_mci128.MWF"):
    p = f"{TPL}/{tag}"
    o = run_oracle(p, f"/tmp/md_o_{tag}.mwx")
    n = run_native(p, f"/tmp/md_n_{tag}.mwx")
    if o is None or n is None:
        print(f"{tag}: ASYMM oracle={'ok' if o else 'rej'} pktxc={'ok' if n else 'rej'}"); continue
    a = o[4:] if o[:4] == b"Preh" else o
    c = n[4:] if n[:4] == b"Preh" else n
    d = [i for i in range(min(len(a), len(c))) if a[i] != c[i]]
    print(f"{tag}: len o={len(a)} n={len(c)} diffs={len(d)}")
    for i in d:
        print(f"  off {i:04x}: oracle={a[i]:02x} pktxc={c[i]:02x}")
        show("oracle", a, i); show("pktxc", c, i)
    # dump the record stream of the ORACLE around the first diff for structure
    if d:
        s = min(d)
        base = s - (s % 8)
        print(f"  oracle records aligned@{base:04x}: " + " ".join(f"{x:02x}" for x in a[base:base+64]))
