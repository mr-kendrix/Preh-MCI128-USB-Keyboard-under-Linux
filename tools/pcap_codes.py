#!/usr/bin/env python3
"""USBPcap -> wire code stream: every SET_REPORT control packet carries [04 code].
Header bytes per OUT transfer (from capture): 21 09 04 02 01 00 02 00 | code_hi code_lo"""
import re, sys

HDR = b'\x21\x09\x04\x02\x01\x00\x02\x00'

def codes(fn):
    d = open(fn, 'rb').read()
    out = []
    for m in re.finditer(re.escape(HDR), d):
        o = m.end()
        if o + 1 < len(d):
            out.append(d[o + 1])   # payload = [04 report-id, code] -> keep code
    return out

if __name__ == "__main__":
    for fn in sys.argv[1:]:
        c = codes(fn)
        print(fn, "->", len(c), "codes:", " ".join(f"{x:02x}" for x in c[:12]))
