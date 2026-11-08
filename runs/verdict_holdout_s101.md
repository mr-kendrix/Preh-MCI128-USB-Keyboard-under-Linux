# Holdout judge verdict — seed 101 (runs/holdout_s101)

Gate: `tools/holdout_loop.py` (strict stop rule: pktxc output must be BYTE-EQUAL **and** SAME LENGTH
as the wine/oracle payload; no reference-container patching allowed).

```
python3 tools/holdout_loop.py gen runs/holdout_s101 101 <oracle_dir>/0{1..4}.mwf
python3 tools/holdout_loop.py run runs/holdout_s101      # -> STATS
python3 tools/holdout_classes.py runs/holdout_s101 runs/holdout_s101/classes.txt
```

## Counts

| bucket | count |
|---|---|
| total gate cases | 456 |
| PASS (byte+length equal) | **0** |
| FAIL | 392 |
| oracle-reject AND pktxc-reject (OK) | 64 |
| oracle-reject but pktxc accepted | 0 |

Holdout cases = 4 captured vendor payloads (01–04, oracle byte-identical to USBPcap wire)
+ 452 unseen variants (single + 2..5-combo header/KEYPRINT/KEYLAYER mutations, seed 101;
the fitting loop used seed 7 — no variant name collides, combos carry the `s101` suffix).

**Stop rule NOT satisfied: 0/392 comparable cases match.** pktxc is still a
reference-container patcher (`MWX_REF = fixtures/uploads/00_pos_mci128.mwx`, pktxc.cpp:11),
which the stop rule forbids.

## Classes (clean run: 392 fails, 21 classes — `runs/holdout_s101/classes.txt`)

| count | len Δ (exp−ours) | verified first diffs | reading |
|---|---|---|---|
| 160 | −87 (`captured.02`) | `0x104…` exp `00 00 …` ours `05 75 05 7c …`, `0x11a` exp `000000000000` ours `05 b4 05 bb 05 c2` | 02-layout has 14 **empty POSKey slots** → index table needs `0000` entries; record region shorter by 87 B |
| 80 | −87 (`captured.03`) | same table shift + `0x003` exp `75` ours `c9` | 03's RATE line is `2,1000,VERY` → byte `0x75`; pktxc kept the reference's `FAST` = `0xc9` + same keyset problem |
| 68 | −3 (`captured.01`) | `0x138` exp `00` ours `24`; `0x141`,`0x149`,`0x151`… exp `01` ours `00` (stride 8) | every key entry carries **byte1 = 0x01 active flag**; global byte `0x138` must be `00` for 01 |
| 22 / 11 / 11 | −92, −8 (`02.MSRLRC`, `01.MSRLRC`, `03.MSRLRC`) | `0x003` re-offset, `0x005` exp `3d` ours `42` … | the **MSR line changes body length** and shifts the whole index/record region |
| 24 | +1477…+1388 (0 bytes out) | no byte diffs | pktxc **refuses valid vendor input** (`LANGUAGE FR189` → `error: LANGUAGE unknown`); the oracle compiles all 24 |
| 3 + 2 + 2 | −3 (`01.MODEmEC/pEC/pSE`) | `0x132` exp `34`/`30`/`38` vs ours `30`/`34`/`38` | MODE **B-byte polarity inverted** for `EC`/`SE` |
| 1 + 2 | −3 / −87 (`01.LANGUAGEGR`, `02.LANGUAGEGR`) | `0x000` exp `08` ours `00` / exp `18` ours `10` | byte0 carries LANGUAGE code **plus MODE bits** (01 `GR,S`→`08`, 02 `FR,C`→`20`, 03/04 `SP,C`→`70`; `02` +`-FFA` → `18`); pktxc never patches byte0 |

Headline for the next lane: single-byte settings classes (~35 cases) are individually fixable,
but ~350 cases need the **record emitter**: u16 offset table (empty slots = `0000`) +
variable-length 7–9 B key records built from KEYPRINT/KEYLAYER lines, with per-layout key counts
and MSR-driven body-length deltas. pktxc's `pos_ids()` regex + reference copy cannot express that.
