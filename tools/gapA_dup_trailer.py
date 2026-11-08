#!/usr/bin/env python3
"""gapA_dup_trailer.py — duplicate/missing POSKey refs on BOTH fixtures.

pktxc rejects a POSKey referenced by two caps (slot.count collision); the
vendor accepts and writes trailer bytes that look like counters. Sweep:
duplicate a POSKey ref onto a second cap (keeping / dropping the -L flag on
either side); dump oracle index + record hex so the trailer rule can be
derived.
"""
import subprocess, os, re

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"
SRC = {
    "dense": "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates/pos_mci128.MWF",
    "allL": f"{ORACLE_DIR}/01.mwf",
}
REF_RE = re.compile(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS[A-Za-z0-9]*)\}"')

def oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def native(p, out):
    if os.path.exists(out): os.remove(out)
    r = subprocess.run([PKTXC, p, out], capture_output=True, text=True, timeout=60, cwd=REPO)
    return (open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None), (r.stdout + r.stderr)[:200]

def recs(b):
    occ = [(k + 1, (b[4 + 2 * k] << 8) | b[5 + 2 * k]) for k in range(142)
           if (b[4 + 2 * k] << 8) | b[5 + 2 * k]]
    out = []
    for n, (pos, off) in enumerate(occ):
        end = occ[n + 1][1] if n + 1 < len(occ) else len(b)
        out.append((pos, off, end - off, b[off:end]))
    return out

def dump(tag, b):
    if b is None:
        print(f"  {tag}: <rejected>"); return
    rs = recs(b)
    print(f"  {tag}: nrec={len(rs)} first@{rs[0][1]:04x}")
    for p, o, l, h in rs[:5] + rs[-3:]:
        print(f"    POS{p} off {o:04x} len {l} {h.hex()}")

# mutators: take (lines, bytok) and return a NEW list
def dup_keepL(lines, bt):
    v = lines[:]
    j = bt["POSKey002"]
    tok = REF_RE.match(lines[j]).group(4)
    v[j] = lines[j].replace(tok, "POSKey001", 1)
    return v

def dup_drop_dst_L(lines, bt):
    v = dup_keepL(lines, bt)[:]
    j = bt["POSKey002"]
    cap = REF_RE.match(lines[j]).group(1)
    v[j] = v[j].replace(cap + "-L+", cap + "+", 1)
    return v

def drop_src_L(lines, bt):
    v = dup_keepL(lines, bt)[:]
    i = bt["POSKey001"]
    cap = REF_RE.match(lines[i]).group(1)
    v[i] = lines[i].replace(cap + "-L+", cap + "+", 1)
    return v

if __name__ == "__main__":
    for sk, src in SRC.items():
        lines = open(src, "rb").read().decode("cp1252").splitlines()
        ref = [i for i, l in enumerate(lines) if REF_RE.match(l)]
        bt = {REF_RE.match(lines[i]).group(4): i for i in ref}
        if "POSKey001" not in bt or "POSKey002" not in bt:
            print(f"skip {sk}: no POSKey001/002"); continue
        for tag, mut in (("dup_keepL", dup_keepL), ("dup_drop_dst_L", dup_drop_dst_L), ("drop_src_L", drop_src_L)):
            v = mut(lines, bt)
            p = f"/tmp/dup_{sk}_{tag}.MWF"
            open(p, "wb").write(("\r\n".join(v) + "\r\n").encode("cp1252"))
            print(f"  mutated lines: {v[bt['POSKey001']]!r} | {v[bt['POSKey002']]!r}")
            o = oracle(p, f"/tmp/dup_o_{sk}_{tag}.mwx")
            n, msg = native(p, f"/tmp/dup_n_{sk}_{tag}.mwx")
            ob = o[4:] if o and o[:4] == b"Preh" else o
            nb = n[4:] if n and n[:4] == b"Preh" else n
            print(f"== {sk}_{tag}: oracle={'ok' if ob else 'rej'} pktxc={'ok' if nb else 'rej'} msg={msg.strip()[:120]!r}")
            dump("or", ob); dump("pk", nb)
            if ob and nb:
                d = [i for i in range(min(len(ob), len(nb))) if ob[i] != nb[i]]
                print(f"  diffs {len(d)} offsets {[hex(x) for x in d[:14]]}")
            if ob and not nb:
                rs = recs(ob)
                print("  ORACLE first8 trailers: " + " ".join(f"POS{p}@{o:04x}/t={h[-1]:02x}" for p, o, l, h in [(p, o, l, h) for p, o, l, h in rs[:8]]))
