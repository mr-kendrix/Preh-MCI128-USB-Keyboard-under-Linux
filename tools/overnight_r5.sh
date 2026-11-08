#!/bin/bash
# overnight round 5: empty-layout class — variant_struct 'empty_13': exp 318 B, ours 322 B,
# diff at 0x03 (42 vs 46): MSR header/gsz length when the layout has NO assignments.
cd /home/nonrmy/hermes-workspace/pkt-editor-cpp
S=/tmp/pktloop5.log
: > "$S"
FITJ="/tmp/var01b /tmp/varhold101 /tmp/varstruct /tmp/varsealed2 /tmp/varsealed4242 /tmp/varsealed8888"
for i in 1 2 3 4 5 6; do
  echo "=== r5.$i ===" >> "$S"
  ANY=""
  for j in $FITJ; do
    R=$(timeout 600 python3 tools/judge.py "$j" 2>/dev/null | grep -E '^(DIFF|AGREE-FAIL)' | head -6)
    echo "$R" >> "$S"
    [ -n "$R" ] && ANY=1
    timeout 600 python3 tools/judge.py "$j" 2>/dev/null | tail -1 >> "$S"
  done
  if [ -z "$ANY" ]; then echo "FIT CLEAN -> sealed check" >> "$S"; break; fi
  timeout 1800 hermes -z "pktxc: empty-layout class — .mwf WITHOUT any assignment line: oracle body 318 B (0x03=42), ours 322 (46). The 4-byte key-count/gsz field at 0x03 must account for zero records + MSR header gsz with no -L flags (0x13c consumer absent -> gsz 29?). Probe: strip all assignment lines from pos_mci128.MWF copy, run_oracle.sh, dump header bytes; implement rule (no hardcode), rebuild cmake --build b, re-judge varstruct+varsealed*, commit on improvement. One line reply." --in /home/nonrmy/hermes-workspace/pkt-editor-cpp >> "$S" 2>&1
done
echo "=== sealed score seed 1234 base pos_mci96 template structural ===" >> "$S"
timeout 1200 python3 tools/variant_struct.py "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates/pos_mci96.MWF" /tmp/varstruct96 1234 >> "$S" 2>&1
timeout 600 python3 tools/judge.py /tmp/varstruct96 >> "$S" 2>&1
echo "ROUND5 DONE" >> "$S"
