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
5. Final user 30 s trial with everything on: stop pktd → `--live` 120 s → press 124/77/96/31/29 + one window-dep key + one replug **while holding** a key → `cat /tmp/sweep.mark` after Ctrl-C → start pktd. This round ADDS: hold POSKey 124 (an AutoRepeat rule) ~40 s without releasing → expect `-- AutoRepeat release (max-hold 30 s)` in the live log and NO key-flood in the editor; and the kill recovery documented in Round 9 §4 (`systemctl restart ydotoold.service` if a `kill -9` cleanup ever leaves a key down).

## Round-8: --calibrate CLOSED (1790ecc)

- New mode `pktm <pmap> --calibrate --calib OUT.json [--feed-hex | --device NODE]`: prompt-guided relearn — press POSKey 1..128 (or feed for tests); first-seen down per scan wins; writes JSON + `_board_chk` from the live board checksum (EF 0E, guarded by `device_present()`).
- Round-trip proven: written JSON re-read by a `--live` run — scans decode to the right POSKeys (`e040->POS1`, `0005->POS3`), stamp matches (no WARN), key format %04x 2-byte / %06x 3-byte like the vendor calib file.
- Pitfall recorded: the 1496 B keytable-region dump does NOT fit a fixed stride (75/127 only — E0xx keys carry 9-byte `1d 68 e0` records); relearn goes through the hidraw event loop, not the dump.

## Question for the next review

`kdotool` 0.3.0 prints `Unsupported KDE version` on stderr while stdout is correct (works on this Plasma 6 build). pktm parses stdout and ignores stderr. Benign contract, or should a non-empty stderr flip the window to `<unknown-window>` as fail-safe?

## Round 9: reviewer round — three fixes + probe hardening (this session)

Reviewer verdict received: `stay` (correct); fix socket leak, stale fd, AutoRepeat safety
BEFORE any long-running/boot use. All three implemented + probe hardening:

1. **ydottool socket leak (confirmed, fixed):** every `ydot_send` opened a fresh
   `AF_UNIX` socket and orphaned the previous fd — 10k keypresses = 10k leaked fds.
   Now ONE connected socket per lifetime: reuse `g_ydot_connected`, reconnect only
   after failure/path change, close old fd, and `g_ydot_connected = -1` BEFORE
   `::close()` so a signal can never `send()` on a closed/reused descriptor
   (`fail_close`). Proof: 4 injection pairs with `PKT_HOLD_MS` → `/proc/<pid>/fd`
   shows exactly ONE socket inode (`socket:[52997237]`) across calls 1–4.
2. **AutoRepeat safety net (added):** max-open-hold 30 s (`PKT_MAX_HOLD_S` overrides
   for tests) + flush of pending AR downs whenever the read path dies
   (replug/lost `0xFE`). The event loop now `ppoll()`s 250 ms per round — a HELD key
   sends no reports, so a blocking read would starve the watchdog (glibc trap:
   plain `poll()` in `<sys/poll.h>` is the legacy SVID variant with swapped args
   and ms timeout — `::poll(&pfd,1,250000)` hung 250 s; POSIX `ppoll` fixed it).
   Proof headless: loop cadence ~4 polls/s while idle; `hidraw gone` flush prints
   and releases (`up sent`); SIGTERM mid-hold still `SWEEP_UP 0006` (re-run green).
   Board-side max-hold release = user trial (poll on hidraw = hidapi precedent;
   poll on FIFOs is a known Linux dead path, harness-only).
3. **kdotool probe hardened:** a probe counts only with exit status 0 (fail-closed
   to `<unknown-window>` otherwise — covers kdotool's own "Unsupported KDE version"
   path on future Plasma bumps), PID must be plain digits before any `/proc` path,
   title = first line of stdout, stderr messages logged ONCE per distinct line
   (probe runs per press — no log flood). Process-name-first: rules name the
   PROCESS (from the PID); title-only matching stays degraded-mode. Test run on
   this box: `proc 'vivaldi-bin'` resolved natively. Tested versions: **kdotool
   v0.3.0 @ plasma-desktop 6.8.2** (on 6.8.2 stderr is now silent; the
   'Unsupported KDE version' warning was seen on the older Plasma build).
4. **Stuck-key incident explained:** the flood of `5`s was the CLEANUP kill —
   leftover test daemons are stopped with `kill -9` (exact PID; the pkill variant
   matched its own wrapper) and `SIGKILL` can never run the sweep handler, so
   ydotoold kept key 6 down. The `SIGTERM`/Ctrl-C path DOES release — proven again
   this round (`SWEEP_UP 0006`). Recovery if it ever happens again:
   `systemctl restart ydotoold.service` (or inject one raw up-record per held key
   into `/tmp/.ydotool_socket`). Kept in the report as the documented recovery.

Gate after all fixes: GREEN (124/95/77 injected, event-node down/up balanced).

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
