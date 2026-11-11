# kdotool upstream — issue draft (2026-11-11)

**Title: `KDE_SESSION_VERSION` check rejects valid Plasma 6 environments`

`main.rs:615`:

```rust
if std::env::var("KDE_SESSION_VERSION") != Ok("6".to_string()) {
    return Err(anyhow!("Unsupported KDE version. kdotool only supports KDE Plasma 6."));
}
```

Problems:
1. Under a Wayland session, terminal/service processes do NOT inherit
   `KDE_SESSION_VERSION` (only GUI children of the session get it) → kdotool refuses
   in exactly the environments where it is most useful (systemd user services,
   tmux/TTY launched daemons).
2. It rejects future versions (7.x) as "unsupported" even with feature detection.

Suggestion: treat an UNSET value as "unknown, try anyway" (the D-Bus call itself
is the real capability check — if there is no KWin scripting endpoint, the call
fails non-zero), and only reject known-incompatible majors (5.x). i.e.

```rust
let v = std::env::var("KDE_SESSION_VERSION").unwrap_or("");
if v == "5" || v.starts_with("5.") { return Err(...); }
```

Affected use case here: pktm's live window probe (POS-key daemon) calls
`kdotool getactivewindow …` per press from a systemd environment; workaround
currently = the caller injects `KDE_SESSION_VERSION=6` into the QProcess env.

Environment: Arch, plasma-desktop 6.8.2, kdotool v0.3.0.
