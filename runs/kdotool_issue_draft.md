# kdotool upstream — issue draft (corrected per review, 2026-11-11)

**Title: Version check rejects environments without KDE_SESSION_VERSION (systemd user services, TTY/SSH sessions)**

The same check occurs twice: `src/main.rs:615` AND `src/lib.rs:68` (library path
gets the same limitation):

```rust
if std::env::var("KDE_SESSION_VERSION") != Ok("6".to_string()) {
    return Err(anyhow!("Unsupported KDE version. kdotool only supports KDE Plasma 6."));
}
```

Precise claim: terminals INSIDE your Plasma session do inherit the variable and work.
What does NOT have it are processes started outside the session environment — systemd
user services, TTY/login-shell tools, programs launching kdotool as subprocess (QProcess).
In exactly those, kdotool refuses on a perfectly fine Plasma 6 desktop.

Minimal reproduction (Plasma 6.8.2, kdotool v0.3.0):

```
$ env -u KDE_SESSION_VERSION kdotool getactivewindow getwindowname
Error: Unsupported KDE version. kdotool only supports KDE Plasma 6.
$ KDE_SESSION_VERSION=6 kdotool getactivewindow getwindowname
* Untitled — Kate        # neutral example; any window title
```

Proposal (maintainers decide; the Plasma-6-only restriction itself is deliberate):
treat an UNSET value as "unknown, try anyway" — the D-Bus call is the real capability
check and fails non-zero without KWin scripting — and keep rejecting only known-
incompatible majors (5.x):

```rust
let v = std::env::var("KDE_SESSION_VERSION").unwrap_or_default();
if v == "5" || v.starts_with("5.") { return Err(anyhow!("...")); }
```

Affected use case: pktm's live window probe calls `kdotool getactivewindow …` per
press from a systemd/QProcess environment; workaround today = caller injects
KDE_SESSION_VERSION=6 into the child env.

Environment: Arch, plasma-desktop 6.8.2, kdotool v0.3.0, Wayland.
