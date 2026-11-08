#!/usr/bin/env python3
"""gapB_dump.py — annotated node dump of the oracle stream for a pos_mci128a_* fixture.
Usage: gapB_dump.py <mwx-oracle-out> <MWF-path>
Prints per region (index-table start): slot, cap/token name, then each node:
  offset  code  mid4  trailer  ptr(absolute) -> target slot/node
"""
import re, sys

b = open(sys.argv[1], "rb").read()
b = b[4:] if b[:4] == b"Preh" else b
mwf = open(sys.argv[2], encoding="cp1252", errors="replace").read()

tbl = [int.from_bytes(b[4 + 2 * k:6 + 2 * k], "big") for k in range(142)]
starts = sorted(v for v in tbl if v)
ends = starts[1:] + [len(b)]
start_of = {s: e for s, e in zip(starts, ends)}

refs = {}
order = []
for l in mwf.splitlines():
    m = re.match(r'^([A-Z0-9]{2,4})(-L)?\+([A-Z/]*):\s*"\{(.*)\}"', l)
    if m:
        refs.setdefault(m[1], []).append((m[4], bool(m[2]), m[3]))
        order.append((m[1], m[4], bool(m[2])))

def slotof(cap):
    if len(cap) == 3 and cap[0] in "ABCDEFGH" and cap[1:].isdigit():
        return (ord(cap[0]) - 65) * 16 + int(cap[1:])
    return None

slot2name = {}
for cap, uses in refs.items():
    sl = slotof(cap)
    if sl:
        slot2name[sl] = uses[0][0]

EXT = {0xE0, 0xE1, 0xE2, 0xE4}

def consume(s, e, mode):
    res = []
    o = s
    while o < e:
        c1, c2 = b[o], b[o + 1]
        if c1 == 0x1D and c2 == 0x68:
            L = 3
        else:
            L = 2
            q = o + 2
            while q + 1 < e and b[q] in EXT:
                if mode == "lazy" and q + 4 <= e and b[q + 2] == 0x00:
                    break
                L += 2
                q += 2
            if b[o + 1] in EXT:
                L = 3
                q2 = o + 3
                while q2 + 1 < e and b[q2] in EXT:
                    L += 2
                    q2 += 2
        if o + L + 5 > e:
            return None
        res.append((o, L, b[o + L:o + L + 4], b[o + L + 4]))
        o += L + 5
    return res

def node_offs(ts):
    r = None
    for mode in ("greedy", "lazy"):
        r = consume(ts, start_of[ts], mode)
        if r:
            break
    return [x[0] for x in r] if r else []

offs_by_region = {s: node_offs(s) for s in start_of}

def resolve(ptr):
    for s, offs in offs_by_region.items():
        if ptr in offs:
            k = offs.index(ptr)
            return s, k
    for s, e in zip(starts, ends):
        if s <= ptr < e:
            return s, None
    return None, None

print(f"table starts: {[(tbl.index(s)+1, s) for s in starts][:8]} ... n={len(starts)}")
for s, e in zip(starts, ends):
    sl = tbl.index(s) + 1
    nm = slot2name.get(sl, f"<slot{sl}>")
    offs = offs_by_region[s]
    r = None
    mode = "?"
    for mode in ("greedy", "lazy"):
        r = consume(s, e, mode)
        if r:
            break
    print(f"--- slot {sl} name={nm} region [{s:04x},{e:04x}) bytes={b[s:e].hex()} nodes={len(r) if r else 'PARSEFAIL'}")
    if not r:
        continue
    for i, (o, L, mid, tr) in enumerate(r):
        p = None
        if mid[0] == 0 and (mid[1] or mid[2]):
            p = int.from_bytes(mid[1:3], "big")
        ts, ti = (None, None)
        if p is not None:
            ts, ti = resolve(p)
        tname = slot2name.get(ts, f"<slot{ts}>") if ts else None
        print(f"  n{i} @{o:04x} code={b[o:o+L].hex()} mid={mid.hex()} x={mid[3]:#04x} tr={tr:#04x} ptr={'-' if p is None else f'{p:04d} -> slot {ts} node {ti} ({tname})'}")
