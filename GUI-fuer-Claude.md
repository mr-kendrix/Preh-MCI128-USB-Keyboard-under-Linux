# PKT-Editor C++/Qt6 — was wurde mit der GUI gemacht (Kontext-Prompt für Claude)

Repo: `mr-kendrix/Preh-MCI128-USB-Keyboard-under-Linux`, Branch `main`, Qt6 Widgets (NO QML — headless-QTest feint!), Module `pkteditor` (`gui_main.cpp`), Modell `pmap.h/.cpp` + `pmap_dom.cpp` (Lossless DOM-Roundtrip), IPC = Socket-Layer to Home-Assistant (`ipc.cpp`).

## GUI Architektur (Vendor PKM-Editor Replica)

- Fensters: `QWidget win` (VBox) + classic QGridLayout/VBox (headless-QTest never relayouts QWidget/QML → layout-selftest gate checks 6 configs + sabotage case; `PKT_LAYOUT_CHECK`).
- Main window (vendor slim): menu bar (File: New/Open/Close/Quit/Open recent + per-recent items; Edit: Undo/Copy key/Paste key/Delete key/Swap keys/Open template/Restore; Settings: Reconnect daemon/Process list; Help: About), board, left process list, status bar (stat + upload progress). Everything else lives in the key dialog or the small windows (Restore=rdlg, Swap=swdlg, Process picker=dproc with Aktive-Fenster/Reconnect).
- Key dialog: layer list (`layerW`) + the four action buttons work on the list entry via internal index; Output-Box, Macro line, function radios, BTF/AutoRepeat/SlowOutput checkboxes, browser pick, App/Web +, Preview, Rename/Delete proc. Right-click on a key = context menu (Edit/Copy/Paste/Delete), Enter (focus) opens the dialog.
- Board (`QWidget board`): QLayout grid of all POS-Keys, `layout_cells(board)` returns the cells; paintEvent draws the state colors of the vendor:
  - **Grey** = no poskey in slot, **light** = poskey w/o function, **green** = function for selected process, **blue-grey** = function for other processes, **yellow + dark outline** = selected key.
- Grid geometry: with the 128 `<Key>` blocks of a real PKM (X/Y/Width/Height + ParentPosKey, hex-decoded!) the layout uses the **own file coordinates** (uniform scale, centered; `g_geom` in `pmap.h`); without geometry falls back to the legacy WIDE grid (110 caps).
- Double-click on a cell opens the **Key-Dialog**: Output-Box, Macro line, Function-Radios, Restore-History (date + key-count), Save/Close, layer buttons (set/del/rename/copy per layer index), browser choice ("Open website"-Panel from vendor lives here), Process-Rename/Use.
- Undo/Redo stack shared (`g_undo`, byte-exact), Copy/Paste-Key-Dialog (Copy Key A->B / Replace Key B->A), Swap keys (block A<->B at poskey level), Template open (`Open Template` copies layers of a second pmap), File menu (New/Open/Close/Quit) + Recent files (`~/.local/share/pkteditor/recent_files.txt`).
- Process model: left list selects the coloring of the board (green/blue-grey/light via `procSel/for_sel/for_other`), Rename/Process-Delete across all keys; kdotool ps + /proc/comm (`list_processes`).
- Save path: DOM-only (preserves unknown blocks incl. `<Key>` geometry, original hex case), atomic temp+rename, backup `~/.local/share/pkteditor/pmap_backup_*` (30), re-read verify.
- Label-PDF export (own minimal PDF writer, xref verified structurally — the fork's poppler is broken on this machine).
- Label PDF export — TWO sheets now: `X.labels.pdf` (row list, 52 rows/A4 page), `X.geom.pdf` (keyboard overview: rect per POSKey at `<Key>` coords, state fill colors green/light/blue-grey, label = file `<Text>` at MAP level with `<Font>` size) and `X.cut.pdf` (vendor cut-out sheet: cells sorted by key size, cut rects, fitted labels). Fit engine `fit_label()` (same Helvetica width table for fit AND draw; shrink from file font in 0.25pt steps, never grow; keep own line breaks, wrap at spaces; 5pt floor + ellipsis). `<Length>`/xref written raw-latin1 (never toUtf8 — ö/… expand!). Content stream must stay ASCII+latin1, PDF ops: `re f S`, `BT /F0 x Tf … Td(…) Tj ET` (font name required!), no clip-op (poppler/gs reject the alias here; fit gate is the guarantee instead).
- Fit gate: `fit gate: labels=27 truncated=0 boxfails=0` inside clicktest — every drawn text box must be inside its key rect (computed with the same width table).
- IPC: 1 socket pair only, POLLOUT<=200ms, auto reconnect ~4s, Reconnect button, live daemon drain into status line, countdown GUI instead of sleep.
- Security: ACTION exec via execvp (no /bin/sh), `prog::arg` syntax, control-char refusal, 500ms cooldown, `--allow-actions` default OFF.

## Gates (all must stay green before commit)

1. `pkteditor <pmap> --clicktest` (headless QTest, click at (900,300) -> sel-cell; fallback first cell; rc=0 = success, FAILs must be 0).
2. `--layout-selftest` (6 window configs + sabotage case; fails=0).
3. `--undo-test` / `--undo` byte-exact + `pmaprt` roundtrip EXACT incl. Mr.Kendrix.pmap (131924 B).
4. PDF: structural check via python (xref offsets), not via poppler.

## Rules

- NEVER touch the real PKM on the Windows mount directly: always copy to /tmp first for gates (`cp /mnt/Windows/ProgramData/PrehKeyTec/.../Mr.Kendrix.pmap /tmp/...`), NEVER open the real .pmap (write!).
- Vendor spec says "128 POS-Keys, 48 layers per poskey" — real PKM files have 128 poskeys with 1-4 layers; 48 = LAYER CAP not entry count.
- Offscreen: `PKT_SHOT3=1 --clicktest` renders the whole window via `QWidget::grab()` to PNG at 1440x810/1152x648/1024x600 — judge real appearance from those, no display needed.
- `itemClicked` never fires in headless QTest (QListView hit-test); wire the signal, gate the fallback instead.
- Commit messages: conventional, GUI in present-tense; push after every green round.
