#!/usr/bin/env python3
"""gapA_trailer_map.py — FULL trailer maps for base + ref mutations.

For each fixture print the oracle trailer byte per record (pos, off, len,
trailer) so the values 00/01/02/... can be compared against pktxc's
next-declared-flag rule. Mutations: swap POSKey tokens, duplicate refs (2 and
3 caps), -L drops/adds. Also pktxc's own map for the accepted cases.
"""
import subprocess, os, re, itertools

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
    return (open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None)

def tmap(b):
    occ = [(k + 1, (b[4 + 2 * k] << 8) | b[5 + 2 * k]) for k in range(142)
           if (b[4 + 2 * k] << 8) | b[5 + 2 * k]]
    out = []
    for n, (pos, off) in enumerate(occ):
        end = occ[n + 1][1] if n + 1 < len(occ) else len(b)
        out.append((pos, off, end - off, b[end - 1] if end - off >= 4 else None))
    return out

def line_map(lines):
    ref = [i for i, l in enumerate(lines) if REF_RE.match(l)]
    return [(REF_RE.match(lines[i]).group(1), REF_RE.match(lines[i]).group(4),
             bool(REF_RE.match(lines[i]).group(2))) for i in ref]

def show(tag, m):
    vals = {}
    for pos, off, l, t in m:
        vals.setdefault(t, []).append((pos, off, l))
    def key(kv):
        return str(kv[0])
    print(f"  {tag}: trailer histogram " + " ".join(f"t={k if k is None else format(k,'02x')}:{len(v)}" for k, v in sorted(vals.items(), key=key)))
    print(f"  first12: " + " ".join(f"p{p}@{o:04x}/l{l}/t{(t if t is None else format(t,'02x'))}" for p, o, l, t in m[:12]))

def run(tag, srckey, mutate):
    lines = open(SRC[srckey], "rb").read().decode("cp1252").splitlines()
    ref = [i for i, l in enumerate(lines) if REF_RE.match(l)]
    tok2i = {REF_RE.match(lines[i]).group(4): i for i in ref}
    v = mutate(lines, tok2i)
    p = f"/tmp/tm_{tag}.MWF"
    open(p, "wb").write(("\r\n".join(v) + "\r\n").encode("cp1252"))
    o = oracle(p, f"/tmp/tm_o_{tag}.mwx")
    n = native(p, f"/tmp/tm_n_{tag}.mwx")
    ob = o[4:] if o and o[:4] == b"Preh" else o
    nb = n[4:] if n and n[:4] == b"Preh" else n
    print(f"== {tag}: oracle={'ok' if ob else 'rej'} pktxc={'ok' if nb else 'rej'}")
    if ob: show("or", tmap(ob))
    if nb: show("pk", tmap(nb))
    if ob and nb:
        d = [i for i in range(min(len(ob), len(nb))) if ob[i] != nb[i]]
        print(f"  diffs {len(d)} {[hex(x) for x in d[:14]]}")
    print("  ref lines first4: " + " | ".join(f"{c}/{t}/L{int(f)}" for c, t, f in line_map(v)[:4]))

def swap(i, j):
    def f(lines, t2i):
        v = lines[:]
        a, b = t2i[i], t2i[j]
        v[a] = lines[a].replace(i, j, 1)
        v[b] = lines[b].replace(j, i, 1)
        return v
    return f

def dup(dst_tok, onto_caps):
    def f(lines, t2i):
        v = lines[:]
        for cap_tok in onto_caps:
            idx = next(i for i in range(len(v)) if REF_RE.match(v[i]) and REF_RE.match(v[i]).group(1) == cap_tok)
            cur = REF_RE.match(v[idx]).group(4)
            v[idx] = v[idx].replace(cur, dst_tok, 1)
        return v
    return f

def drop_L_caps(caps):
    def f(lines, t2i):
        v = lines[:]
        for cap in caps:
            for i, l in enumerate(v):
                m = REF_RE.match(l)
                if m and m.group(1) == cap and m.group(2):
                    v[i] = l.replace(cap + "-L+", cap + "+", 1)
        return v
    return f

if __name__ == "__main__":
    for sk in SRC:
        caps = None
        run(f"{sk}_base", sk, lambda l, t: l[:])
        run(f"{sk}_swap1_2", sk, swap("POSKey001", "POSKey002"))
        run(f"{sk}_swap1_3", sk, swap("POSKey001", "POSKey003"))
        run(f"{sk}_dup001_on_A03", sk, dup("POSKey001", ["A03"]))
        run(f"{sk}_dup001_on_A02_A03", sk, dup("POSKey001", ["A02", "A03"]))
        run(f"{sk}_dropL_A01", sk, drop_L_caps(["A01"]))
        run(f"{sk}_dropL_A01_A02", sk, drop_L_caps(["A01", "A02"]))
