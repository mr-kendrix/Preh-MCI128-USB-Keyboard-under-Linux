#!/usr/bin/env python3
"""gapA_trailer_semantics.py — decode the POS record stream via the index
table and print (POS, code, mid4, trailer, offset) for base + ref-line
mutations on the DENSE no-L template pos_mci128.MWF and on 01.mwf (all -L).
Goal: pin what the trailer byte encodes (-L flag? count? chain marker?).
"""
import subprocess, os, re, json

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
TPL = "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates/pos_mci128.MWF"
MWF01 = f"{ORACLE_DIR}/01.mwf"
PKTXC = f"{REPO}/b/pktxc"

def oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([PKTXC, p, out], capture_output=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def decode(b):
    """b = body (no Preh). index table: 142 u16 BE entries at 4..288."""
    idx = [(4 + 2 * k, (b[4 + 2 * k] << 8) | b[5 + 2 * k]) for k in range(142)]
    occ = [(k + 1, off) for k, (_, off) in enumerate(idx) if off]
    if not occ:
        return []
    stream_end = len(b)
    recs = []
    for n, (pos, off) in enumerate(occ):
        end = occ[n + 1][1] if n + 1 < len(occ) else stream_end
        body = b[off:end]
        recs.append((pos, off, end - off, body.hex()))
    return recs

def show(tag, b):
    if b is None:
        print(f"  {tag}: rejected"); return
    recs = decode(b)
    nz = [(p, o, ln, hx) for p, o, ln, hx in recs]
    print(f"  {tag}: {len(recs)} recs; first@{recs[0][1] if recs else 0:04x}; last@{recs[-1][1]:04x}")
    for p, o, ln, hx in recs[:4] + recs[-6:]:
        print(f"    POS{p} off {o:04x} len {ln} {hx}")
    return recs

REF_RE = re.compile(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS[A-Za-z0-9]*)\}"')

def mk(path, tag, fn, lines):
    v = lines[:]
    if fn: fn(v)
    p = f"/tmp/sem_{tag}.mwf"
    open(p, "wb").write(("\r\n".join(v) + "\r\n").encode("cp1252"))
    o = oracle(p, f"/tmp/sem_o_{tag}.mwx")
    n = native(p, f"/tmp/sem_n_{tag}.mwx")
    ob = o[4:] if o and o[:4] == b"Preh" else o
    nb = n[4:] if n and n[:4] == b"Preh" else n
    print(f"== {tag}: oracle={'ok' if ob else 'rej'} pktxc={'ok' if nb else 'rej'}")
    ro = show("or", ob)
    rn = show("pk", nb)
    if ob and nb:
        d = [i for i in range(min(len(ob), len(nb))) if ob[i] != nb[i]]
        print(f"  diffs {len(d)} {[hex(x) for x in d[:10]]}")
    return ob, nb

for src in (TPL, MWF01):
    lines = open(src, "rb").read().decode("cp1252").splitlines()
    ref = [i for i, l in enumerate(lines) if REF_RE.match(l)]
    posk = [i for i in ref if REF_RE.match(lines[i]).group(4).startswith("POSKey")]
    named = [i for i in ref if not REF_RE.match(lines[i]).group(4).startswith("POSKey")]
    hasL = [i for i in ref if REF_RE.match(lines[i]).group(2)]
    base = "WITH_L" if hasL else "no_L"
    print(f"### {os.path.basename(src)} ref={len(ref)} poskey={len(posk)} named={len(named)} hasL={len(hasL)} ({base})")
    mk(src, f"{os.path.basename(src)}_base", None, lines)
    def add_L(i):
        def f(v):
            m = REF_RE.match(lines[i])
            v[i] = lines[i].replace(m.group(1) + "+", m.group(1) + "-L+", 1)
        return f
    mk(src, f"{os.path.basename(src)}_addL0", add_L(posk[0]), lines)
    mk(src, f"{os.path.basename(src)}_addL01", lambda v: (add_L(posk[0])(v), add_L(posk[1])(v)), lines)
    mk(src, f"{os.path.basename(src)}_addL_named", add_L(named[0]), lines)
    mk(src, f"{os.path.basename(src)}_addL_last", add_L(posk[-1]), lines)
    def tok_swap(i, j):
        def f(v):
            ta = REF_RE.match(lines[i]).group(4); tb = REF_RE.match(lines[j]).group(4)
            v[i] = lines[i].replace(ta, tb, 1); v[j] = lines[j].replace(tb, ta, 1)
        return f
    mk(src, f"{os.path.basename(src)}_swap01", tok_swap(posk[0], posk[1]), lines)
    mk(src, f"{os.path.basename(src)}_swap01_L0", lambda v: (tok_swap(posk[0], posk[1])(v), add_L(posk[0])(v)), lines)
    mk(src, f"{os.path.basename(src)}_dropL0" if hasL else f"{os.path.basename(src)}_dropL_none", (lambda v: v.__setitem__(posk[0], lines[posk[0]].replace("-L+", "+", 1)) if hasL else None), lines)
