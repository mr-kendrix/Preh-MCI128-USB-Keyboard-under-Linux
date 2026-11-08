#!/usr/bin/env python3
"""Cluster the holdout failures into byte-region classes (see holdout_classes_out in argv[2]).
Usage: holdout_classes.py <outdir> [outclassfile]
Class key = (len_delta, first 8 (offset,exp,ours) diff triples). Prints top 15 with counts
so the next lane knows WHICH encodings are wrong, not just that they are."""
import json, collections, sys

outdir = sys.argv[1]
PKTXC = "/home/nonrmy/hermes-workspace/pkt-editor-cpp/b/pktxc"
import subprocess, os
SC = "/home/nonrmy/.hermes/profiles/coder/cache/scratch/hlclasses"
os.makedirs(SC, exist_ok=True)

def payload(p):
    d = open(p, "rb").read()
    return d[4:] if d[:4] == b"Preh" else d

classes = collections.Counter()
members = collections.defaultdict(list)
nfail = 0
for line in open(f"{outdir}/manifest.jsonl"):
    e = json.loads(line)
    if not e["expected"]:
        continue
    if os.path.exists(f"{SC}/o.mwx"):
        os.remove(f"{SC}/o.mwx")   # stale output would fake byte-diffs for pktxc-rejected variants
    subprocess.run([PKTXC, e["path"], f"{SC}/o.mwx"], capture_output=True, timeout=60, cwd="/home/nonrmy/hermes-workspace/pkt-editor-cpp")
    ours = payload(f"{SC}/o.mwx") if os.path.exists(f"{SC}/o.mwx") else b""
    exp = payload(e["expected"])
    if ours == exp:
        continue
    nfail += 1
    idx = [i for i in range(min(len(exp), len(ours))) if exp[i] != ours[i]]
    key = (len(exp) - len(ours), tuple((i, exp[i], ours[i]) for i in idx[:8]))
    classes[key] += 1
    members[key].append(e["variant"])

rows = []
for (dl, sig), cnt in classes.most_common(15):
    desc = "; ".join(f"@{i:#05x} exp {e:02x} ours {o:02x}" for i, e, o in sig)
    print(f"[{cnt:4d}] len_delta={dl:+d} {members[(dl, sig)][0]} | {desc[:220]}")
    rows.append(f"{cnt}\t{dl}\t{members[(dl, sig)][0]}\t{desc}")
print(f"total_fail={nfail} classes={len(classes)}")
if len(sys.argv) > 2:
    with open(sys.argv[2], "w") as fh:
        fh.write(f"total_fail={nfail} classes={len(classes)}\n")
        for r in rows:
            fh.write(r + "\n")
