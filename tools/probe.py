#!/usr/bin/env python3
"""Feature->byte delta probe from oracle outputs."""
import hashlib, json, os, subprocess, sys
from_oracle = "/home/nonrmy/hermes-workspace/oracle_files/run_oracle.sh"
DLL_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"

def oracle(mwf, out):
    r = subprocess.run(["bash", from_oracle, DLL_DIR, mwf, out],
                       capture_output=True, text=True, timeout=60)
    if os.path.exists(out) and os.path.getsize(out) > 0:
        d = open(out, "rb").read()
        return d[4:] if d[:4] == b"Preh" else d
    return None

if __name__ == "__main__":
    base = sys.argv[1]                       # base .mwf
    b0 = oracle(base, "/tmp/probe_base.mwx")
    print("base len:", len(b0) if b0 else None)
    text = open(base, encoding="cp1252", errors="replace").read()
    lines = text.splitlines()
    # candidate single-token header mutations on their own line groups
    edits = []
    for i, l in enumerate(lines):
        k = l.split(" ", 1)[0].strip()
        if k == "LANGUAGE":
            for v in ("US", "US S", "GR", "FR189", "SP", "UK", "SG", "SF", "IT"):
                edits.append((i, f"LANGUAGE {v}"))
        elif k == "MODE":
            for tok in ("+T1", "-T1", "+T2", "-T2", "+KL", "-KL", "+BA0", "+BA2",
                        "+EF", "-EF", "+IEK", "-IEK", "+FFA", "-FFA", "+OPOS", "-OPOS"):
                edits.append((i, (l + tok) if tok[0] == "+" else (l + tok)))
        elif k == "SOUND":
            for v in ("100,5100,0,20", "0,0,0,0", "50,20,50,20"):
                edits.append((i, f"SOUND {v}"))
        elif k == "RATE":
            for v in ("30,250,FAST", "30,250,VERY SLOW", "0,0,FAST"):
                edits.append((i, f"RATE {v}"))
        elif k == "MSR":
            for v in ("M3, LRC, NSE", "M3", "", "NSE"):
                edits.append((i, f"MSR {v}"))
    for n, (i, newl) in enumerate(edits):
        cl = lines[:]
        cl[i] = newl
        mt = "\n".join(cl) + "\n"
        p = f"/tmp/probe_v{n}.mwf"
        open(p, "w", encoding="cp1252").write(mt)
        b1 = oracle(p, f"/tmp/probe_v{n}.mwx")
        if b1 is None:
            print("REJECT", newl.strip())
            mw = f"{DLL_DIR}/mwxc.out"
            if os.path.exists(mw):
                print("   mwxc.out:", open(mw, errors='replace').read()[:120])
            continue
        diffs = [(j, b0[j], b1[j]) for j in range(min(len(b0), len(b1))) if b0[j] != b1[j]]
        ln = " ".join(f"{j:04x}:{a:02x}->{c:02x}" for j, a, c in diffs[:8])
        print(f"{newl.strip():28s} len {len(b1)-len(b0):+d}  {ln}")
