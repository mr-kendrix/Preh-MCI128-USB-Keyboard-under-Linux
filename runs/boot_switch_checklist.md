# pktm — Boot-Switch Checklist (one page, 2026-11-11)

State now: `pktd.service` (system, python) = rollback, ACTIVE. `pktm` verified
(headless + board trials r9–r15), running only by hand. Switch = YOUR decision
after pktm ran for days.

## A. Trial phase (repeat daily before any boot switch)
1. `sudo systemctl stop pktd.service`   (system unit — needs sudo)
2. `pktm --live "<PMAP>" --calib <CALIB>` (by hand; log visible)
3. press the everyday set + one window-rule key (FOCUS an editor/terminal first —
   the injection goes to the FOCUSED window, chat focus = chat flood, see r13)
4. replug while holding a key; Ctrl-C mid-press (sweep: marker/`SWEEP_UP`)
5. `sudo systemctl start pktd.service` — board normal again
   stuck-key recovery if a kill -9 left a key down: `systemctl restart ydotoold.service`

## B. Enable pktm at boot — only when ALL hold
- [ ] pktm ran N days, no unexplained log lines
- [ ] `ydotoold.service` active BEFORE pktm starts (socket order — missing socket = rc=2 loop)
- [ ] unit installed as USER unit ~/.config/systemd/user/pktm.service from runs/pktm.service
      with <HOME>/<PMAP>/<CALIB> filled; WantedBy=graphical-session.target (window probe!)
- [ ] PartOf=graphical-session.target → pktm stops when the session stops (no orphan daemon)
- [ ] `systemctl --user enable pktm.service` THEN `sudo systemctl disable pktd.service`
      (ONE reader — pktm refuses while pktd.py inject lives; both enabled = one quits)
- [ ] TimeoutStopSec=10 + Restart=on-failure in place (sweep before SIGKILL, mappings back)

## C. Rollback (always available, one command each way)
- back to pktd: `kill <pktm-pid>; sudo systemctl start pktd.service`
- sanity: `journalctl --user -u pktm.service -b | tail`, `ls -l /dev/input/by-id/*hidraw`
- single-consumer reminder: while pktm runs, ONLY POSKeys work (hidraw node owned);
  normal keys are unaffected (they go via kernel uinput, r10 gate proves down/up)

## D. Known findings recorded in review_report (do not re-investigate)
- kdotool needs KDE_SESSION_VERSION → pktm injects it for the QProcess (r15); upstream
  issue draft in runs/kdotool_issue_draft.md
- 0/128 rules in Mr.Kendrix.pmap REFUSED → mixed literal+macro not blocking (r15)
- exit-sweep marker clean incl. hot-unplug flush (r13), AutoRepeat max-hold 30 s works
