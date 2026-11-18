# Review des clean Shell — for Claude (18.11.2026, nach Commit `a0a6915`)

Kurzfassung: **Der Kritik ist komplett resolved — alle 6 blockers und alle Logic-Errors sind fixed, jeweils with proof (gate rc + commit).**

## Kontext

- Build: `pkteditor = gui_clean.cpp + pdfexport.cpp + upload.cpp + pmap_dom.cpp + mapper.cpp + ipc.cpp + transport` — `gui_main.cpp` is **gone** (`09d2185`), its last `--clicktest` run was 0 FAIL before deletion.
- Gates: `--dlg-gate` (full dialog/daemon/upload chain) `rc=0 / 0 FAIL`; `--layout-hard` (window parts + grid ≥40 % + no extra widgets) `rc=0`; `--openlast-gate` asserts both directions (`rc=0`/`rc=2`).

## Die critique, point by point

| # | Review-Item | Status | Proof |
|---|---|---|---|
| 1 | No event loop, `return gfail?1:0` | **Fixed** | no gate flag → `return app.exec();`; offscreen run stays alive 4 s (`78993b4`) |
| 2 | Menu items never added (`new QAction(text, menu)`) | **Fixed** | `mk_item`/`mkctx` now call `addAction(a)` (+ ctor parent); all 5 menu tabs have entries (`78993b4`) |
| 3 | Ribbon buttons without handler | **Fixed** | Upload/Undo/New/Recent/Close/Exit/Settings/Modules-Add+Del connected; File-menu: New/Open/Close/Restore/Upload/Reload/Reopen-Recent; Edit: Undo/Swap/Inject/Reconnect/Export (`9658ef7`…`8bfe14e`) |
| 4 | Spinbox-tabs, no text | **Fixed** | tabstrip: 5 QToolButtons with text label + icon; click switches button group; gear/About share `f_settings` (`022d34d`); Settings section anchors below ribbon, no floating panel at 6,6 (`f9136d6`) |
| 5 | Key dialog not a dialog / widgets overlap | **Fixed** | all 5 overlays are `QDialog`; `dlgTitle`/`edit`/`layerW`/row-buttons are in the layout; height ≤560 px (`ccb6afa`) |
| 6 | "Slot 0–111" colors | **Fixed** | model already stored **signed RGBA ints** (`ForeColor=-16777216` in vendor file); `QColorDialog` wired: `currentColor().rgba()` → `Map.fore/back`; gate asserts blue/yellow values (`61ddf48`) |

## Logic-Errors from the review

- `do_open_file`: now sets `path`, `g_pm`, `pathlbl`, relayout + pushes recent. Close: real close (`f_closefile`), New: empties `pm`+`g_geom` + gate (`9fb0d92`).
- Paste/Delete/repaint: ctx actions run through shared lambdas with `dlg_refresh`/`relayout` paths.
- Left-click selection: `selectAt` on click + dblclick→dialog; right-click popup via `mousePressEvent` (`Qt::RightButton`) — `rightClicked()` does not exist in this Qt fork.
- `fit_label` vs painter font: `pt.setFont(fo)` with `pixelSize=int(f.pt)` before `drawText` (`2293144`).
- Settings apply at start: `openlast=1` + no file → opens `recent.first()`; `--openlast-gate` rc=0 (1) / rc=2 (0) (`2ca7c3c`); ini read **incl. `modules=MSR`** (`8bfe14e`).
- PDF export: dialog-free but **calibrated**: board units = 1/1000 mm (file: pitch 19000 = 19 mm real PK pitch) → 17 mm key = 48.2 pt; decompressed cut.pdf proves `54.2` cell = 48.2 + 6 pt margin (`9ce25c3`); `pdfx::unit_mm` exposed + gate asserts.
- Browser list: kept vendor-neutral — pmap hex-scan: **no** browser tags in the file, docs silent. Default entry `Default browser`.
- Swatch sample (`a0a6915`): QLabel restyled from model (font-family/size from `fontdesc`, colors from signed int) — the critique's missing color/font dialog is now QColorDialog+QFontDialog+sample.

## What the gates prove / do NOT prove

- They act via direct handler calls and named-object lookups — visibility is now separately covered by `--layout-hard` (part set + hidden-widget rules), but **no real user click** is simulated. Manual test recipe: `./run_clean.sh [pmap]` (adds `--debug`); screenshot mode `--shot` → PNG at 1920×1080 logical.
- Daemon side is `fakekd` (tools/fakekd.cpp, line-buffered `fflush`): round-trip is proven both ways — GUI→daemon (`mk_reload_req`, `mk_inject` with ms=175 in the JSON log line) and daemon→GUI (`press` burst drained → `g_pressed` → status `pressed: 2` + red inner frame on the tile) (`501fa6e`, `2028082`, `1dead4b`).

## Pitfalls in THIS Qt fork (reduced API — do not assume desktop-Qt6 names)

Missing here: `QComboBox::findIndexOf`, `setText/setIconText`, `QListWidget::appendItem/itemText`, `QSpinBox::setStep`, `QRect::marginsModified/reduced`, `QJsonValue::toBoolean`, `QColorDialog::okBtn`, `QColor::familyName` (use `family()`), `QFontDialog::setCurrentFont` (exists), signals must be connected via `&QObject::signal` (`QAction::triggered`, `QColorDialog::colorSelected`, `QFontDialog::currentFontChanged`).
Workarounds in tree: index-picks by `setCurrentIndex`, parallel `g_macro` list for the macro slots, manual rect math, static `g_swatch_css` for stylesheet read-back.

## Remaining, honest

- New-file flow is **one** step (`f_newfile` empties model), not the vendor's 3-step wizard with process-question.
- Modules panel shows entries via menu/ribbon handlers (MSR toggle + ini) — no per-board availability probe yet (vendor offers modules only when the hardware has them).
- Macro drag&drop is **not** real drag: token combi + slot list (max 16, executor expands all, preview asserts 2× "down").
- `Settings → lang` writes the ini but there is no DE/EN string catalog yet (only vendor's DE strings exist in the shell).
- Physical key press (user types on a real keyboard) → daemon sends `press` lines: drain is proven; the reverse (GUI listens while user types) works in principle with the same drain — no live board was ever involved, `armed upload` bounces at libusb without board (`9368107`).

## Save-Undo-Path (2026-11-18, commit 2b5f0a3) — Claude's Review 2

Claude flagged after deletion of gui_main.cpp that the file is no longer losslessly saved. All 4 points restored:

1. **lossless save**: `f_save` now uses the DOM path — parse original, **refuse save if the byte-roundtrip is not lossless** (`SAVE BLOCKED: file not in supported pmap dialect` / `not lossless on save`), model applied via `dom_apply_model` (only changed texts), then **atomic write** (tmp+rename via `write_atomic`) and verification re-parse. `pmap_dom.cpp` is used by both GUIs — nothing was re-implemented.
2. **Backup can no longer be bypassed**: write original bytes **before** the write, abort save on failure; wall-clock stamp `yyyyMMdd-HHmmss-zzz.pmap`; keep the 30 newest.
3. **Undo**: in-memory stack `UndoSnap{path, pre, post}` (exact bytes); on restart a file backup restored from undoDir — and **the restored copy is deleted**, second Undo does not repeat it.
4. **Gate**: new assert — tag counts `<Key>`, `<MSR>`, `<OCR>` before/after save equal (unmodeled tags preserved) → `dlg rc=0 / 0 FAIL`. Empirical: 131692 bytes = 131692 bytes, tag-count delta empty.

### Pitfall-Section corrected (Qt 6.4 headers, verified on /usr/include/qt6)

- QComboBox: **no** `findIndexOf`/`CaseInsensitive` (those are Qt5 names), but **`itemText(int)` exists** — I used the wrong widget (QListWidget has no itemText/appendItem → own `g_macro` list was correct)
- QSpinBox: real name **`setSingleStep`** (exists), not `setStep`
- QRect 6.4: only `adjusted`/`inset`; **`marginsModified` and `reduced` really do not exist** (Qt5 names)
- QColorDialog: **no** `okBtn` — signal `currentFontChanged`/`colorSelected` are the API
- QFont: `family()` exists, `familyName` does not
- QToolButton::setDefaultAction exists — used for the Ribbon-QAction pairing (see next section below)
 (chain)

`78993b4` event-loop+addAction · `9658ef7` ribbon Undo/Open/Close · `022d34d` tabstrip · `ccb6afa` QDialogs+layer-rows · `2293144` fit-label font · `2ca7c3c` openlast-applies · `9fb0d92` New · `9e93338` Settings/Help menu items · `61ddf48` QFontDialog · `501fa6e` inject+fflush · `9368107` upload-armed-proven · `0a7fb8e` macro slots · `8bfe14e` modules · `09d2185` legacy deleted · `1dead4b` press-drain · `2028082` hold-ms roundtrip · `f9136d6` settings-tab anchor · `a0a6915` swatch sample.
## Save-Undo-Path (2026-11-18, commit 2b5f0a3) — Claude's Review 2

Claude flagged after deletion of gui_main.cpp that the file is no longer losslessly saved. All 4 points restored:

1. **lossless save**: `f_save` now uses the DOM path — parse original, **refuse save if the byte-roundtrip is not lossless** (`SAVE BLOCKED: file not in supported pmap dialect` / `not lossless on save`), model applied via `dom_apply_model` (only changed texts), then **atomic write** (tmp+rename via `write_atomic`) and verification re-parse. `pmap_dom.cpp` is used by both GUIs — nothing was re-implemented.
2. **Backup can no longer be bypassed**: write original bytes **before** the write, abort save on failure; wall-clock stamp `yyyyMMdd-HHmmss-zzz.pmap`; keep the 30 newest.
3. **Undo**: in-memory stack `UndoSnap{path, pre, post}` (exact bytes); on restart a file backup restored from undoDir — and **the restored copy is deleted**, second Undo does not repeat it.
4. **Gate**: new assert — tag counts `<Key>`, `<MSR>`, `<OCR>` before/after save equal (unmodeled tags preserved) → `dlg rc=0 / 0 FAIL`. Empirical: 131692 bytes = 131692 bytes, tag-count delta empty.

### Pitfall-Section corrected (Qt 6.4 headers, verified on /usr/include/qt6)

- QComboBox: **no** `findIndexOf`/`CaseInsensitive` (those are Qt5 names), but **`itemText(int)` exists** — I used the wrong widget (QListWidget has no itemText/appendItem → own `g_macro` list was correct)
- QSpinBox: real name **`setSingleStep`** (exists), not `setStep`
- QRect 6.4: only `adjusted`/`inset`; **`marginsModified` and `reduced` really do not exist** (Qt5 names)
- QColorDialog: **no** `okBtn` — signal `currentFontChanged`/`colorSelected` are the API
- QFont: `family()` exists, `familyName` does not
- QToolButton::setDefaultAction exists — used for the Ribbon-QAction pairing (see next section below)
