# pkteditor — Linux replacement for PrehKeyTec MapMyKey (pure C++)

Editor for `.pmap` mapping lists of the PrehKeyTec **MCI-3100** (MCI-128 compatible),
with native report-4 upload — no Wine, no Python, one binary.

Build (Arch): `cmake -B b -S . && cmake --build b` → `./b/pkteditor Mr.Kendrix.pmap`
Install (AUR): see `PKGBUILD` — `makepkg -si` gives `/usr/bin/pkteditor`.

- Qt6 Widgets, libusb; C++20; MIT
- Gate test: `./b/pktest <pmap> <mwx>` → `maps=32 name=MCI 128 frames=182`
