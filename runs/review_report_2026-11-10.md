# pktm review report — consolidated for Claude, 2026-11-10

All commits pushed to `origin/main` (ssh mr-kendrix/Preh-MCI128-USB-Keyboard-under-Linux):
`9faa2e9` macro executor · `bbf40bf` {DELAY} refuse · `dd17328` sweep marker · `e89d5c4` hard-fail socket + test-hook warning + nanosleep · `e0c35f2` SlowOutput + calib chk stamp · `93732a1` probe chain kdotool · `79106f7` AutoRepeat.
(`7c14090` interim fallback was superseded by `e89d5c4` hard-fail per review.)

## DONE — each row proven by a real run

| Area | Proof |
|---|---|
| Macro executor `value_script` | matrix dry-injected: BS→14, TAB→15, ESC→1, N5→6, Scroll→70, Caps→58, Num→69, Enter→28, Space→57, Shift→42, Ctrl→29, Alt→56, arrows→103/105; `{CTRL+w}` → `29:1 17:1 17:0 29:0`; user PHYSICAL test typed `5`, tab-indent, backspace, newline, `^[[A` ↑ |
| `{DELAY}` | REFUSED "not modelled", never dropped — `{ENTER}{DELAY}{ENTER}` → `REFUSED`; `{DELAY1000}` also refused |
| AutoRepeat | MEASURED on the board (`hold_record.py`: 2.3 s hold = exactly one down + one `0xFE` release, no board-side repeats) → impl = down at press, up deferred until release (`ar_pending`), OS/XKB typematic (250/15 ms) provides repeats; batch test printed `AutoRepeat press` + `release ... up sent` |
| SlowOutput | 20 ms/char via `ydot_send(.., gap_us)`; measured **20.0 ms** gap at the uinput node |
| Exit sweep | headless kill-test: `PKT_HOLD_MS=3000` + SIGTERM@0.6 s → marker `SWEEP_UP 0006` (signal-safe raw write + ::send up-event over `ydot_sweep_fd()`, `_Exit`; no heap/stdio) |
| Socket env | explicit `YDOTOOL_SOCKET` + missing file → `error:` + exit 2 (no silent override, review r4); default only when unset; `>> TEST HOOKS ACTIVE` startup line; `nanosleep` for ≥1 s holds |
| Calib drift guard | `_board_chk` stamp in calib JSON (scancode parser skips `_`-keys); live start reads board chk via EF 0E (`board_checksum16`, `device_present` guard) → `WARN live D2B6 != stamp ABCD` on stale; verified both paths vs the real board |
| Wayland focus source | CLOSED: `kdotool` (KWin D-Bus scripting, v0.3.0 via cargo, symlink `~/.local/bin/kdotool` in PATH incl. QProcess); `getactivewindow getwindowname` → `~ : fish — Konsole`; pktm chain `kdotool→xdotool→<unknown-window>` verified end-to-end: dep-`konsole` pmap → `rule match → injected` natively; ~ms/call |
| hidraw reader | reopen-spam bounded (5 open+read failures → one errno hint, clean sweep exit); replug renames the node → pktd/pktm must be restarted (pitfall recorded); stuck-key incident from kill-test recovered via raw up-events into `/tmp/.ydotool_socket` |
| Gate | `tools/live_gate.py` GREEN incl. uinput down/up arrival (keys 6,1,15); single RED reproduces as event-delivery timing, re-run GREEN |

## OPEN — verdict remains `stay` (do not install pktm.service at boot yet)

1. `--calibrate` relearn after board re-bind (manual calib patch + pktd restart works today).
2. Reader-lock parity vs pktd flock — pktd stays running untouched as rollback; new daemon must refuse to start while pktd is active.
3. Mixed literal+macro stays REFUSED — vendor behaviour settles via a Windows keylogger recording; same recording settles `{DELAY}` wait.
4. `pktm.service` is a template only (placeholders, `--seconds 0`); boot enable = human decision.
5. Final user 30 s trial with everything on: stop pktd → `--live` 120 s → press 124/77/96/31/29 + one window-dep key + unplug/replug → `cat /tmp/sweep.mark` after Ctrl-C → start pktd.

## Round-8: --calibrate CLOSED (1790ecc)

- New mode `pktm <pmap> --calibrate --calib OUT.json [--feed-hex | --device NODE]`: prompt-guided relearn — press POSKey 1..128 (or feed for tests); first-seen down per scan wins; writes JSON + `_board_chk` from the live board checksum (EF 0E, guarded by `device_present()`).
- Round-trip proven: written JSON re-read by a `--live` run — scans decode to the right POSKeys (`e040->POS1`, `0005->POS3`), stamp matches (no WARN), key format %04x 2-byte / %06x 3-byte like the vendor calib file.
- Pitfall recorded: the 1496 B keytable-region dump does NOT fit a fixed stride (75/127 only — E0xx keys carry 9-byte `1d 68 e0` records); relearn goes through the hidraw event loop, not the dump.

## Question for the next review

`kdotool` 0.3.0 prints `Unsupported KDE version` on stderr while stdout is correct (works on this Plasma 6 build). pktm parses stdout and ignores stderr. Benign contract, or should a non-empty stderr flip the window to `<unknown-window>` as fail-safe?

## Repro (headless)

```bash
cd ~/hermes-workspace/pkt-editor-cpp && cmake -B b2 -S . && cmake --build b2 --target pktm
python3 tools/live_gate.py b/pktm          # GATE: GREEN
./b/pktm /tmp/delay.pmap --dry-inject 51   # DELAY refused
./b/pktm /tmp/kw.pmap --live --feed-hex '04 e0 40 00 00' --calib /home/nonrmy/MK.calib.json  # konsole-dep -> injected
PKT_SWEEP_LOG=/tmp/sweep.mark PKT_HOLD_MS=3000 ./b/pktm "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/MappingLists/Mr.Kendrix.pmap" --live --feed-hex 04e0400000 --calib /home/nonrmy/MK.calib.json
# SIGTERM ~0.6 s in -> /tmp/sweep.mark: "SWEEP_UP 0006"
YDOTOOL_SOCKET=/tmp/.does-not-exist ./b/pktm ...  # error: + rc=2
```
