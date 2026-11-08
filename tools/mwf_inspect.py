#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re
p = "/run/media/nonrmy/MULTIBOOT/Wireshark/01.mwf"
t = open(p, encoding="cp1252", errors="replace").read()
ls = [l for l in t.splitlines() if "KEYPRINT" in l]
for l in ls[:4]:
    print(repr(l[:200]))
print("kp count:", len(ls))
ls2 = [l for l in t.splitlines() if "KEYLAYER" in l]
for l in ls2[:3]:
    print(repr(l[:200]))
print("kl count:", len(ls2))
# a sample key with POS line:
for l in t.splitlines():
    if "POS\\line25" in l or "POS\\line26" in l:
        print(repr(l[:240]))
        break
