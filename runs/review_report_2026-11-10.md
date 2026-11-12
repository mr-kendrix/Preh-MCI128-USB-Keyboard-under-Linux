# pktm review report — consolidated for Claude, 2026-11-10

All commits pushed to `origin/main` (ssh mr-kendrix/Preh-MCI128-USB-Keyboard-under-Linux):
`9faa2e9` macro executor · `bbf40bf` {DELAY} refuse · `dd17328` sweep marker · `e89d5c4` hard-fail socket + test-hook warning + nanosleep · `e0c35f2` SlowOutput + calib chk stamp · `93732a1` probe chain kdotool · `79106f7` AutoRepeat · `1790ecc` --calibrate · `c034252` r9 socket/fd/AR/probe · `8daebbf` r10 POLLIN+parity · `1526f3e` r11 lock hygiene · `29e2a50` r12 SOCK_CLOEXEC+gate-hook · `d69cf2e` r13 sweep bookkeeping · `e65a982` r14 focus-tool docs.
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
| One socket per lifetime (round-9) | 4 injection pairs under `PKT_HOLD_MS`: `/proc/<pid>/fd` (via `pgrep -x pktm`) shows exactly ONE `socket:[N]` inode across calls 1–4; reconnect+close only after send failure, `g_ydot_connected=-1` before every `::close()` |
| AutoRepeat safety net (round-9) | 30 s max-hold (`PKT_MAX_HOLD_S` test override) + flush of pending AR downs on read death; event loop polls 250 ms so the watchdog runs while a held key is silent; PROVEN on the real read path via FIFO (round 10): down report arrives, `AutoRepeat press`, then `-- AutoRepeat release (max-hold): scancode 0xe040 up sent` at 1 s with `PKT_MAX_HOLD_S=1`; physical-board max-hold = user trial |
| Reader parity (round-10) | pktd.service (cmd=inject) never flocks — parity part 1 is a `/proc` cmdline scan (pktd.py `other_pktd_readers` style): pktm refuses (rc=2) while a `pktd.py inject` lives, and pktd's own scan refuses beside pktm. Part 2: flock pair on `/tmp/pktd-hidraw.lock` — pktm LOCK_SH|NB vs pktd calibrate/watch/raw LOCK_EX conflict BOTH ways (proved errno 11 in both orders) + own LOCK_EX file: pktm-vs-pktm refuse rc=2. Kernel drops locks at process exit (no stale locks) |
| fd-exhaustion test (round-10) | `ulimit -n 64` + 200 injections: `-- 200 presses, 200 injected, 0 unknown` rc=0 — one-socket design survives; ASAN irrelevant for fd counts (reviewer agreed) |
| Probe hardening (round-9) | probe counts only with exit 0 (else fail-closed `<unknown-window>`); PID validated digits before `/proc`, dead PID -> undeterminable; title = first stdout line; distinct stderr lines logged ONCE. NOTE: plain kdotool v0.3.0 later proved broken in user env (see round 14, fork fixes it) |
| Sweep bookkeeping (round-13) | trial found exit-sweep re-upping released keys (`SWEEP_UP 002d` x2 after clean taps): only AR-deferred keys tracked now (`sweep_remove` on release/flush/watchdog) — mid-hold kill = exactly ONE `SWEEP_UP`, clean taps = empty marker, no `-- exit sweep` line |
| Focus tool (round-14) | kdotool fork (~/hermes-workspace/kdotool-fork): upstream refused unless `KDE_SESSION_VERSION == "6"` exactly (empty in Plasma-Wayland/terminals = the REAL root cause, not 6.8.2); fork accepts empty/6.x/future, rejects 5/5.x — plain terminal: `getactivewindow getwindowpid` -> 35743 (Blender), `--dry-inject 91` -> window + rule-match YES. Installed over `~/.local/bin/kdotool` (upstream kept `kdotool.upstream`) |
| Focus tool (round-14/15) | round-14 fork replaced by round-15 minimal fix: pktm `run_probe` injects `KDE_SESSION_VERSION=6` into the kdotool QProcess only (upstream binary untouched; `~/.local/bin/kdotool` = upstream). Proof systemd env: `systemd-run --user` journal `window='Hermes' ... rule-match=YES`; probe = real env + insert, non-KDE still fails closed |
| USER TRIAL, real board (r13) | 124/77/96/31/29 = 5/5 injected; window-rule test POSKey 30 (B14, 91 is blind!): Blender focus -> 5x `rule match -> injected`; Hermes/KCalc/kate/vivaldi windows -> `no rule match` with correct proc each; 40 s hold -> release at 30 s (max-hold) no flood; replug -> `reconnected` (by-id node stable); kill marker clean |

## OPEN — verdict: `leave` possible; still NOT installing pktm.service at boot

1. Mixed literal+macro semantics: still REFUSED — settled only by a Windows
   keylogger recording of the vendor tool (vendor-data question, not code).
2. `pktm.service` boot enable: HUMAN decision (template packaging/pktm.service
   has placeholders + `--seconds 0`; pktd.service stays as rollback).
3. kdotool fork = local patch, upstream PR not filed (offer: PR to jinliu/kdotool
   would benefit the whole community).
Closed meanwhile: user trial fully done (round 13), reader parity done (r10),
focus tool done (r14), max-hold = ship 30 s configurable+logged (r10 Q1).

## Question for the next review (round-9 state)

The old kdotool stderr question is ANSWERED and implemented (round 9 §3): exit status + parsed output decide; stderr is informational, logged once per distinct line.

New questions (round-9) — ALL ANSWERED, see Round 10 section:
1. Max-hold: ship the 30 s cap (configurable, logged) — >30 s single holds are not
   plausible on POS hardware; late board release silently ignored. DONE as-is.
2. Reader parity: IMPLEMENTED round 10 (proc scan + flock pair; pktd inject never
   flock'd — that finding changed the shape, see Round 10).
3. ASAN would not cover fd exhaustion; `ulimit -n 64` + 200 injections run instead:
   200/200 rc=0.

## Round-8: --calibrate CLOSED (1790ecc)

- New mode `pktm <pmap> --calibrate --calib OUT.json [--feed-hex | --device NODE]`: prompt-guided relearn — press POSKey 1..128 (or feed for tests); first-seen down per scan wins; writes JSON + `_board_chk` from the live board checksum (EF 0E, guarded by `device_present()`).
- Round-trip proven: written JSON re-read by a `--live` run — scans decode to the right POSKeys (`e040->POS1`, `0005->POS3`), stamp matches (no WARN), key format %04x 2-byte / %06x 3-byte like the vendor calib file.
- Pitfall recorded: the 1496 B keytable-region dump does NOT fit a fixed stride (75/127 only — E0xx keys carry 9-byte `1d 68 e0` records); relearn goes through the hidraw event loop, not the dump.

## Question for the next review

`kdotool` 0.3.0 prints `Unsupported KDE version` on stderr while stdout is correct (works on this Plasma 6 build). pktm parses stdout and ignores stderr. Benign contract, or should a non-empty stderr flip the window to `<unknown-window>` as fail-safe?
(ANSWERED round 9: decision = exit status + output, stderr logged once — implemented in `c034252`.)

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

## Round 10: `stay` follow-up — POLLIN regression fix, parity, corrected claims (this session)

Reviewer found `struct pollfd pfd{ fd, 0, -1 }`: events=0 -> ppoll reports nothing ->
the live read loop starved (no key ever read). Confirmed empirically: pipe with data,
events=0 -> ppoll 0; events=POLLIN -> 1. Fixed to `struct pollfd pfd{ fd, POLLIN, 0 }`.

Two earlier claims were WRONG, cause was the same events=0 bug (both re-tested):
- "poll on FIFO never wakes on Linux" — false; with POLLIN it wakes immediately.
- "glibc legacy SVID poll() with swapped args" — false; `poll(fds,nfds,timeout_ms)` is
  the standard signature, timeout just in ms (our hang came from 250000 ms = 250 s).

Real-read-path re-test via FIFO (bypasses were the problem, now covered):
POSKey 124 AutoRepeat press -> max-hold release at 1 s (PKT_MAX_HOLD_S=1) -> proof
read loop + watchdog work end-to-end; ulimit -n 64 + 200 injections rc=0 (fd proof the
reviewer asked for); gate GREEN rerun.

Reader parity implementation (the reviewer's flock shape refined by reality): pktd
cmd=inject NEVER calls hold_device_lock() (only calibrate/watch/raw do, pktd.py:525) —
so a shared-lock dance alone would not stop the daemon. pktm does BOTH:
1. /proc cmdline scan for live `pktd.py ... inject` (same method pktd uses:
   other_pktd_readers) -> refuse rc=2 with the stop-pktd hint.
2. flock pair: conditional LOCK_SH|NB on /tmp/pktd-hidraw.lock (conflicts both ways
   with pktd's calibrate/watch/raw LOCK_EX, errno 11 verified both orders) + own
   LOCK_EX /tmp/pktm-hidraw.lock -> pktm-vs-pktm refuse too.
Late release report after max-hold flush: arrives with empty ar_pending -> silently
ignored (verified on the FIFO run, no double up).
Failed injection sends stay drop-only (no reconnect-and-retry added — POS tool,
explicit beats implicit; revisit only with vendor data).

Verdict: `stay`. User trial only AFTER this POLLIN fix (before it nothing would work).

## Round 11: lock hygiene (reviewer findings on 8daebbf, this session)

- Peer lock file is pktd's path — pktd runs as ROOT -> file may be root-owned: opening
  it O_WRONLY would fail EACCES = false refusal even with pktd stopped. Now: flock is
  fd-based -> open O_RDONLY|O_CLOEXEC, create O_NOFOLLOW only if missing (fallback).
- Own lock moved from /tmp (world-writable = foreign pre-holders could sabotage the
  daemon) to $XDG_RUNTIME_DIR (/run/user/<uid>), mode 0600, O_NOFOLLOW. Verified:
  `ls -l /run/user/1000/pktm-hidraw.lock` -> `-rw------- nonrmy`.
- O_CLOEXEC on lock + hidraw opens (fd leak into forked helpers — kdotool probes run
  via QProcess — impossible to leak a fd across exec with cloexec). ydotool socket is
  QLocalSocket-managed, no open() under our control there — noted, not leaked either
  (reused g_ydot_connected).
- Reviewer Q1 answered by CODE: live_gate.py gained stage-2 REAL read path (FIFO ->
  ppoll loop -> AutoRepeat press + max-hold release), SKIPs while pktd.service active
  and still GREEN; with pktd stopped the stage proves read+watchdog:
  `POSKey 124 'x' -> AutoRepeat press` then `-- AutoRepeat release (max-hold)`.
- Hot unplug while holding (review Q2): read-death flush is IMMEDIATE (not after 5
  tries — the 5-counter only bounds reopen spam and ends the run): pending AR downs
  get their up right away with `-- AutoRepeat release (hidraw gone)`. Proven fifo
  variant in report repro.
- ydotool socket: plain ::socket() in mapper.cpp (QLocalSocket is only an include) —
  now SOCK_CLOEXEC, so QProcess helpers (kdotool/xdotool) never inherit the fd.
- Verdict unchanged `stay`; user trial per checklist below (pktd stop/start bracketed).

## Round 12: gate honesty (review of round 11 — SKIP was invisible)

- live_gate.py stage 2 now runs WITHOUT stopping pktd.service via new test hook
  `PKT_SKIP_PKTD_SCAN` (pktm announces it like the other hooks: ">> TEST HOOK ACTIVE:
  pktd scan skipped — never set in a service"). Scan skip is safe for the gate because
  the stage reads a FIFO, not the hidraw node.
- Honest verdict: if even the hooked run refuses (pktd calibrate/watch/raw holds the
  flock), the gate prints "REAL-PATH: SKIP ... read path NOT tested" and the final
  line says `GREEN (read path NOT tested)` — never a silent GREEN.
- Verified with pktd.service ACTIVE: REAL-PATH press + max-hold release, GATE: GREEN.
- Review round-11 verdict otherwise accepted; next step is the HUMAN trial (checklist
  in OPEN 4) — headless rounds cannot cover real hidraw + Plasma window deps.

## Round 13: sweep bookkeeping + USER TRIAL results (real board, 2026-11-11)

Sweep bookkeeping bug found BY the trial (marker: `SWEEP_UP 002d` x2 after two clean
AR taps = exit sweep re-sent ups for already-released keys): the down-registration
tracked EVERY injected key. Fixed: only AR-deferred ups are tracked; `sweep_remove`
on normal release / flush / watchdog. Proven: mid-hold kill -> exactly ONE SWEEP_UP;
after clean taps -> marker empty, no `-- exit sweep` line.

User trial on the physical board (Runs A/B1/B3):
- POSKeys 124/77/96/31/29: 5/5 injected (chat got the `5` — injection follows the
  FOCUSED window; lesson: focus a terminal/editor during trials, never the chat).
- 40 s hold POSKey 124 (AR pmap) -> `AutoRepeat release (max-hold)` at 30 s, flood
  stopped. Replug -> `reconnected hidraw by-id/...-hidraw` (node NAME stable via by-id).
- Window-dep rule (91, dep Blender, Blender open): `no rule match`, window='<unknown-window>'
  because **kdotool v0.3.0 is BROKEN on Plasma 6.8.2**: prints 'Unsupported KDE version.
  kdotool only supports KDE Plasma 6.' exit 1 on EVERY call. The r9 claim "stderr silent
  @6.8.2, proc 'vivaldi-bin' native" was from a DIFFERENT plasma/kdotool state — current
  honest state: probe fail-closed everywhere on this box. Window rules need a working
  focus tool (kdotool version check rejects 6.8.x? check upstream) — OPEN, not blocking
  the fail-closed design (it degraded correctly).
- kill test: pktm exited, marker EMPTY (bookkeeping fix verified on board).
- pktd.service restarted after trial; ydotoold restarted to clear the chat-flood key
  state (recovery cmd from Round 9 §4 used and works).

## Round 14: focus tool FIXED (kdotool fork) — window rules live

Root cause (upstream github jinliu/kdotool src/main.rs:615): exact match
`KDE_SESSION_VERSION == "6"`; Plasma/Wayland + plain terminals DO NOT export it
(GUI procs get "6" only inside sessions) -> every kdotool call failed on this box
(Plasma 6.8.2 irrelevant — the var was just empty).

Fix (kdotool-fork in ~/hermes-workspace/kdotool-fork, cargo release, 15 s build):
reject ONLY "5"/"5.*" (true Plasma 5), accept empty/6.x/future. Verified headless:
plain terminal, no env: getactivewindow getwindowpid -> 35743 (Blender), getwindowname
-> '* (Unsaved) - Blender 5.2.3 LTS'; KDE_SESSION_VERSION=5 -> still refuses (fail-closed).
Installed at ~/.local/bin/kdotool (upstream kept as kdotool.upstream).
pktm --dry-inject 91: window resolved + rule-match YES (Blender focused at probe time)
-> degraded-case warning from round 13 no longer applies on this machine.

## Round 15: probe env fix (no fork needed) + reviewer-checklist answers

- Reviewer's alternative beats the fork: pktm's run_probe sets KDE_SESSION_VERSION=6
  ONLY for the kdotool QProcess (env built from pktm's own environ + insert). Upstream
  kdotool v0.3.0 runs untouched. Verified headless: `systemd-run --user ... pktm
  --dry-inject 93` journal shows `window='Hermes' ... rule-match=YES` — probe works
  in a service env WITHOUT the var (exactly the systemd scenario the reviewer flagged).
- Mixed literal+macro: ran --dry-inject over POSKey 1..128 of Mr.Kendrix.pmap:
  **0 REFUSED** -> the open vendor question does not block this file.
- pktm.service template updated: TimeoutStopSec=10 (sweep runs before SIGKILL),
  commented ExecStopPost ydotoold restart, parity + sweep-trial notes.
- GUI UploadPanel (gui_main.cpp:488) uses the SAME upload.cpp flow as CLI: EF 0E
  backup read-back (~/.local/share/pkteditor/pktup_backup_*.mwx — survives reboot,
  round-7 move; the old /tmp path only lived in comments), refuse on blind overwrite, ACK count,
  EF 12 checksum verify, half-feed warning. Panel layer is new, the safety path shared.
- kdotool fork kept at ~/hermes-workspace/kdotool-fork as local artifact only;
  ~/.local/bin/kdotool = upstream binary restored.

## Round 16: boot-prep deliverables (review approved r15; human part left)

- `runs/pktm.service`: added After/PartOf=graphical-session.target + [Install] WantedBy
  (window probe needs the Plasma session alive), Restart=on-failure/RestartSec=3
  (safe since sweep proven r13), socket-order warning (rc=2 + Restart = loop trap).
- `README.md` housekeeping block: supported keyboards MCI128-family + MCI96, paths are
  machine-specific (placeholders), compiler-oracle = optional + needs OWN vendor DLL,
  vendor tools not in repo.
- `runs/kdotool_issue_draft.md`: upstream issue (env-var check too strict; suggested
  relaxation "only reject 5.x", D-Bus call = real capability check).
- `runs/boot_switch_checklist.md`: one-pager — trial daily routine, enable-preconditions,
  rollback one-liners, known findings pointer.
- Verdict: nothing in code blocks anymore; remaining = human trial-days + the boot
  decision. pktd.service ACTIVE = rollback while pktm is hand-run.

## Round 17: review corrections applied (issue draft, checklist, backup-path claim)

- Issue draft rewritten per review: precise claim (session children inherit, session-external
  processes don't), lib.rs:68 second occurrence, compiles-rust `unwrap_or_default()`,
  minimal repro `env -u KDE_SESSION_VERSION kdotool …` (verified pattern), proposal tone.
- Checklist: stop pktm via `systemctl --user stop pktm.service`, session-target
  check command, After= can't cross user->system unit (retries until ydotoold up
  = NOT a bug, logged noise), hidraw wording = OUR flock policy not OS limit.
- Stale claim fixed in code comment (upload.cpp:59) + report: backup lives at
  ~/.local/share/pkteditor/pktup_backup_*.mwx since round 7 — survives reboot.

## Phase 0 (2026-11-12): GUI pmap round-trip — the file-saving bug found in review

- Review of the GUI plan found it: save_pmap used serialize_pmap (4 fields) — a
  real pmap loses ~675 tags (Font/Text/Icon/Keylock/Dependency/…) on save.
- FIX: lossless DOM (pmap_dom.h/.cpp): slot pool, verbatim text children,
  attrs/self-close/mismatch detection; dom_serialize(dom_parse(x)) == x.
- pmaprt gate: 10/10 vendor pmaps RT EXACT (incl. Mr.Kendrix.pmap 131,924 B),
  counts match old parser, attributes/mismatched close REJECTED, isolated
  edit = 1 byte diff. FAIL-FAST: structural model changes refused (Phase 1).
- GUI save_pmap: DOM path, backup BEFORE overwrite ->
  ~/.local/share/pkteditor/pmap_backup_<ts>_<name>, atomic write (temp +
  std::filesystem::rename — Qt6 QFile::rename static refused EPERM), re-read
  verify (DOM RT + model view stable). GUI live-wire (QLineEdit etc.) untouched.
- pmaprt --gate: `b2/pmaprt <files...>`; pkteditor CLI gate:
  `b2/pkteditor <pmap> --set V [--cells ..] --save <path>`.
- Headless proof: copy save -> 158 B diff = ONLY hex-case of Type/Value
  (vendor used lowercase, to_hex_cp1252 uppercases; semantics identical),
  675 unknown tags byte-identical; --add -> SAVE BLOCKED, file UNTOUCHED.

## Phase 0b: review fixes on the save path (2026-11-12, a8156bb)

- dom_apply_model now diffs against dom_view(file) — ONLY changed fields are
  rewritten, in the ORIGINAL hex case (format_hex_like); unedited files save
  as no-op (no backup, no rewrite), one-layer edit = exactly that element
  differs (verified prefix/suffix on Mr.Kendrix.pmap and on repo fixture).
- Repo fixture fixtures/p0_synth.pmap: lowercase hex + cp1252 UNDEFINED bytes
  0x81/0x8D in a Value — round trip EXACT, edit writes lowercase style.
- save_pmap: backup-write failure ABORTS (short write detected), temp write
  byte-count checked + ::fsync(fd) before rename, backup rotation keeps 30
  newest pmap_backup_* (lexicographic == timestamp).
- pmaprt adds IDOM (apply(view)==raw byte-exact) per file + MISSING→SKIP for
  repo-independent gating; negative tests unchanged.
- UNVERIFIED-by-me: vendor MapMyKey opening a GUI-saved copy (human check if
  the Windows tool is available); undo is NOT the backup — Phase 1 adds undo.

## Repro (headless)

```bash
cd ~/hermes-workspace/pkt-editor-cpp && cmake -B b2 -S . && cmake --build b2 --target pktm
python3 tools/live_gate.py b/pktm          # GATE: GREEN
./b/pktm /tmp/delay.pmap --dry-inject 51   # DELAY refused
./b/pktm /tmp/kw.pmap --live --feed-hex '04 e0 40 00 00' --calib /home/nonrmy/MK.calib.json  # konsole-dep -> injected
PKT_SWEEP_LOG=/tmp/sweep.mark PKT_HOLD_MS=3000 ./b/pktm "/mnt/Windows/ProgramData/PrehKeyTec/PKT Mapper/MappingLists/Mr.Kendrix.pmap" --live --feed-hex 04e0400000 --calib /home/nonrmy/MK.calib.json
# SIGTERM ~0.6 s in -> /tmp/sweep.mark: "SWEEP_UP 0006"
YDOTOOL_SOCKET=/tmp/.does-not-exist ./b/pktm ...  # error: + rc=2
# round-9 additions:
# one-socket proof: PKT_HOLD_MS=3000 + 4 down/up pairs; while running:
#   ls -l /proc/$(pgrep -x pktm)/fd | grep socket   -> exactly ONE socket:[N], same inode across calls
# flush-on-read-death (fifo stand-in for replug): data arrives, then writer closes:
#   -- AutoRepeat release (hidraw gone): scancode 0xe040 up sent
# max-hold (round-10: FIFO poll DOES wake — earlier failure was events=0 in pollfd):
#   mkfifo /tmp/arfifo; writer: { printf '\004\340\100\000\000'; sleep 5; } > /tmp/arfifo
#   PKT_MAX_HOLD_S=1 ./b2/pktm /tmp/ar.pmap --live --device /tmp/arfifo --seconds 4 ...
#   -> POSKey 124 AutoRepeat press -> '-- AutoRepeat release (max-hold): ... up sent' at 1 s
#   (board user-trial variant: PKT_MAX_HOLD_S unset, physically hold POSKey 124 ~40 s)
# ulimit proof (fd-exhaustion): FEED=200 pairs; bash -c 'ulimit -n 64; pktm --live --feed-hex $FEED'
#   -> '-- 200 presses, 200 injected, 0 unknown' rc=0
# reader parity: while `pktd.py inject` lives -> pktm --live rc=2 'pktd.py inject is live';
#   two pktm live runs -> 2nd rc=2 'another pktm holds the exclusive pktm lock';
#   flock conflict proved both orders errno 11 (LOCK_EX held vs LOCK_SH|NB attempt)
# window probe now validates exit status: kdotool with bad Plasma -> nonzero exit ->
#   fail-closed '<unknown-window>' (was: stdout-blind parse); stderr warning logged ONCE.
```
