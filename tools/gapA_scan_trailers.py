#!/usr/bin/env python3
"""gapA_scan_trailers.py — offline scan of EVERY oracle .mwx in oracle_dir:
decode the POS index table, slice records, histogram the last byte of each
record, and flag files whose trailers leave {00,01}. Pair with pktxc on the
same .mwf to show which cases pktxc still gets wrong.
"""
import os, glob, subprocess

D = "/home/nonrmy/hermes-workspace/oracle_dir"
PKTXC = "/home/nonrmy/hermes-workspace/pkt-editor-cpp/b/pktxc"

def recs(b):
    occ = [(k + 1, (b[4 + 2 * k] << 8) | b[5 + 2 * k]) for k in range(142)
           if (b[4 + 2 * k] << 8) | b[5 + 2 * k]]
    out = []
    for n, (pos, off) in enumerate(occ):
        end = occ[n + 1][1] if n + 1 < len(occ) else len(b)
        out.append((pos, off, end - off, b[off:end]))
    return out

for mwx in sorted(glob.glob(f"{D}/*.mwx")):
    b = open(mwx, "rb").read()
    if len(b) < 300: continue
    if b[:4] == b"Preh": b = b[4:]
    try:
        rs = recs(b)
    except Exception:
        continue
    if not rs: continue
    strange = [(p, o, l, r) for p, o, l, r in rs if r and r[-1] not in (0x00, 0x01) and len(r) in (7, 8, 9)]
    hist = {}
    for p, o, l, r in rs:
        if r: hist[r[-1]] = hist.get(r[-1], 0) + 1
    if strange:
        mwf = mwx.replace(".mwx", ".mwf")
        name = os.path.basename(mwx)
        print(f"{name}: len={len(b)} nrec={len(rs)} strange_trailers={len(strange)} " +
              " ".join(f"POS{p}@{o:04x}/l{l}/t{r[-1]:02x}" for p, o, l, r in strange[:8]))
        r = subprocess.run([PKTXC, mwf, "/tmp/scan_n.mwx"], capture_output=True, text=True, timeout=60,
                           cwd="/home/nonrmy/hermes-workspace/pkt-editor-cpp")
        nb = open("/tmp/scan_n.mwx", "rb").read() if os.path.exists("/tmp/scan_n.mwx") else None
        if nb and nb[:4] == b"Preh": nb = nb[4:]
        if nb:
            nrs = recs(nb)
            nd = [x for x in range(min(len(rs), len(nrs))) if rs[x][3] != nrs[x][3]]
            print(f"   pktxc: nrec={len(nrs)} rec-diffs={len(nd)} first={[(rs[i][0], rs[i][3].hex(), nrs[i][3].hex()) for i in nd[:6]]}")
        else:
            print("   pktxc: REJECTED")
