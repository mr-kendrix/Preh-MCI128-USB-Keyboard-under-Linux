#!/usr/bin/env python3
import sys
t = open("/tmp/true_payload_01.bin", "rb").read()
c = open("/tmp/pktxc_01.mwx", "rb").read()
if c[:4] == b"Preh":
    c = c[4:]
diffs = [i for i in range(min(len(t), len(c))) if t[i] != c[i]]
print("n:", len(diffs))
# group into runs
runs = []
for i in diffs:
    if runs and i == runs[-1][-1] + 1:
        runs[-1].append(i)
    else:
        runs.append([i])
for r in runs:
    a, b = r[0], r[-1]
    print(f"{a:04x}-{b:04x}: true={t[a:b+1].hex()} pktxc={c[a:b+1].hex()}")
