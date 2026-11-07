# pkteditor — Linux replacement for PrehKeyTec MapMyKey (pure C++)

Editor for `.pmap` mapping lists of the PrehKeyTec **MCI-3100** (MCI-128 compatible),
with native report-4 upload — no Wine, no Python, one binary.

Build (Arch): `cmake -B b -S . && cmake --build b` → `./b/pkteditor Mr.Kendrix.pmap`
Install (AUR): see `PKGBUILD` — `makepkg -si` gives `/usr/bin/pkteditor`.

- Qt6 Widgets, libusb; C++20; MIT
- GUI (LB4): one `QWidget` + one QPainter paint routine (`paint_grid`) draws the physical grid —
  rows A..H, `ROWS_N` keycaps per row, POSKey slot numbering (A=1..16 … H=113..128, checked against
  `Mr.Kendrix.pmap`). Clicking a cap selects its Map; the sidebar edits its `<LAYER>`s
  (`+ Layer` / `Set` / `Layer lösch`); `Speich` writes through `serialize_pmap()` and re-parses.
- GUI gates (headless, no display needed):
  `./b/pkteditor <pmap> --smoke` → `rows=8 caps=110 slots=128 name=MCI 128 maps=32`;
  `--cells H12,A1 --set MACRO --save out.pmap` (or raw board coords `--cells "px:32,32;H12"` — `;`
  separates when `px:` keeps its own comma) → per-cell layer report + `STABLE=yes` reload line;
  `--shot out.png` → the identical paint routine rendered by Qt's offscreen plugin.
- Serializer (LB4): `serialize_pmap()` writes vendor tags (`<POSKeyMapper><MAPPING><LAYOUT>`,
  mixed-case `<POSKey>/<Type>/<Value>/<Name>`) and hex-cp1252 POSKeys, so save→reload round-trips.
  The old form wrote `<POSKEY>` as raw 16-bit hex, which its own parser read back as POSKey 0.
- Upload (LB5): `upload.h/cpp` = the loop — `compile_frames()` → one `device_send()` per code,
  driven by a `QProgressDialog` (Cancel aborts, `LIBUSB_ERROR_*` mapped to human reasons in the
  status line). Mountable in one line (`UploadPanel upload(&win, &pm->pm, mwx, up_arm);`), already
  inside `gui_main.cpp`; `transport.h` adds `device_present()` (probe without writing),
  `device_last_status()` and `transfer_error_message()`.
  Gate: `./b/pktup <pmap> [mwx] [--upload] [--arm-upload] [--cancel-at N] [--map-table] [--fail-text] [--shot f.png]`
  → `payload=1480 frames=182 first=04 armed=0 board=1`; unarmed run says
  `Upload NOT ARMED — 182 frames compiled, nothing sent`, cancel run says
  `upload cancelled at frame 8/182 — nothing was sent`. The board is **only** written with
  `--arm-upload` (or `PKT_UPLOAD_ARM=1`), and `EF 12` (finalising) is never sent.
- Endpoint probe fix (LB5): `find_control_ep` walked only interface 0's altsettings, so it never
  saw iface 1 and always returned -1; it now walks all `bNumInterfaces` and falls back to the
  HID-class descriptor the kernel hands back (`0x82` = report type 2 = OUT), verified on the
  plugged MCI 3100 (`b/probe_if` → `iface=1 nEP=1 ep=0x82 attr=0x03`).`
- Gate compiler: `./b/pktxc fixtures/uploads/pos_mci128.MWF out.mwx` → `byte-equal-fixture=True` (exit 0);
  C++ port of `pkt-linux-editor/src/pktxc.py` semantics (MWF POS ids, header-derived store bytes
  0x0004/0x0135/0x0136, SOUND quad 0x0124, RATE 0x0007) — training pair `fixtures/uploads/` is self-contained

## Install (Arch/CachyOS)

```bash
curl -O https://raw.githubusercontent.com/mr-kendrix/Preh-MCI128-USB-Keyboard-under-Linux/main/PKGBUILD
makepkg -si            # → /usr/bin/pkteditor
```

## Prior art & legal

- PrehKeyTec official Linux-Utility: „on request“ only, no public package.
- `cevou/preh-keyboard` (hidapi): POSKey-Reader only — no editor/upload.
- Own RE implementation, MIT; no vendor binaries, no Windows-toolchain circumvention.
