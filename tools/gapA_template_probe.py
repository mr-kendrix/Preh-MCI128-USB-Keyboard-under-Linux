#!/usr/bin/env python3
"""gapA_template_probe.py — trailer semantics on the DENSE no-L template
pos_mci128.MWF (unlike 01.mwf where every ref carries -L and flags are
indistinguishable). Mutate ref lines, compile with oracle + pktxc, and
print each record's trailer byte + diffs with context."""
import subprocess, os, re

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
TPL = "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates/pos_mci128.MWF"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"

def oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([PKTXC, p, out], capture_output=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

base_raw = open(TPL, "rb").read().decode("cp1252")
lines = base_raw.splitlines()          # keep no keepends; rewrite with \r\n
pat = re.compile(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(POS\w+)\}"')
REF = [i for i, l in enumerate(lines) if pat.match(l)]
print(f"ref_lines={len(REF)}")

def write(tag, mutated):
    p = f"/tmp/gt_{tag}.MWF"
    open(p, "wb").write(("\r\n".join(mutated) + "\r\n").encode("cp1252"))
    o = oracle(p, f"/tmp/gt_o_{tag}.mwx")
    n = native(p, f"/tmp/gt_n_{tag}.mwx")
    if o is None or n is None:
        print(f"{tag}: ASYMM oracle={'ok' if o else 'rej'} pktxc={'ok' if n else 'rej'}"); return
    a = o[4:] if o[:4] == b"Preh" else o
    c = n[4:] if n[:4] == b"Preh" else n
    d = [i for i in range(min(len(a), len(c))) if a[i] != c[i]]
    print(f"{tag}: len {len(a)}/{len(c)} diffs={len(d)} {[hex(x) for x in d[:12]]}")
    # decode the oracle record stream: POSKey recs 8B (1d 68 xx 00*4 T), named 7B
    # locate stream start: first 1d 68 at/after 0x13c
    s = a.find(b"\x1d\x68", 0x13c)
    recs = []
    i = s
    while i + 1 < len(a):
        if a[i] == 0x1d and a[i+1] == 0x68:
            code = a[i+2]; recs.append((code, a[i+7], i)); i += 8
        elif a[i] == 0x1e:
            code = f"{a[i+1]:02x}"
            # named code may have shifted high byte (0x1d.. collides) — assume 1e xx 7B
            code = a[i:i+2].hex(); recs.append((code, a[i+6], i)); i += 7
        else:
            break
    print(f"  oracle recs ({len(recs)}): " + " ".join(f"{c}/T={t:02x}@{o_:04x}" for c, t, o_ in recs[:10]) + f" ... last={recs[-1][0]}/{recs[-1][1]:02x}@{recs[-1][2]:04x}")
    for i_ in d[:8]:
        print(f"  off {i_:04x}: or={a[i_]:02x} pk={c[i_]:02x} | or ctx {a[max(0,i_-8):i_+9].hex()} pk ctx {c[max(0,i_-8):i_+9].hex()}")

def refidx(tok):
    return [i for i in REF if pat.match(lines[i]).group(4) == tok]

def swap(t1, t2):
    v = lines[:]
    i, j = refidx(t1)[0], refidx(t2)[0]
    v[i] = lines[i].replace(t1, t2); v[j] = lines[j].replace(t2, t1)
    return v

def move_line(i, j):
    v = lines[:]
    v[i], v[j] = lines[j], lines[i]
    return v

def shuffle_k(seed, k=6):
    import random
    idx = REF[:k]
    block = [lines[i] for i in idx]
    random.Random(seed).shuffle(block)
    v = lines[:]
    for pos_, i in enumerate(idx): v[i] = block[pos_]
    return v

def drop_caps(drops):  # remove ref lines (caps unbound?)
    v = lines[:]
    for t in drops:
        i = refidx(t)[0]
        v[i] = ""   # blank line
    return v

if __name__ == "__main__":
    write("base", lines)
    write("swap_1_2", swap("POSKey001", "POSKey002"))
    write("swap_1_57", swap("POSKey001", "POSKey057"))
    write("swap_adj_57_58", swap("POSKey057", "POSKey058"))
    write("move_first_to_last_cap", move_line(REF[0], REF[-1]))
    write("shuf_6", shuffle_k(6))
    write("shuf_13", shuffle_k(13))
    write("drop_last_cap", drop_caps(["POSKey128"]))
    write("drop_mid_cap", drop_caps(["POSKey057"]))
