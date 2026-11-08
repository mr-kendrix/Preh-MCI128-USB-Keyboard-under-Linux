#!/usr/bin/env python3
"""Structural variant generator: swaps, moves, deletions, duplicates, reordering,
empty layouts, max sizes. Emits the same manifest.jsonl schema as variant_gen.py
so judge.py and the reject-agreement check can consume it."""
import json, os, random, subprocess, sys

ORACLE_SH = "/home/nonrmy/hermes-workspace/pkt-editor-cpp/oracle/run_oracle.sh"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"

def lines_of(t): return t.splitlines()

def assign_idx(ls):
    return [i for i, l in enumerate(ls)
            if l[:1].isalpha() and ": \"" in l and ("{POSKey" in l or "{POS" in l)]

def mutate_struct(base_lines, rng):
    ls = lines_of_base = base_lines[:]
    ai = assign_idx(ls)
    out = []
    if len(ai) >= 2:
        # swap two assignment targets
        i, j = rng.sample(ai, 2)
        l2 = ls[:]; l2[i], l2[j] = l2[j], l2[i]
        out.append(("swap_ij", l2))
        # move: retarget assignment i onto cap-prefix of assignment j
        i, j = rng.sample(ai, 2)
        l2 = ls[:]
        pi = ls[i].split(":", 1)[0]; pj = ls[j].split(":", 1)[0]
        l2[i] = ls[i].replace(pi, pj, 1)
        out.append(("move_i_onto_j", l2))
        # delete one assignment line
        k = rng.choice(ai); l2 = ls[:]; del l2[k]
        out.append(("delete_k", l2))
        # duplicate one assignment line
        k = rng.choice(ai); l2 = ls[:]; l2.insert(k + 1, ls[k])
        out.append(("dup_k", l2))
        # reorder: rotate a window of assignments
        s = min(ai[:6]); e = max(ai[:6]); l2 = ls[:s:e + 1][::-1] + ls[e + 1:]
        l2 = ls[:s] + ls[s:e + 1][::-1] + ls[e + 1:]
        out.append(("reorder_win", l2))
    # empty layout: no assignments at all
    out.append(("empty", [l for l in ls if l not in set(ls[i] for i in assign_idx(ls))]))
    # max size: add assignment lines for caps not present
    caps = set(l.split(":", 1)[0].split("-")[0].split("+")[0] for l in ls if ": \"" in l)
    extra = [f"G{c:02d}+K/K/P/M/R: \"{{POSKey{100+ k}}}\"" for k, c in enumerate(range(1, 17)) if f"G{c:02d}" not in caps][:16]
    out.append(("max_extra", ls + extra))
    return out

def run_oracle(p, out):
    r = subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out],
                       capture_output=True, text=True, timeout=90)
    return r.returncode

if __name__ == "__main__":
    base = sys.argv[1]; outdir = sys.argv[2]; seed = int(sys.argv[3]) if len(sys.argv) > 3 else 13
    rng = random.Random(seed)
    text = open(base, encoding="cp1252", errors="replace").read()
    ls = [l.rstrip("\r") for l in text.splitlines()]
    os.makedirs(f"{outdir}/variants", exist_ok=True); os.makedirs(f"{outdir}/expected", exist_ok=True)
    mani = open(f"{outdir}/manifest.jsonl", "w")
    n = 0
    for name, ml in mutate_struct(ls, rng):
        body = "\n".join(ml) + "\n"
        vn = f"{name}_{seed}"
        p = f"{outdir}/variants/{vn}.mwf"
        open(p, "w", encoding="cp1252").write(body)
        exp = f"{outdir}/expected/{vn}.mwx"
        rc = run_oracle(p, exp)
        sha = ""
        if os.path.exists(exp) and os.path.getsize(exp) > 0:
            sha = __import__("hashlib").sha256(open(exp, "rb").read()).hexdigest()
        mw = f"{ORACLE_DIR}/mwxc.out"
        msg = open(mw, encoding="cp1252", errors="replace").read() if os.path.exists(mw) else ""
        mani.write(json.dumps({"variant": vn, "path": p, "expected": exp if sha else None,
                               "sha256": sha, "rc": rc, "stdout": "", "mwxc_out": msg[:300]}) + "\n")
        mani.flush()
        print(f"{vn}: rc={rc} bytes={os.path.getsize(exp) if os.path.exists(exp) else 0}")
        n += 1
    print("total:", n)
