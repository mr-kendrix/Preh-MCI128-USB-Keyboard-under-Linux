#!/usr/bin/env python3
"""Hold-out judge loop: pktxc vs vendor oracle, strict stop rule.

Usage:
  holdout_loop.py gen   <outdir> <seed> [base ...]   # generate unseen variants + oracle expected bytes
  holdout_loop.py run   <outdir>                     # judge b/pktxc vs manifest (strict byte+length equal)
  holdout_loop.py score <outdir>                     # compact verdict: counts + diff classes

Stop rule (fixed, do not relax):
  - expected bytes always come from the oracle (wine + mwxc32.dll); pktxc never sees them.
  - a variant counts as PASS only if pktxc output is BYTE-EQUAL and SAME LENGTH as the oracle
    payload (no trailing-padding tolerance, no reference-container patching).
  - oracle-rejected variants count as REJECT-OK only when pktxc also refuses (rc != 0 or no output).
  - declare "gate passed" only when: captured 01-04 payloads + every unseen variant PASS, 0 fail.
Generation uses variant_gen.mutate_lines; unseen = seed never used while fitting pktxc.
"""
import os, sys, json, hashlib, subprocess, random, importlib.util

REPO = "/home/nonrmy/hermes-workspace/pkt-editor-cpp"
PKTXC = f"{REPO}/b/pktxc"
DLL_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"
ORACLE = "/home/nonrmy/hermes-workspace/oracle_files/run_oracle.sh"
CAPTURED = [(n, f"{DLL_DIR}/{n}.mwf", f"{REPO}/fixtures/captured/{n}.mwx")
            for n in ("01", "02", "03", "04")]
SC = "/home/nonrmy/.hermes/profiles/coder/cache/scratch/holdout"

spec = importlib.util.spec_from_file_location("vg", f"{REPO}/tools/variant_gen.py")
vg = importlib.util.module_from_spec(spec); spec.loader.exec_module(vg)


def oracle(mwf, out):
    if os.path.exists(out):
        os.remove(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    r = subprocess.run(["bash", ORACLE, DLL_DIR, mwf, out], capture_output=True, text=True, timeout=180)
    d = open(out, "rb").read() if (os.path.exists(out) and os.path.getsize(out)) else b""
    d = d[4:] if d[:4] == b"Preh" else d
    msg = ""
    try:
        msg = open(f"{DLL_DIR}/mwxc.out", encoding="cp1252", errors="replace").read().strip()[:200]
    except FileNotFoundError:
        pass
    return r.returncode, d, msg


def payload(path):
    d = open(path, "rb").read()
    return d[4:] if d[:4] == b"Preh" else d


def run_pktxc(mwf, out):
    if os.path.exists(out):
        os.remove(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    r = subprocess.run([PKTXC, mwf, out], capture_output=True, text=True, timeout=60, cwd=REPO)
    d = open(out, "rb").read() if (os.path.exists(out) and os.path.getsize(out)) else b""
    return r, d[4:] if d[:4] == b"Preh" else d


def diffruns(a, b, gap=3, lim=6):
    n = min(len(a), len(b))
    idx = [i for i in range(n) if a[i] != b[i]]
    runs = []
    for i in idx:
        if runs and i - runs[-1][-1] <= gap:
            runs[-1].append(i)
        else:
            runs.append([i])
    return runs, idx


def gen(outdir, seed, bases):
    os.makedirs(f"{outdir}/variants", exist_ok=True)
    os.makedirs(f"{outdir}/expected", exist_ok=True)
    rows = []
    for n, mwf, exp in CAPTURED:
        rows.append({"variant": f"captured.{n}", "path": mwf, "expected": exp,
                     "sha256": hashlib.sha256(payload(exp)).hexdigest(), "rc": 0,
                     "stdout": "captured USBPcap payload", "mwxc_out": "", "holdout": True})
    for base in bases:
        text = open(base, encoding="cp1252", errors="replace").read()
        rng = random.Random(seed)
        singles = []
        for name, mt in vg.mutate_lines(text):
            if mt is None:
                continue
            singles.append((name, mt))
            write_variant(outdir, f"{base_name(base)}.{name}", mt)
            rows.append(None)  # placeholder, filled below in order
        # combos: sequential application of k single mutations (fresh seed => unseen)
        for k in range(2, 6):
            for ci in range(8):
                picks = rng.sample(singles, k)
                cur = text
                for nm2, mt2 in picks:
                    a, b = text.splitlines(), mt2.splitlines()
                    try:
                        i = next(j for j in range(min(len(a), len(b))) if a[j] != b[j])
                    except StopIteration:
                        continue
                    cl = cur.splitlines()
                    if i < len(cl):
                        cl[i] = b[i]
                        cur = "\n".join(cl) + ("\n" if mt2.endswith("\n") else "")
                write_variant(outdir, f"{base_name(base)}.combo{k}_{ci}s{seed}", cur)
    # (variants written; oracle pass below, walking the dir so order is deterministic)
    paths = sorted(f"{outdir}/variants/{f}" for f in os.listdir(f"{outdir}/variants")
                   if f.endswith(".mwf"))
    for p in paths:
        vn = os.path.basename(p)[:-4]
        exp = f"{outdir}/expected/{vn}.mwx"
        rc, d, msg = oracle(p, exp)
        print(f"{vn}: rc={rc} bytes={len(d)}", flush=True)
        rows.append({"variant": vn, "path": p, "expected": exp if d else None,
                     "sha256": hashlib.sha256(d).hexdigest() if d else "", "rc": rc,
                     "stdout": "", "mwxc_out": msg, "holdout": True})
    with open(f"{outdir}/manifest.jsonl", "w") as fh:
        for r in rows:
            if r:
                fh.write(json.dumps(r) + "\n")
    print(f"generated: {sum(1 for r in rows if r)} entries -> {outdir}/manifest.jsonl")


def write_variant(outdir, vn, text):
    safe = vn.replace("/", "_").replace(" ", "_").replace(",", "-").replace("+", "p").replace("-", "m").replace("=", "")
    open(f"{outdir}/variants/{safe}.mwf", "w", encoding="cp1252").write(text)


def base_name(path):
    return os.path.basename(path)[:-4]


def judge(outdir):
    stats = {"pass": 0, "fail": 0, "reject_ok": 0, "reject_fail": 0, "skip": 0}
    fails = []
    for line in open(f"{outdir}/manifest.jsonl"):
        e = json.loads(line)
        if not e["expected"]:
            r, ours = run_pktxc(e["path"], f"{SC}/out.mwx")
            refused = (r.returncode != 0 or not ours)
            stats["reject_ok" if refused else "reject_fail"] += 1
            if not refused:
                fails.append(("ORACLE-REJECT-PASSED", e["variant"], f"rc=0 (oracle msg: {e['mwxc_out'][:80]})"))
            continue
        exp = payload(e["expected"])
        r, ours = run_pktxc(e["path"], f"{SC}/out.mwx")
        if ours == exp:
            stats["pass"] += 1
            continue
        stats["fail"] += 1
        runs, idx = diffruns(exp, ours)
        desc = "; ".join(f"@{rr[0]:04x} exp {exp[rr[0]:rr[-1]+1].hex()} ours {ours[rr[0]:rr[-1]+1].hex()}" for rr in runs[:5])
        fails.append(("BYTE-EQUAL-FAIL" if len(exp) == len(ours) else "LENGTH-FAIL",
                      e["variant"], desc + f" | len exp={len(exp)} ours={len(ours)} ndiff={len(idx)}"))
    for kind, name, why in fails[:30]:
        print(f"{kind} {name}: {why}")
    print("STATS", json.dumps(stats), "total:", sum(stats.values()))
    return 0 if stats["fail"] == 0 and stats["reject_fail"] == 0 else 1


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gen":
        bases = sys.argv[4:] or [f"{DLL_DIR}/{n}.mwf" for n in ("01", "02", "03", "04")]
        # captured payloads are re-extracted from the oracle dir (kept read-only there)
        for n in ("01", "02", "03", "04"):
            src, dst = f"{DLL_DIR}/out{n}.mwx", f"{REPO}/fixtures/captured/{n}.mwx"
            if os.path.exists(src):
                os.makedirs(f"{REPO}/fixtures/captured", exist_ok=True)
                open(dst, "wb").write(open(src, "rb").read())
        sys.exit(0 if gen(sys.argv[2], int(sys.argv[3]), bases) is None else 1)
    if cmd == "run" or cmd == "score":
        sys.exit(judge(sys.argv[2]))
    print(__doc__)
