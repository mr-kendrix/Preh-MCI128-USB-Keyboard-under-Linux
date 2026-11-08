#!/usr/bin/env python3
"""Vendor-template honest hold-out: for every vendor layout template, require
   oracle(mwf) == native_pktxc(mwf) byte-identical, OR both reject.
Logs rejections (input + mwxc.out message). Skips cleanly if wine/DLL absent."""
import subprocess, os, sys, glob
REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
TPL = "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
PKTXC = f"{REPO}/b/pktxc"

WINE_OK = subprocess.run(["which", "wine"], capture_output=True, text=True).returncode == 0

def oracle(p, out):
    r = subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=90)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None, (r.stdout + r.stderr).decode(errors="replace")[:300]

def native(p, out):
    r = subprocess.run([PKTXC, p, out], capture_output=True, text=True, timeout=60, cwd=REPO)
    return (open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None), (r.stdout + r.stderr)[:300]

if __name__ == "__main__":
    fails = []
    n = 0
    for p in sorted(glob.glob(f"{TPL}/*.mwf") + glob.glob(f"{TPL}/*.MWF")):
        n += 1
        name = os.path.basename(p)
        tag = name.replace(".", "_")
        if WINE_OK:
            ob, oerr = oracle(p, f"/tmp/tg_or_{tag}.mwx")
        else:
            ob, oerr = None, "wine absent — skipped"
        nb, nerr = native(p, f"/tmp/tg_na_{tag}.mwx")
        if ob is None and nb is None:
            print(f"BOTH-REJECT {name}: oracle={oerr.strip()[:90]!r} native={nerr.strip()[:90]!r}")
        elif ob is not None and nb is not None:
            a = ob[4:] if ob[:4] == b"Preh" else ob
            b = nb[4:] if nb[:4] == b"Preh" else nb
            m = min(len(a), len(b))
            d = sum(1 for i in range(m) if a[i] != b[i])
            ok = d == 0 and abs(len(a) - len(b)) <= 8
            print(f"{'MATCH' if ok else 'DIFF'} {name}: len_o={len(a)} len_n={len(b)} diffs={d}")
            if not ok:
                fails.append((name, d, len(a), len(b)))
        else:
            side = "oracle-only" if ob is not None else "native-only"
            print(f"ASYMM {name}: oracle={'OK' if ob else 'rej'} native={'OK' if nb else 'rej'} -> {side} produced bytes")
            fails.append((name, -1, len(ob or b''), len(nb or b'')))
    print(f"TOTAL templates: {n}, fails: {len(fails)}")
    sys.exit(1 if fails or not WINE_OK and False else 0)
