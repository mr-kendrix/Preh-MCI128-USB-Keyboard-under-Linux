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
- Gate test: `./b/pktest <pmap> <mwx>` → `maps=32 name=MCI 128 frames=182`
- Gate compiler: `./b/pktxc fixtures/uploads/pos_mci128.MWF out.mwx` → `byte-equal-fixture=True` (exit 0);
  C++ port of `pkt-linux-editor/src/pktxc.py` semantics (MWF POS ids, header-derived store bytes
  0x0004/0x0135/0x0136, SOUND quad 0x0124, RATE 0x0007) — training pair `fixtures/uploads/` is self-contained
