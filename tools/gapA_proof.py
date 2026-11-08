#!/usr/bin/env python3
"""gapA_proof.py — fresh variants exercising cap<->POSKey binding order and -L
flag semantics. Gate: pktxc == oracle byte-equal AND same length."""
import os, re, random, subprocess
REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
D = "/home/nonrmy/hermes-workspace/oracle_dir"
BASE = open(f"{D}/01.mwf", "rb").read().split(b"\r\n")
pat = re.compile(rb'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS\w+)\}"')
REF = [i for i, l in enumerate(BASE) if pat.match(l)]
def tok(l): return pat.match(l).group(4)
POSK = [i for i in REF if tok(BASE[i]).startswith(b"POSKey") and tok(BASE[i]) != b"POSKeylock"]
NAMED = [i for i in REF if i not in POSK]
print(f"ref={len(REF)} poskey={len(POSK)} named={len(NAMED)}")

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
    p = f"/tmp/gb_{tag}.mwf"
    open(p, "wb").write(b"\r\n".join(v))
    o = orc(p, f"/tmp/gb_{tag}_o.mwx")
    n = native(p, f"/tmp/gb_{tag}_n.mwx")
    if o is None and n is None:
        print(f"{tag}: both-reject OK"); return
    if o is None or n is None:
        print(f"{tag}: ASYMM oracle={'ok' if o else 'rej'} pktxc={'ok' if n else 'rej'}"); return
    a = o[4:] if o[:4] == b"Preh" else o
    b = n[4:] if n[:4] == b"Preh" else n
    d = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    ok = not d and len(a) == len(b)
    print(f"{tag}: {'MATCH' if ok else 'DIFF'} len {len(a)}/{len(b)} diffs {len(d)} at {[hex(x) for x in d[:6]]}")
    if d:
        s = d[0]
        for src, lab in ((a, "or"), (b, "pk")):
            print(f"   {lab}[{s-1:04x}..]: " + " ".join(f"{x:02x}" for x in src[max(0, s - 1):s + 11]))

def tok_swap(a_i, b_i, xa, xb):
    return lambda v: (v.__setitem__(a_i, BASE[a_i].replace(xa, xb)), v.__setitem__(b_i, BASE[b_i].replace(xb, xa)))

def lab_swap(i, j):
    def f(v):
        la = pat.match(BASE[i]).group(1); lb = pat.match(BASE[j]).group(1)
        v[i] = BASE[i].replace(la, lb, 1); v[j] = BASE[j].replace(lb, la, 1)
    return f

def move_lines(i, j):
    return lambda v: (v.__setitem__(i, BASE[j]), v.__setitem__(j, BASE[i]))

def shuffle(seed):
    def f(v):
        block = [BASE[i] for i in POSK]
        rng = random.Random(seed); rng.shuffle(block)
        for j, i in enumerate(POSK): v[i] = block[j]
    return f

def drop_L(i):
    return lambda v: v.__setitem__(i, BASE[i].replace(b"-L+", b"+", 1))

if __name__ == "__main__":
    mk("tok_swap_0_1", tok_swap(POSK[0], POSK[1], b"POSKey001", b"POSKey002"))
    mk("tok_swap_far", tok_swap(POSK[0], POSK[-1], b"POSKey001", b"POSKey128"))
    mk("lab_swap_0_1", lab_swap(POSK[0], POSK[1]))
    mk("lab_swap_far", lab_swap(POSK[0], POSK[-1]))
    mk("move_far", move_lines(POSK[0], POSK[-1]))
    mk("shuf_5", shuffle(5))
    mk("shuf_11", shuffle(11))
    mk("drop_L_0", drop_L(POSK[0]))
    mk("drop_L_last", drop_L(POSK[-1]))
    mk("drop_L_named0", drop_L(NAMED[0]))
    mk("drop_L_named_all", lambda v: [v.__setitem__(i, BASE[i].replace(b"-L+", b"+", 1)) for i in NAMED])
    mk("drop_L_poskey_all", lambda v: [v.__setitem__(i, BASE[i].replace(b"-L+", b"+", 1)) for i in POSK])
    mk("drop_L_two", lambda v: (v.__setitem__(POSK[0], BASE[POSK[0]].replace(b"-L+", b"+", 1)),
                                v.__setitem__(POSK[1], BASE[POSK[1]].replace(b"-L+", b"+", 1))))
