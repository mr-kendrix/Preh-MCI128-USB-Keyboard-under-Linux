#!/bin/bash
# overnight round 4: 0x13c tail = MSR text-form rule (gsz 34 -> flag consumer exists;
# empty text -> 00). 4242 classes are FIT now; new sealed seed 8888 on pos_mci96 template.
cd /home/nonrmy/hermes-workspace/pkt-editor-cpp
S=/tmp/pktloop4.log
: > "$S"
FITJ="/tmp/var01b /tmp/varhold101 /tmp/varstruct /tmp/varsealed2 /tmp/varsealed4242"
for i in 1 2 3 4 5 6; do
  echo "=== r4.$i ===" >> "$S"
  ANY=""
  for j in $FITJ; do
    R=$(timeout 600 python3 tools/judge.py "$j" 2>/dev/null | grep -E '^(DIFF|AGREE-FAIL)' | head -6)
    echo "$R" >> "$S"
    [ -n "$R" ] && ANY=1
    timeout 600 python3 tools/judge.py "$j" 2>/dev/null | tail -1 >> "$S"
  done
  if [ -z "$ANY" ]; then echo "FIT CLEAN -> sealed check" >> "$S"; break; fi
  timeout 1800 hermes -z "pktxc 0x13c tail rule: stream offset 0x13c = the -L flag CONSUMER; it exists iff the MSR line carries TEXT (gsz 34 form incl. ': \"\"'), else 00. sealed4242 shows exp 01 (text-form) where ours 00 (over-trimmed by 9aa93b4), sealed2 shows exp 00 (empty text). Implement the gsz-dependent rule in pktxc.cpp (no variant hardcode), rebuild, re-judge varsealed2+varsealed4242+var01b, commit only on improvement. One line reply." --in /home/nonrmy/hermes-workspace/pkt-editor-cpp >> "$S" 2>&1
done
echo "=== sealed score seed 8888 base pos_mci96 template ===" >> "$S"
timeout 1500 python3 tools/variant_gen.py "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/LayoutTemplates/pos_mci96.MWF" /tmp/varsealed8888 8888 >> "$S" 2>&1
timeout 300 python3 tools/judge.py /tmp/varsealed8888 >> "$S" 2>&1
echo "ROUND4 DONE" >> "$S"
