# Parity: pktd.py vs pktm (daemon switch checklist)

| Feature | pktd.py | pktm (948f762) | Action |
|---|---|---|---|
| HID IN-report decode (0x04 → scancodes) | ✔ | ✔ (`decode_report`, port) | — |
| calib JSON scancode→POSKey | ✔ | ✔ (QJsonDocument, same file) | — |
| inject ydotool (24-byte input_event DGRAM + EV_SYN) | ✔ | ✔ (proven vs live ydotoold recv) | — |
| rules: POSKey→Value/Text, cp1252-hex | ✔ | ✔ (`unhex_cp1252`) | — |
| window/`<Dependency>` matching | ✔ x11/file/cmd | ✔ (xdotool if present; `<unknown-window>` = unconditional only) | fragile: KDE Wayland ⇒ **not covered** ⇒ keep pktd.py until a KWin focus source is added |
| pick_layer / `<Value>` layer selection | ✔ | ✔ | — |
| `calibrate` (learn live, next-key-prompt) | ✔ | ✘ | covered by existing MK.calib.json (learned via pktd) — pktm needs no relearn, but no relearn path |
| `watch` / `raw` / `--dump-unknown` | ✔ | ✘ | diagnostics only; pktd remains for those |
| auto-repeat (`<AutoRepeat>`) | ✔ | ✘ | open |
| macros `{TAB}/{ESC}/{N5}` | partial | **open**: known-macros (ESC/TAB/DELAY/Nx, case-insens.) → Text-fallback; UNKNOWN macro → REFUSED (no silent strip) | full macro engine (mixed macro+literal → key+char sequence) open |
| window/`<Dependency>` rule | ✔ x11/file/cmd | **process-name via getwindowpid→/proc/comm added**; title-contains one-directional; xdotool per press, X11/XWayland only | open: KDE-Wayland focus source |
| unplug/replug | ✔ | ✔ (fresh open per reader) | — |
| exit sweep | ✔? | **SIGINT/SIGTERM handler → ydot_release_all() (held-keys)** | 30s trial with mid-seq kill open |
| relearn (`calibrate`) | ✔ | ✘ | open — pktm --calibrate missing |
| held-set typematic-repeat kill | ✔ | ✔ (held set) | — |
| hold_device_lock / other-readers detection | ✔ | ✘ | hidraw single-consumer risk unmanaged in pktm |
| umlauts/unicode text→evdev seq | ✔ (`expand_tokens`) | ✔ (`CHARMAP` + refuse on unmappable) | — |
| unplug/replug (reopen device on ENOENT) | ✔ | ✔ (device path re-resolved per open) — verify | open: verify once |
| release-all on exit (no stuck modifiers) | ✔ | ✘ (no atexit sweep) | open |

Verdict: **stay** — pktd.py is the superset; pktm covers the core inject path with a headless gate, but focus/`AutoRepeat`/relearn/reader-lock/exit-sweep are missing. Switch only after those 5 rows close.
