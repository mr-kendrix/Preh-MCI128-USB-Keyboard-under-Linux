#!/usr/bin/env python3
"""Variant generator + oracle batch runner for pktxc RE.
Usage: variant_gen.py <base.mwf> <outdir> [--seed N]
Generates single-feature and random-combination .mwf variants, runs the
wine oracle (skips cleanly if wine/DLL absent), stores expected bytes +
rejection log. Manifest JSON lines: {variant, base, muts, expected_sha, rc, err}
"""
import hashlib, json, os, random, re, subprocess, sys

from_oracle = "/home/nonrmy/hermes-workspace/oracle_files/run_oracle.sh"
DLL_DIR = "/home/nonrmy/hermes-workspace/oracle_dir"

HEAD_KEYS = ("LANGUAGE", "MSR", "MODE", "SOUND", "RATE", "MEMORY", "VERSION")

def mutate_lines(text):
    """Yield (name, new_text) mutations of header lines + key-layer flags."""
    lines = text.splitlines(keepends=True)
    idx = {}
    for i, l in enumerate(lines):
        k = l.split(" ", 1)[0].strip()
        if k in HEAD_KEYS:
            idx.setdefault(k, i)
    def repl(key, val):
        if key not in idx: return None
        i = idx[key][0] if isinstance(idx[key], list) else idx[key]
        out = lines[:]
        nl = (key + " " + val).strip() + "\n"
        out[i] = nl
        return "".join(out)
    # LANGUAGE single features
    for lang in ("US", "GR", "FR189", "SP", "SG", "SF", "IT", "UK"):
        for sfx in ("", ",S"):
            yield (f"LANGUAGE={lang}{sfx}", repl("LANGUAGE", lang + sfx))
    # MODE single bit flips
    for tok in ("+T1", "-T1", "+T2", "-T2", "+T3", "-T3", "+MSR", "-MSR",
                "+KL", "-KL", "+KVK", "-KVK", "+BCR", "-BCR", "+FCR", "-FCR",
                "+BA0", "+BA1", "+BA2", "-EF", "+EF", "+IEK", "-IEK", "-FFA",
                "+OPOS", "-OPOS", "+SE", "-SE", "-EC", "+EC"):
        yield (f"MODE={tok}", repl("MODE", tok))
    # SOUND numbers
    for a in ("0", "50", "100"):
        for b in ("0", "10", "20"):
            yield (f"SOUND={a},{b},{a},{b}", repl("SOUND", f"{a},{b},{a},{b}"))
    # RATE token
    for r in ("FAST", "VERY SLOW", "SLOW", "MEDIUM"):
        yield (f"RATE={r}", repl("RATE", r))
    # MSR token
    for m in ("NSE", "LRC", "M3", "NA1", "N1", ""):
        yield (f"MSR={m}", repl("MSR", m))
    # KEYLAYER / KEYPRINT edits: layer type codes (0.00 fields), enabled flags
    kp = [i for i, l in enumerate(lines) if "!@KEYPRINT" in l]
    kl = [i for i, l in enumerate(lines) if "!@KEYLAYER" in l]
    for i in kp[:10]:
        l = lines[i]
        parts = l.split(";")
        if len(parts) > 4:
            p = parts[:]; p[1] = "1.00"  # x coord
            p[4] = "1"
            nl = ";".join(p)
            out = lines[:]; out[i] = nl
            yield (f"KP@{i}", "".join(out))
    for i in kl:
        l = lines[i]
        m = re.match(r"!@KEYLAYER:([^,]*),([0-9A-Fa-f]*),(\d+)", l)
        if m and m.group(2):
            out = lines[:]
            out[i] = l.replace("," + m.group(2) + ",", ",0000,")
            yield (f"KL@{i}", "".join(out))

def run_oracle(path, outname):
    r = subprocess.run(["bash", from_oracle, DLL_DIR, path, outname],
                       capture_output=True, text=True, timeout=60)
    return r

if __name__ == "__main__":
    base, outdir = sys.argv[1], sys.argv[2]
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 7
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(outdir + "/variants", exist_ok=True)
    os.makedirs(outdir + "/expected", exist_ok=True)
    text = open(base, encoding="cp1252", errors="replace").read()
    rng = random.Random(seed)
    names = []
    for name, mt in mutate_lines(text):
        if mt is None: continue
        vn = f"{os.path.basename(base)[:-4]}.{name}".replace("/", "_").replace(" ", "_").replace(",", "-").replace("+", "p").replace("-", "m").replace("=", "")
        p = f"{outdir}/variants/{vn}.mwf"
        open(p, "w", encoding="cp1252").write(mt)
        names.append((vn, p))
    # random combinations 2..5
    singles = [(n, mt) for n, mt in mutate_lines(text) if mt]
    for k in range(2, 6):
        for ci in range(8):
            picks = rng.sample(singles, k)
            mt = text
            applied = []
            for n2, mt2 in picks:
                mt = mt2 if mt == text else mt  # naive: last wins per-key (good enough for mapping)
            # apply sequentially properly:
            mt = text
            for n2, mt2 in picks:
                # mt2 is full text from base; apply mutation directly onto mt by line surgery:
                pass
            # sequential single application:
            cur = text
            ok = True
            for n2, mt2 in picks:
                # mt2 differs from text at exactly one header/layer line; find it
                a, b = text.splitlines(), mt2.splitlines()
                diffs = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
                if not diffs: continue
                i = diffs[0]
                cl = cur.splitlines()
                if i < len(cl):
                    cl[i] = b[i]
                    cur = "\n".join(cl) + ("\n" if mt2.endswith("\n") else "")
            vn = f"{os.path.basename(base)[:-4]}.combo{k}_{ci}"
            p = f"{outdir}/variants/{vn}.mwf"
            open(p, "w", encoding="cp1252").write(cur)
            names.append((vn, p))
    # oracle run
    mani = open(f"{outdir}/manifest.jsonl", "w"); import functools
    for vn, p in names:
        exp = f"{outdir}/expected/{vn}.mwx"
        try:
            r = run_oracle(p, exp)
            rc = r.returncode
            err = (r.stdout + r.stderr)
        except Exception as e:
            rc, err = 99, str(e)
        sha = ""
        if os.path.exists(exp) and os.path.getsize(exp) > 0:
            sha = hashlib.sha256(open(exp, "rb").read()).hexdigest()
        mw = f"{DLL_DIR}/mwxc.out"
        msg = open(mw, encoding="cp1252", errors="replace").read() if os.path.exists(mw) else ""
        mani.write(json.dumps({"variant": vn, "path": p, "expected": exp if sha else None,
                               "sha256": sha, "rc": rc, "stdout": err[:300],
                               "mwxc_out": msg[:300]}) + "\n"); mani.flush()
        print(f"{vn}: rc={rc} bytes={os.path.getsize(exp) if os.path.exists(exp) else 0}")
    print("total variants:", len(names))
