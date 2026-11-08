#!/usr/bin/env python3
"""named_dump.py — parse the named-record stream of the ORACLE output for
the MCI-family fixtures and compare with pktxc; print trailer bytes + slot
guesses so the trailer rule can be derived exactly."""
import subprocess, os, re

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
TPL = "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates"
ORACLE_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE_SH = f"{REPO}/oracle/run_oracle.sh"
PKTXC = f"{REPO}/b/pktxc"

def run_oracle(p, out):
    subprocess.run(["bash", ORACLE_SH, ORACLE_DIR, p, out], capture_output=True, timeout=120)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def run_native(p, out):
    if os.path.exists(out): os.remove(out)
    subprocess.run([PKTXC, p, out], capture_output=True, timeout=60, cwd=REPO)
    return open(out, "rb").read() if os.path.exists(out) and os.path.getsize(out) else None

def parse_named(body):
    """records: POSKey = 1d 68 <1B code> 00 00 00 00 T (8B). named = 2B code
    00 00 00 00 T (7B). Walk the stream from first record to end."""
    s = body.find(b"\x1d\x68")
    if s < 0: return
    i = s
    while i + 6 < len(body):
        if body[i] == 0x1d and body[i+1] == 0x68:
            print(f"  POS c={body[i+2]:02x} t={body[i+7]:02x} @{i:04x}"); i += 8
        elif body[i+2] == 0 and body[i+3] == 0 and body[i+4] == 0 and body[i+5] == 0:
            print(f"  NAMED c={body[i]:02x}{body[i+1]:02x} t={body[i+6]:02x} @{i:04x}"); i += 7
        else:
            print(f"  ??? @{i:04x}: {body[i:i+7].hex()}"); break

for tag, p in (("mci128", f"{TPL}/pos_mci128.MWF"),
               ("mci96", f"{TPL}/pos_mci96.MWF")):
    o = run_oracle(p, f"/tmp/nd_o_{tag}.mwx")
    n = run_native(p, f"/tmp/nd_n_{tag}.mwx")
    a = o[4:] if o[:4] == b"Preh" else o
    c = n[4:] if n[:4] == b"Preh" else n
    d = [i for i in range(min(len(a), len(c))) if a[i] != c[i]]
    print(f"== {tag}: len {len(a)}/{len(c)} diffs={[hex(x) for x in d]}")
    # dump last 200 bytes of the record stream region for both
    tail_a, tail_c = a[-260:], c[-260:]
    print("ORACLE named stream (last 260B):")
    parse_named(tail_a)
    print("PKTXC  named stream (last 260B):")
    parse_named(tail_c)
    # also: print the .mwf named ref lines + token runs for this template
    src = open(p, "rb").read().decode("cp1252", errors="replace")
    print("REF lines (named):")
    for l in src.splitlines():
        if re.search(r'\{POS(?!Key[0-9])', l) or re.search(r'\{POSKeylock', l):
            print("  " + l.strip())
