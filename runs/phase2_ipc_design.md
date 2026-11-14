# Phase 2 — GUI<->daemon IPC: one JSON object per line over a Unix socket

Transport: `AF_UNIX` stream socket at `$XDG_RUNTIME_DIR/derpktxc/daemon.sock`
(dir created 0700; GUI = only peer). Line-delimited JSON (NDJSON), no framing
bytes, no shell anywhere. Reader side = existing IN-pump path (same 5-byte
reports may still arrive from the board).

Every message: `{"t":<type>, ...}` — `t` is the only required key, others
type-specific. Unknown `t` -> ignored + one `warn` back. Unknown keys -> ignored.

## Message types

### from daemon -> GUI (status feed)
| t | payload | notes |
|---|---------|-------|
| `press` | `poskey:int`, `down:bool`, `ms:int` | one board keypress observed |
| `inject` | `poskey:int`, `seq:[[ev,down],...]`, `kind:"text"\|"macro"\|"action-blocked"` | what rule fired; action only with --allow-actions |
| `window` | `title:string`, `proc:string`, `ms:int` | active-window probe result (was GUI-only; now shared) |
| `health` | `polls:int`, `heartbeat:int`, `board_chk:string`, `warn:[...]` | calibration/health snapshot for the panel |
| `reload` | `path:string`, `rules:int` | pmap changed on disk -> rules reloaded (GUI re-renders rows) |
| `error` | `where:string`, `why:string` | parse/load/socket failure — never kills the stream |

### from GUI -> daemon (control, all OPTIONAL — daemon works without the socket)
| t | payload | notes |
|---|---------|-------|
| `set-value` | `layer:int`, `text:string` | edits textarea -> daemon swaps TEXT layer value (atomic write+rename) |
| `set-dep` | `layer:int`, `proc:string` | picker committed |
| `probe-window` | (none) | starts the 3 s countdown + active-window probe |
| `reload` | (none) | force pmap reread |
| `quit` | (none) | GUI closing; daemon flushes and exits |

## Rules
- Socket is additive: without it, every existing gate behaves 1:1 (verified
  by --dry-inject + --processes paths staying byte-identical).
- Actions keep the daemon-side guard: `--allow-actions` OFF => `inject` carries
  `kind:"action-blocked"`, nothing forked.
- Undo safety unchanged: writes go through UndoSnap(pre,post) check on the FILE,
  never through the socket.
- `ms` fields = CLOCK_MONOTONIC ms since daemon start.

## Still to build (Phase 2 code, after design is reviewed)
1. daemon side: parse stdin lines? NO — read from `$XDG_RUNTIME_DIR/derpktxc/daemon.sock`;
   select()-loop beside the IN-poll; write only to GUI peer fd.
2. GUI side: QXxxSocketClient? plain ::connect+write on button clicks; repaint
   panel from `press/window/health` instead of the private QTimer heartbeat.
3. Test: offscreen GUI --clicktest grows a `--ipc` mode: fake socketpair peer,
   feed canned NDJSON, assert panel state changes per message type.
