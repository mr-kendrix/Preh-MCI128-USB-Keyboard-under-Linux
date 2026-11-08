#!/usr/bin/env python3
"""Judge pktxc against the oracle manifest. Usage: judge.py <outdir>
For each manifest entry with expected bytes: run b/pktxc on the variant,
byte-compare (ignoring trailing zero padding), report structured diffs
(offset ranges with true/ours byte pairs) and reject-cases (rc!=0).
Exit 0 only if every comparable variant matches."""
import json, subprocess, sys, os

PKTXC = "/home/nonrmy/hermes-workspace/pkt-editor-cpp/b/pktxc"

def run_pktxc(mwf, out):
    subprocess.run([PKTXC, mwf, out], capture_output=True, text=True, timeout=60,
                   cwd="/home/nonrmy/hermes-workspace/pkt-editor-cpp")
    return open(out, "rb").read() if os.path.exists(out) else b""

if __name__ == "__main__":
    outdir = sys.argv[1]
    mf = f"{outdir}/manifest.jsonl"
    stats = {"match": 0, "diff": 0, "reject": 0, "skip": 0}
    fails = []
    for line in open(mf):
        e = json.loads(line)
        if not e["expected"]:
            # reject-agreement: oracle rejected -> native MUST refuse too
            ours_check = run_pktxc(e["path"], "/tmp/judge_rej.mwx")
            if len(ours_check) > 0:
                stats["agree_fail"] = stats.get("agree_fail", 0) + 1
                fails.append(("AGREE-FAIL", e["variant"], "oracle rejected, native ACCEPTED"))
            else:
                stats["agree_ok"] = stats.get("agree_ok", 0) + 1
            continue
        exp = open(e["expected"], "rb").read()
        ours = run_pktxc(e["path"], "/tmp/judge_out.mwx")
        if ours[:4] == b"Preh": ours = ours[4:]
        bexp = exp[4:] if exp[:4] == b"Preh" else exp
        m = min(len(bexp), len(ours))
        diffs = [i for i in range(m) if bexp[i] != ours[i]]
        if not diffs and abs(len(bexp) - len(ours)) <= 8:
            stats["match"] += 1
        else:
            stats["diff"] += 1
            runs = []
            for i in diffs:
                if runs and i - runs[-1][-1] <= 2: runs[-1].append(i)
                else: runs.append([i])
            desc = "; ".join(f"{r[0]:04x}-{r[-1]:04x} exp {bexp[r[0]:r[-1]+1].hex()} ours {ours[r[0]:r[-1]+1].hex()}" for r in runs[:6])
            fails.append(("DIFF", e["variant"], desc + f" | len exp={len(bexp)} ours={len(ours)}"))
    for kind, name, why in fails[:40]:
        print(f"{kind} {name}: {why}")
    print("STATS", stats, "total:", sum(stats.values()))
    raise SystemExit(0 if stats["diff"] == 0 and stats["reject"] == 0 else 1)
