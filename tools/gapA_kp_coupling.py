#!/usr/bin/env python3
"""gapA_kp_coupling.py — read EXISTING oracle outputs for KEYPRINT/POS
mismatch fixtures in oracle_dir and dump the trailer map + code bytes so the
trailer semantics (mismatch count vs next-declared -L flag) can be confirmed.
"""
import os, re

D = "/home/nonrmy/hermes-workspace/oracle_dir"
REF_RE = re.compile(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS[A-Za-z0-9]*)\}"')
KP_RE = re.compile(r'^!@KEYPRINT:([A-Z0-9\-]{2,5}) "([^"]*)"')

def tmap(b):
    occ = [(k + 1, (b[4 + 2 * k] << 8) | b[5 + 2 * k]) for k in range(142)
           if (b[4 + 2 * k] << 8) | b[5 + 2 * k]]
    out = []
    for n, (pos, off) in enumerate(occ):
        end = occ[n + 1][1] if n + 1 < len(occ) else len(b)
        out.append((pos, off, end - off, b[off:end]))
    return out

def summarize(name, b):
    ms = tmap(b)
    hist = {}
    for p, o, l, r in ms:
        hist[r[-1] if r else None] = hist.get(r[-1] if r else None, 0) + 1
    print(f"{name}: nrec={len(ms)} hist={ {format(k,'02x') if k is not None else 'None':v for k,v in hist.items()} }")
    interesting = [(p, o, l, r.hex()) for p, o, l, r in ms if r and r[-1] not in (0, 1)]
    for row in interesting[:12]:
        print(f"   POS{row[0]} off {row[1]:04x} len {row[2]} {row[3]}")
    print("   first6: " + " ".join(f"p{p}@{o:04x}/l{l}/t{r[-1]:02x}" for p, o, l, r in ms[:6]))

for tag in ("01.mwf", "_mut_KP_A01_pos128.mwf", "_mut_KP_B13_pos001.mwf", "_mut_KP_H16_pos001.mwf",
            "_mut_KP_A01_x1.mwf", "_mut_KP_A01_f4_1.mwf", "_mut_KP_A01_f5_1.mwf"):
    mwf = os.path.join(D, tag)
    if not os.path.exists(mwf):
        print(f"{tag}: mwf missing"); continue
    mwx = mwf.replace(".mwf", ".mwx") if tag != "01.mwf" else os.path.join(D, "out01.mwx")
    if not os.path.exists(mwx):
        print(f"{tag}: no oracle output"); continue
    b = open(mwx, "rb").read()
    if b[:4] == b"Preh": b = b[4:]
    lines = open(mwf, "rb").read().decode("cp1252").splitlines()
    kp = [KP_RE.match(l).groups() for l in lines if KP_RE.match(l)]
    ref = [(REF_RE.match(l).group(1), REF_RE.match(l).group(4)) for l in lines if REF_RE.match(l)]
    mism = [(c, t) for c, t in ref if any(kc == c and not kpp.startswith("POS\\line") for kc, kpp in kp)]
    kpmap = {kc: kpp for kc, kpp in kp}
    dig = {t: re.search(r"(\d+)", t) for _, t in ref}
    posnum = {t: int(m.group(1)) for t, m in dig.items() if m}
    mism = [(c, t) for c, t in ref if c in kpmap and t in posnum and kpmap[c] != f"POS\\line{posnum[t]:03d}"]
    print(f"== {tag} len={len(b)}: ref={len(ref)} kp={len(kp)} kp/pos mismatches={len(mism)} {mism[:6]}")
    summarize(tag, b)
