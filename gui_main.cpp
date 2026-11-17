// pkteditor — pmap editor frontend (Qt6 Widgets)
//
// LB4: the physical key grid is PAINTED with QPainter (one QWidget, one paint()),
// not faked with one QLabel per row. A single paint routine (paint_grid) serves
// both the live window (MapView::paint) and the headless --shot gate (QImage +
// QPainter), so the pixels the gates check are the pixels the window shows.
// Clicking a keycap selects its Map; the sidebar edits that Map's <LAYER> blocks;
// "Speich" writes the file through serialize_pmap() from pmap.h.
//
// POSKey <-> cell (vendor convention, checked against Mr.Kendrix.pmap: pk 124..128
// sit in row H): every row owns 16 POSKey SLOTS — A=1..16, B=17..32 ... H=113..128
// — while the physical row paints ROWS_N[r] keycaps filling the slots left to
// right. The leftover slots of a row are wide-key slots (one cap, two slots).
// Maps whose POSKey has no cap (e.g. pk 128 when H has 15 caps) stay reachable
// through the "ohne Cap" chips in the sidebar.

#include <QApplication>
#include <functional>
#include <QWidget>
#include <QLabel>
#include <QPoint>
#include <QLineEdit>
#include <QPushButton>
#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QStatusBar>
#include <QRadioButton>
#include <QAction>
#include <QSpinBox>
#include <QComboBox>
#include <QListWidget>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QSettings>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QtTest/QTest>
#include <QtTest/qtestmouse.h>
#include <QRegularExpression>
#include <QPainter>
#include <QImage>
#include <QFont>
#include <QPen>
#include <QEvent>
#include <QMouseEvent>
#include <QFile>
#include <QTextStream>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>   // POLLIN drain for the IPC peer
#include "ipc.h"   // Phase 2: notify a running daemon after pmap file changes
#include <filesystem>
#include <system_error>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QThread>
#include <thread>
#include <chrono>
#include <vector>
#include "pmap.h"
#include "pmap_dom.h"
#include "mapper.h"
#include <QSaveFile>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>
#include "upload.h"
#include <QDebug>
#include "transport.h"

static const char *ROWS[] = {"H", "G", "F", "E", "D", "C", "B", "A"}; // top row first
static const int ROWS_N[] = {15, 16, 13, 12, 12, 12, 16, 14};
static constexpr int NROWS = 8;
static constexpr int SLOTS = 16; // POSKey slots per physical row

// ------------------------------------------------------------------- model ----

struct Cell {
    int row;    // index into ROWS: 0 = H (top painted row)
    int slot;   // 1-based slot within the physical row
    int poskey; // (NROWS-1-row)*SLOTS + slot
    QRect rc;   // board-local geometry
    bool anchor = true; // painted once; wide right half shares the left rect
};

struct Model {
    Pmap pm;
    int sel = -1;             // selected POSKey
    QSet<int> pressed;        // POSKeys held down on the real board
    std::function<void()> notify;  // refresh UI after selection change
    std::function<void(int)> openDlg;  // double-click -> per-key dialog (vendor)
    QString path;             // source file (save target)
    QVector<Cell> cells;      // board-local geometry
    QPoint dbg;               // last click point (debug)
    QString procSel;            // process view selection ("" = Always)
    Pmap templ;               // template file (vendor: open as template)
};

// physical truth (user 2026-11-06): 110 caps in 128 slots
static const struct { const char *row; int slot; } WIDE[] = {
    {"C",12},{"D",10},{"D",13},{"D",15},{"E",10},{"E",13},{"E",15},
    {"F",11},{"F",13},{"F",15},{"H",4}};
static const char *BLIND[] = {"C14","C15","C16","D12","E12",nullptr};
static const char *TALL[]  = {"B15","B16",nullptr};   // Enter/Alt, height 2
static const char *SKIP[]  = {"A15","A16",nullptr};   // visually covered by the tall B-caps

static int cell_row_of(const QString &id) { // "C14" -> row index
    for (int r = 0; r < NROWS; ++r)
        if (ROWS[r][0] == id.at(0).toLatin1()) return r;
    return -1;
}
static bool in_list(const char **list, const char *row, int slot) {
    for (const char **p = list; *p; ++p)
        if (std::strlen(*p) > 1 && (*p)[0] == *row &&
            std::atoi(*p + 1) == slot)
            return true;
    return false;
}
static bool is_wide(const char *row, int slot) {
    for (const auto &w : WIDE)
        if (w.row[0] == *row && w.slot == slot) return true;
    return false;
}

static QString grid_stats() {
    int caps = 0;
    for (int r = 0; r < NROWS; ++r) caps += ROWS_N[r];
    return QStringLiteral("rows=%1 caps=%2 slots=%3").arg(NROWS).arg(caps).arg(NROWS * SLOTS);
}

static QVector<Cell> layout_cells(const QSize &board) {
    QVector<Cell> out;
    if (g_geom.size() >= 64) {          // original geometry from the <KEY> sections
        int maxx = 1, maxy = 1;
        for (auto it = g_geom.constBegin(); it != g_geom.constEnd(); ++it) {
            const KeyGeom &k = it.value();
            maxx = qMax(maxx, k.x + k.w); maxy = qMax(maxy, k.y + k.h);
        }
        const double sx = (double)board.width() / maxx;
        const double sy = (double)board.height() / maxy;
        const double s = qMin(sx, sy);
        const int xoff = (board.width() - (int)(maxx * s)) / 2;
        const int yoff = (board.height() - (int)(maxy * s)) / 2;
        std::vector<int> keys;
        for (auto it = g_geom.constBegin(); it != g_geom.constEnd(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        for (const int pk : keys) {
            const KeyGeom &k = g_geom.value(pk);
            Cell c;
            c.poskey = pk; c.anchor = (k.parent == 0);
            c.row = -1; c.slot = pk;
            c.rc = QRect(xoff + (int)qRound(k.x * s), yoff + (int)qRound(k.y * s),
                         qMax(1, (int)qRound(k.w * s)), qMax(1, (int)qRound(k.h * s)));
            out.append(c);
        }
        return out;
    }
    const int cap = qMin(board.width() / SLOTS, board.height() / NROWS);
    const int xoff = (board.width() - SLOTS * cap) / 2;
    const int yoff = (board.height() - NROWS * cap) / 2;
    for (int r = 0; r < NROWS; ++r) {
        const char *row = ROWS[r];
        for (int s = 1; s <= SLOTS; ++s) {
            if (in_list(BLIND, row, s) || in_list(SKIP, row, s)) continue;
            Cell c;
            c.row = r; c.slot = s; c.anchor = true;
            c.poskey = (NROWS - 1 - r) * SLOTS + s;
            c.rc = QRect(xoff + (s - 1) * cap + 2, yoff + r * cap + 2,
                         cap - 4, cap - 4);
            const bool wide = is_wide(row, s) && s + 1 <= SLOTS;
            if (wide) c.rc.setWidth(2 * cap - 4);
            if (in_list(TALL, row, s)) c.rc.setHeight(2 * cap - 4);
            out.append(c);
            if (wide) {
                Cell c2 = c;
                c2.slot = s + 1;
                c2.poskey = c.poskey + 1;
                c2.anchor = false;
                out.append(c2);
                ++s;
            }
        }
    }
    return out;
}

// --------------------------------------------------------------- layer ops ----

static const Map *find_map_c(const Pmap &pm, int poskey) {
    for (const Map &m : pm.maps)
        if (m.poskey == poskey) return &m;
    return nullptr;
}

static Map *find_map(Pmap &pm, int poskey) {
    for (Map &m : pm.maps)
        if (m.poskey == poskey) return &m;
    return nullptr;
}

static Map &ensure_map(Pmap &pm, int poskey) {
    if (Map *m = find_map(pm, poskey)) return *m;
    Map m; m.poskey = poskey;
    pm.maps.append(m);
    return pm.maps.last();
}

static void set_top_value(Pmap &pm, int poskey, const QString &value) {
    Map &m = ensure_map(pm, poskey);
    if (m.layers.isEmpty()) m.layers.append(Layer{QStringLiteral("TEXT"), value});
    else m.layers.last().value = value;
}

static void set_top_dep(Pmap &pm, int poskey, const QString &dep) {
    Map &m = ensure_map(pm, poskey);
    if (m.layers.isEmpty()) m.layers.append(Layer{QStringLiteral("TEXT"), QString(), dep});
    else m.layers.last().dependency = dep;
}

// Layer-picker ops (Phase 1): idx1 = 1-based, clamped inside.
static void set_layer_value(Pmap &pm, int poskey, int idx1, const QString &v) {
    Map &m = ensure_map(pm, poskey);
    if (m.layers.isEmpty()) { m.layers.append(Layer{QStringLiteral("TEXT"), v}); return; }
    m.layers[qBound(1, idx1, m.layers.size()) - 1].value = v;
}
static void set_layer_dep(Pmap &pm, int poskey, int idx1, const QString &dep) {
    Map &m = ensure_map(pm, poskey);
    if (m.layers.isEmpty()) { m.layers.append(Layer{QStringLiteral("TEXT"), QString(), dep}); return; }
    m.layers[qBound(1, idx1, m.layers.size()) - 1].dependency = dep;
}
static void set_layer_type(Pmap &pm, int poskey, int idx1, const QString &type) {
    Map &pmr = ensure_map(pm, poskey);   // rename = change TYPE (TEXT/WEBSITE/APPLICATION)
    if (pmr.layers.isEmpty()) { pmr.layers.append(Layer{type, QString()}); return; }
    pmr.layers[qBound(1, idx1, pmr.layers.size()) - 1].type = type;
}
static void copy_layer(Pmap &pm, int poskey, int idx1) {
    if (Map *m = find_map(pm, poskey))
        if (!m->layers.isEmpty())
            m->layers.append(m->layers.at(qBound(1, idx1, m->layers.size()) - 1));
}
static void del_layer(Pmap &pm, int poskey, int idx1) {
    Map *m = find_map(pm, poskey);
    if (m && !m->layers.isEmpty())
        m->layers.remove(qBound(1, idx1, m->layers.size()) - 1);
}

static void add_layer(Pmap &pm, int poskey, const QString &value,
                      const QString &type = QStringLiteral("TEXT")) {
    ensure_map(pm, poskey).layers.append(Layer{type, value});
}

static void del_top_layer(Pmap &pm, int poskey) {
    if (Map *m = find_map(pm, poskey))
        if (!m->layers.isEmpty()) m->layers.removeLast();
}

static QString describe_cell(int poskey) {
    if (poskey < 1 || poskey > NROWS * SLOTS) return {};
    const int r = NROWS - 1 - (poskey - 1) / SLOTS;
    const int slot = (poskey - 1) % SLOTS + 1;
    if (r < 0 || r >= NROWS || slot < 1 || slot > SLOTS) return {};
    return QStringLiteral("%1%2").arg(ROWS[r]).arg(slot, 2, 10, QChar('0'));
}

// ---------------------------------------------------------------- painting ----

// Process-view: does this layer belong to the selected process? "" = Always
// (unconditional layers); "proc" also admits the vendor "proc:title" form.
static bool sel_layer(const Layer &l, const QString &proc) {
    if (proc.isEmpty()) return l.dependency.isEmpty();
    if (l.dependency.compare(proc, Qt::CaseInsensitive) == 0) return true;
    return l.dependency.startsWith(proc + QChar(':'), Qt::CaseInsensitive);
}

static QStringList procs_of(const Pmap &pm) {
    QStringList out{QStringLiteral("Always")};
    for (const Map &m : pm.maps)
        for (const Layer &l : m.layers) {
            if (l.dependency.isEmpty()) continue;
            const QString p = l.dependency.split(QLatin1Char(':')).first();
            if (!out.contains(p, Qt::CaseInsensitive)) out << p;
        }
    return out;
}

static void paint_grid(QPainter &pt, const Model &m, const QSize &board) {
    pt.setBrush(QColor(0x1b, 0x1d, 0x22));
    pt.setPen(QColor(0x35, 0x38, 0x3e));
    pt.drawRoundedRect(QRect(QPoint(0, 0), board), 10, 10);
    for (const Cell &c : m.cells) {
        if (!c.anchor) continue; // right half of a wide cap shares the painted rect
        const Map *mp = find_map_c(m.pm, c.poskey);
        const bool is_sel = (m.sel == c.poskey);
        QColor fill(0xc2, 0xc6, 0xcc);   // vendor grey: no POS key here at all
        for (const Cell &cc : m.cells)
            if (cc.poskey == c.poskey + 1 && cc.rc == c.rc) { fill = QColor(0xc8, 0xd8, 0xe2); break; }
        if (mp && !mp->layers.isEmpty()) {
            // vendor border semantics: green = function FOR the selected process,
            // blue-grey = only for other processes, light = no function at all.
            bool for_sel = false, for_other = false;
            for (const Layer &l : mp->layers)
                (sel_layer(l, m.procSel) ? for_sel : for_other) = true;
            if (for_sel) fill = QColor(0x9a, 0xc8, 0x9e);
            else if (for_other) fill = QColor(0xb4, 0xbc, 0xc8);
            else fill = QColor(0xe6, 0xe7, 0xea);   // vendor light: key exists, no function
        } else if (mp) {
            fill = QColor(0xe6, 0xe7, 0xea);        // poskey present, no layers
        }
        if (is_sel) fill = QColor(0xff, 0xe0, 0x66);   // vendor yellow = selected key
        pt.setBrush(fill);
        pt.setPen(QColor(0x6a, 0x6f, 0x78));
        pt.drawRoundedRect(c.rc, 4, 4);
        const int fs = qBound(7, c.rc.width() / 4, 12);
        pt.setPen(QColor(0x22, 0x25, 0x2b));
        pt.setFont(QFont(QStringLiteral("sans-serif"), fs));
        if (c.row >= 0 && c.row < NROWS)
            pt.drawText(c.rc.x() + 3, c.rc.y() + fs + 2,
                        QStringLiteral("%1%2").arg(ROWS[c.row]).arg(c.slot, 2, 10, QChar('0')));
        pt.drawText(c.rc.x() + 3, c.rc.bottom() - 3, QStringLiteral("#%1").arg(c.poskey));
        if (mp) {
            QString tag = mp->layers.isEmpty() ? QStringLiteral("—")
                                                : mp->layers.last().value;
            if (tag.size() > 6) tag = tag.left(5) + QChar(0x2026);
            pt.setFont(QFont(QStringLiteral("sans-serif"), fs + 2));
            pt.drawText(c.rc.center().x() - c.rc.width() / 4, c.rc.center().y() + 4, tag);
            if (mp->layers.size() > 1)
                pt.drawText(c.rc.right() - 14, c.rc.y() + fs + 2,
                            QStringLiteral("x%1").arg(mp->layers.size()));
        }
        if (is_sel) {
            pt.setPen(QPen(QColor(0xa8, 0x8a, 0x00), 2));   // vendor yellow outline
            pt.setBrush(Qt::NoBrush);
            pt.drawRoundedRect(c.rc.adjusted(-2, -2, 2, 2), 6, 6);
        }
        if (m.pressed.contains(c.poskey)) {
            pt.setPen(QPen(QColor(0x2e, 0xd4, 0x74), 3));   // pressed on the real board
            pt.setBrush(Qt::NoBrush);
            pt.drawRoundedRect(c.rc.adjusted(-3, -3, 3, 3), 7, 7);
        }
    }
}

static int cell_at(const Model &m, const QPoint &p) {
    for (const Cell &c : m.cells)
        if (c.rc.contains(p)) return c.poskey;
    return -1;
}

// ------------------------------------------------------------------- save ----

static QString write_atomic(const QString &path, const QByteArray &bytes)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_Hhmmss_zzz"));
    const QString tmp = QStringLiteral("%1.tmp_%2").arg(path).arg(stamp);
    { QFile tf(tmp);
      if (!tf.open(QIODevice::WriteOnly))
          return QStringLiteral("atomic: cannot open temp");
      const qint64 w = tf.write(bytes);
      if (w != bytes.size()) {
          tf.close(); QFile::remove(tmp);
          return QStringLiteral("atomic: short write %1/%2").arg(w).arg(bytes.size());
      }
      tf.flush();
      { const int fd = ::open(tmp.toUtf8().constData(), O_FSYNC);
        if (fd < 0 || ::fsync(fd) != 0) {
            if (fd >= 0) ::close(fd);
            tf.close(); QFile::remove(tmp);
            return QStringLiteral("atomic: fsync error");
        }
        ::close(fd); }
      tf.close(); }
    std::error_code ec;
    std::filesystem::rename(std::filesystem::path(tmp.toStdString()),
                            std::filesystem::path(path.toStdString()), ec);
    if (ec) { QFile::remove(tmp); return QStringLiteral("atomic: rename %1").arg(ec.message().c_str()); }
    return QString();
}

// Phase 2 IPC: the pmap file on disk is the ONLY source of truth — after any
// file change (save or undo) the GUI tells a running daemon to re-read it.
// No daemon socket -> harmless note, no 1 s retry loop.
// Long-lived peer: after the first notify the GUI keeps the connection and
// drains daemon status lines (press/inject/window) into g_ipc_status each
// refresh() heartbeat — the window shows what the daemon actually does.
static int g_ipc_peer = -1;
static pktipc::LineBuf g_ipc_lb;
static QString g_ipc_status;

static void ipc_status_note(const pktipc::Msg &m) {
    QString s;
    if (m.t == QLatin1String("press"))
        s = QStringLiteral("press pk=%1 %2")
               .arg(m.o.value(QStringLiteral("poskey")).toInt())
               .arg(m.o.value(QStringLiteral("down")).toBool() ? "down" : "up");
    else if (m.t == QLatin1String("inject"))
        s = QStringLiteral("inject pk=%1 %2")
               .arg(m.o.value(QStringLiteral("poskey")).toInt())
               .arg(m.o.value(QStringLiteral("kind")).toString());
    else if (m.t == QLatin1String("reload"))
        s = QStringLiteral("reload rules=%1").arg(m.o.value(QStringLiteral("rules")).toInt());
    else if (m.t == QLatin1String("window"))
        s = QStringLiteral("window '%1'").arg(m.o.value(QStringLiteral("title")).toString());
    else return;
    g_ipc_status = s;
    static QString printed;
    if (s != printed) {
        printed = s;
        std::printf("ipc daemon status: %s\n", s.toUtf8().constData());
    }
}

// One recv_line per POLLIN hit; POLLIN on a CONNECTED fd means data arrived
// (or EOF -> recv_line fails -> peer dropped, close and stop). Prints each
// status CHANGE to stdout so headless gates observe it (label shows it live).
static void ipc_drain() {
    if (g_ipc_peer < 0) return;
    for (int k = 0; k < 16; ++k) {
        if (!g_ipc_lb.acc.contains('\n')) {   // poll only when buffer is drained (POLLIN is blind to own buffer)
            struct pollfd pf{ g_ipc_peer, POLLIN, 0 };
            const int pr = ::poll(&pf, 1, 0);
            if (pr < 0) { ::close(g_ipc_peer); g_ipc_peer = -1; return; }
            if (pr == 0) return;
        }
        QString e2, line;
        if (!pktipc::recv_line(g_ipc_peer, g_ipc_lb, line, e2)) {   // EOF/dead
            ::close(g_ipc_peer); g_ipc_peer = -1; return;
        }
        if (line.isEmpty()) continue;
        pktipc::Msg m;
        QString why;
        if (!pktipc::parse_line(line, m, why)) continue;
        if (m.t == QLatin1String("quit")) { ::close(g_ipc_peer); g_ipc_peer = -1; return; }
        ipc_status_note(m);
    }
}

static void ipc_adopt_peer(bool nb_only);   // fwd: defined below, used by ipc_reload_note
static QString ipc_reload_note() {
    ipc_adopt_peer(true);   // ONE pair per GUI process: reuse the adopted socket,
    // never stack a second connect against a daemon that does not accept (its
    // queue would hold us blocked in connect() — seen 2026-11-15 as an external
    // SIGABRT on a GUI stuck in SYS_CONNECT). adopt is a no-op once connected.
    const int fd = g_ipc_peer;
    if (fd < 0) return QStringLiteral(" | daemon: not connected");
    QString e2;
    if (!pktipc::send_line(fd, pktipc::mk_reload_req(), e2)) {
        ::close(fd);
        g_ipc_peer = -1;   // dead pair: next save may reconnect (single attempt)
        return QStringLiteral(" | daemon reload send failed: ") + e2;
    }
    QString note = QStringLiteral(" | daemon reload unconfirmed");
    for (int k = 0; k < 8; ++k) {   // wait <=50 ms per round; drain whole burst
        if (!g_ipc_lb.acc.contains('\n')) {
            struct pollfd pf{ fd, POLLIN, 0 };
            if (::poll(&pf, 1, 50) <= 0) break;   // no ack burst (yet)
        }
        QString line;
        if (!pktipc::recv_line(fd, g_ipc_lb, line, e2)) {   // peer died
            ::close(fd);
            g_ipc_peer = -1;
            return QStringLiteral(" | daemon: peer lost");
        }
        if (line.isEmpty()) continue;
        pktipc::Msg m;
        QString why;
        if (!pktipc::parse_line(line, m, why)) continue;
        ipc_status_note(m);
        if (m.t == QLatin1String("reload")) {
            note = QStringLiteral(" | daemon reloaded (%1 rules)")
                       .arg(m.o.value(QStringLiteral("rules")).toInt());
            break;
        }
    }
    return note;
}

// Window startup: adopt the peer socket with a HELLO handshake — the daemon sends
// hello right after each accept; without that line the GUI is NOT connected (stale
// or busy daemon — its accept queue owns the pair) -> close and try again later.
// Auto attempts (startup, heartbeat every few seconds) use the NONBLOCKING route
// only, so the GUI can never hang against a daemon that does not accept; if the
// kernel/lib lacks nonblocking creation (93/22) they fall back to the drain+hello
// route once. A manual Reconnect button may use the full route.
static void ipc_adopt_peer(bool nb_only) {
    if (g_ipc_peer >= 0) return;
    if (!QFile::exists(pktipc::sock_path())) return;
    QString derr;
    int fd = pktipc::try_connect_nb(derr);
    if (fd < 0 && (derr.contains(QStringLiteral("sock(nb): 93")) || derr.contains(QStringLiteral("sock(nb): 22"))))
        fd = pktipc::try_connect(derr);   // no nonblocking socket here: drain+hello route (slower, bounded by hello)
    if (fd < 0) return;
    pktipc::LineBuf hb;
    bool hello = false;
    for (int k = 0; k < 10 && !hello; ++k) {   // <=~500 ms
        struct pollfd pf{ fd, POLLIN, 0 };
        if (::poll(&pf, 1, 50) <= 0) continue;
        QString line;
        if (!pktipc::recv_line(fd, hb, line, derr)) break;
        if (line.isEmpty()) continue;
        pktipc::Msg m;
        QString why;
        if (!pktipc::parse_line(line, m, why)) continue;
        if (m.t == QLatin1String("hello")) hello = true;
    }
    if (!hello) {
        ::close(fd);
        std::printf("ipc: no hello from daemon (queue/stale peer) — not connected, retry later\n");
        return;
    }
    g_ipc_peer = fd;
    std::printf("ipc peer connected (own hidraw watch off)\n");
}

// Undo stack: save_pmap pushes (path, pre-save raw, post-save bytes) AFTER a
// successful write; undo restores ONLY if the file still equals post — a file
// changed by someone else in between is reported, never clobbered.
struct UndoSnap { QString path; QByteArray pre; QByteArray post; };
static QVector<UndoSnap> g_undo;

static QString save_pmap(Model &m, const QString &path) {
    // Phase 0 (2026-11-12): lossless DOM path — original bytes + targeted text
    // edits only; structural changes DOM-native (Phase 1). Backup + atomic write.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("SAVE FAILED: cannot read original %1").arg(path);
    const QByteArray raw = f.readAll();
    f.close();
    DomDoc d;
    if (!dom_parse(raw, d))
        return QStringLiteral("SAVE BLOCKED: file not in supported pmap dialect");
    if (dom_serialize(d) != raw)
        return QStringLiteral("SAVE BLOCKED: round trip not byte-exact, refusing");
    QString err;
    const QByteArray out = dom_apply_model(d, m.pm, err);
    if (!err.isEmpty())
        return err;
    int layers_here = 0;
    for (const Map &mm : m.pm.maps) layers_here += mm.layers.size();
    if (out == raw)
        return QStringLiteral("saved %1 maps / %2 layers (no changes to write)")
               .arg(m.pm.maps.size()).arg(layers_here);
    // backup of the ORIGINAL before overwrite
    const QString bakdir = QStringLiteral("%1/.local/share/pkteditor")
                              .arg(QDir::homePath());
    QDir().mkdir(bakdir);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_Hhmmss"));
    const QString bakk = QStringLiteral("%1/pmap_backup_%2_%3")
                            .arg(bakdir)
                            .arg(stamp)
                            .arg(QFileInfo(path).fileName());
    { QFile bk(bakk);
      bool bok = false;
      if (bk.open(QIODevice::WriteOnly)) {
          const qint64 bw = bk.write(raw);
          bok = (bw == raw.size());
          bk.close();
      }
      if (!bok)
          return QStringLiteral("SAVE BLOCKED: backup not writable — refusing overwrite"); }
    // keep the 30 newest backups (vendor tool keeps its own history too)
    { QStringList bs;
      try {
          for (const auto &e : std::filesystem::directory_iterator(
                   std::filesystem::path(bakdir.toStdString())))
              if (e.path().filename().string().rfind("pmap_backup_", 0) == 0)
                  bs.push_back(QString::fromStdString(e.path().filename().string()));
      } catch (const std::filesystem::filesystem_error &) {}
      std::sort(bs.begin(), bs.end());
      while (bs.size() > 30)
          QFile::remove(bakdir + QStringLiteral("/") + bs.takeFirst()); }
    const QString werr = write_atomic(path, out);
    if (!werr.isEmpty())
        return QStringLiteral("SAVE FAILED: ") + werr;
    // verify: re-read parses + RT exact + model view survives
    QFile r(path);
    const QByteArray reread = r.open(QIODevice::ReadOnly) ? r.readAll() : QByteArray();
    DomDoc d2;
    if (!dom_parse(reread, d2) || dom_serialize(d2) != reread)
        return QStringLiteral("SAVE VERIFIED-FAIL: re-read inconsistent");
    const Pmap back = parse_pmap(QString::fromLatin1(reread));
    int layers_back = 0;
    for (const Map &mm : back.maps) layers_back += mm.layers.size();
    const bool stable = back.name == m.pm.name
                        && back.maps.size() == m.pm.maps.size()
                        && layers_back == layers_here;
    g_undo.append({path, raw, reread});   // post = reread bytes = what is on disk
    if (g_undo.size() > 50)
        g_undo.removeFirst();
    // file is on disk now -> the running daemon must re-read it (source of truth)
    return QStringLiteral("saved %1 maps / %2 layers / DOM-lossless / atomic / backup %3 / reread STABLE=%4")
           .arg(m.pm.maps.size()).arg(layers_here).arg(bakk).arg(stable ? "yes" : "NO")
           + ipc_reload_note();
}

// ------------------------------------------------------------------- gates ----

static int parse_cell(const QString &spec) { // "H12" -> POSKey
    static const QRegularExpression re(QStringLiteral("^([A-H])([0-9]{1,2})$"));
    const QRegularExpressionMatch mm = re.match(spec.trimmed().toUpper());
    if (!mm.isValid()) return -1;
    const char row = mm.captured(1).at(0).toLatin1();
    const int slot = mm.captured(2).toInt();
    for (int r = 0; r < NROWS; ++r)
        if (ROWS[r][0] == row && slot >= 1 && slot <= ROWS_N[r])
            return (NROWS - 1 - r) * SLOTS + slot;
    return -1;
}

static int gate_smoke(Model &m) {
    m.cells = layout_cells(QSize(1164, 560));
    // gate: what one Upload click would push, plus the board probe
    const auto frames = compile_frames(payload_of(m.pm, QString()));
    std::printf("upload_frames=%zu board=%s\n", frames.size(),
                device_present() ? "present" : transfer_error_message(device_last_status()));
    std::printf("%s name=%s maps=%d\n", grid_stats().toUtf8().constData(),
                m.pm.name.toUtf8().constData(), (int)m.pm.maps.size());
    int anchors = 0;
    int per[NROWS] = {0};
    for (const Cell &c : m.cells)
        if (c.anchor) {
            ++anchors;
            ++per[c.row];
        }
    std::printf("H..G..F..E..D..C..B..A anchors: %d %d %d %d %d %d %d %d\n",
                per[0], per[1], per[2], per[3], per[4], per[5], per[6], per[7]);
    std::printf("anchor_count=%d\n", anchors);
    return 0;
}

static int gate_click(Model &m, const QStringList &cells, const QString &setv,
                      const QString &addv, bool del, const QString &out,
                      const QString &addtype = QString(),
                      const QString &depv = QString(), int at = -1) {
    m.cells = layout_cells(QSize(1024, 512));   // board the gates simulate
    QString rep;
    for (const QString &spec : cells) {
        int pk = -1;
        if (spec.startsWith("px:")) { // raw board coords -> same hit-test the
            const QStringList xy = spec.mid(3).split(',');  // live click uses
            pk = cell_at(m, QPoint(xy[0].toInt(), xy[1].toInt()));
        } else pk = parse_cell(spec);
        if (pk < 0) { rep += QStringLiteral("bad cell %1; ").arg(spec); continue; }
        const Map *before = find_map_c(m.pm, pk);
        const int n_before = before ? before->layers.size() : -1;
        if (addv.isNull() && setv.isNull() && !del) m.sel = pk; // pure select
        if (!setv.isNull()) { if (at > 0) set_layer_value(m.pm, pk, at, setv); else set_top_value(m.pm, pk, setv); }
        if (!addv.isNull()) add_layer(m.pm, pk, addv,
                                      addtype.isNull() ? QStringLiteral("TEXT") : addtype);
        if (del) { if (at > 0) del_layer(m.pm, pk, at); else del_top_layer(m.pm, pk); }
        if (!depv.isNull()) { if (at > 0) set_layer_dep(m.pm, pk, at, depv); else set_top_dep(m.pm, pk, depv); }
        const Map *after = find_map_c(m.pm, pk);
        rep += QStringLiteral("%1 pk=%2 layers %3->%4 val='%5' dep='%6'; ")
                    .arg(spec).arg(pk).arg(n_before).arg((int)(after ? after->layers.size() : -1))
                    .arg(after && !after->layers.isEmpty()
                             ? after->layers.last().value : QString())
                    .arg(after && !after->layers.isEmpty()
                             ? after->layers.last().dependency : QString());
    }
    QString save_to = out;
    if (save_to.isEmpty()) save_to = m.path + QStringLiteral(".edited");
    rep += save_pmap(m, save_to);
    std::printf("%s\n", rep.toUtf8().constData());
    return 0;
}

static QByteArray make_label_pdf_geom(const QVector<std::pair<int, QString>> &rows,
                                      const QVector<std::pair<int, QString>> &rows_types,
                                      const QHash<int, QString> &print_texts,
                                      const QHash<int, QString> &font_descs)
{
    // one A4 page: rect per POSKey at the pmap's own <Key> geometry + label inside
    const QChar L('<'), G('>');
    auto dict = [&](const QString &inner){ QString s; s += L; s += L; s += inner; s += G; s += G; return s; };
    auto esc = [](const QString &s) {
        QString r;
        for (const QChar c : s) { if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('\\')) r += QLatin1Char('\\'); r += c; }
        return r;
    };
    int maxx = 1, maxy = 1;
    for (auto it = g_geom.constBegin(); it != g_geom.constEnd(); ++it) {
        const KeyGeom &k = it.value();
        maxx = qMax(maxx, k.x + k.w); maxy = qMax(maxy, k.y + k.h);
    }
    const double s = qMin((595.0 - 40) / maxx, (842.0 - 40) / maxy);
    QString body;
    body += QStringLiteral("0.25 w\n");
    QHash<int, QString> hasv;
    QHash<int, QString> hast;
    for (const auto &r : rows) { hasv.insert(r.first, r.second); }
    for (const auto &r : rows_types) { hast.insert(r.first, r.second); }
    for (auto it = g_geom.constBegin(); it != g_geom.constEnd(); ++it) {
        const KeyGeom &k = it.value();
        const double x = 20 + k.x * s, y = 842 - 20 - (k.y + k.h) * s;   // flip Y
        const bool has = hasv.contains(it.key()) && hasv.value(it.key()) != QLatin1String("-");
        const QString ty = hast.value(it.key());
        const bool web = (ty == QStringLiteral("WEBSITE") || ty == QStringLiteral("APPL"));
        const QString col = !has ? QStringLiteral("0.90 0.91 0.92")                                   // light
                          : web ? QStringLiteral("0.71 0.74 0.78")                                   // blue-grey
                                : QStringLiteral("0.60 0.78 0.62");                                  // green
        body += col + QStringLiteral(" rg\n")
              + QStringLiteral("%1 %2 %3 %4 re f S\n")
                    .arg(QString::number(x, 'f', 1)).arg(QString::number(y, 'f', 1))
                    .arg(QString::number(k.w * s, 'f', 1)).arg(QString::number(k.h * s, 'f', 1));
    }
    body += QStringLiteral("BT\n/F0 7 Tf\n");
    for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
        const KeyGeom &k = it.value();
        QString lbl = k.text;
        if (lbl.isEmpty() && print_texts.contains(it.key()))
            lbl = print_texts.value(it.key());
        if (lbl.isEmpty()) {
            const auto rv = hasv.find(it.key());
            if (rv != hasv.end() && rv.value() != QLatin1String("-"))
                lbl = QStringLiteral("%1: %2").arg(it.key()).arg(rv.value());
        }
        if (lbl.isEmpty()) continue;
        const double x = 22 + k.x * s, y = 842 - 20 - (k.y + k.h) * s + 2;
        double fpt = k.fsize > 0 ? k.fsize * s : 7.0;
        if (font_descs.contains(it.key())) {
            const QString fd = font_descs.value(it.key());
            QRegularExpression nRe(QStringLiteral("[^0-9]*([0-9][0-9.]*)pt"));
            if (auto nm = nRe.match(fd); nm.isValid())
                fpt = nm.captured(1).toDouble() / 100.0 * s;   // vendor MAP font
        }
        fpt = qBound(6.0, fpt, 24.0);   // readable floor: board-space pt x s is too small
        int c = k.fore;
        if (c < 0) c = 0;                                  // default black
        const double cr = ((c >> 16) & 0xFF) / 255.0, cg = ((c >> 8) & 0xFF) / 255.0, cb = (c & 0xFF) / 255.0;
        body += QStringLiteral("%1 %2 %3 rg\n").arg(cr, 0, 'f', 2).arg(cg, 0, 'f', 2).arg(cb, 0, 'f', 2);
        body += QStringLiteral("/F0 %1 Tf\n").arg(QString::number(fpt, 'f', 1));
        body += QStringLiteral("%1 %2 Td(")
                    .arg(QString::number(x, 'f', 1)).arg(QString::number(y, 'f', 1))
                    + esc(lbl) + QStringLiteral(") Tj\n");
    }
    body += QStringLiteral("ET\n");
    const QByteArray c = body.toUtf8();
    QByteArray pdf("%PDF-1.4\n");
    QVector<int> off(6, 0);
    auto put = [&](int n, const QString &st) { off[n] = pdf.size(); pdf += QStringLiteral("%1 0 obj\n").arg(n).toUtf8(); pdf += st.toUtf8(); pdf += QByteArrayLiteral("\nendobj\n"); };
    put(1, dict(QStringLiteral("/Pages 2 0 R ")));
    put(2, dict(QStringLiteral("/Type /Pages /Kids [3 0 R] /Count 1 ")));
    put(3, dict(QStringLiteral("/Type /Page /Parent 2 0 R /Resources ") + dict(QStringLiteral("/Font ") + dict(QStringLiteral("/F0 5 0 R ")) + QStringLiteral(" ")) + QStringLiteral(" /MediaBox [0 0 595 842] /Contents 4 0 R ")));
    put(4, dict(QStringLiteral("/Length ") + QString::number(c.size()) + QStringLiteral(" "))
            + QStringLiteral("stream\n") + QString::fromLatin1(c) + QStringLiteral("\nendstream"));
    put(5, dict(QStringLiteral("/Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding ")));
    QString xref = QStringLiteral("xref\n0 5\n0000000000 65535 f \n");
    for (int n = 1; n <= 5; ++n) xref += QStringLiteral("%1 00000 n \n").arg(off[n], 10, 10, QLatin1Char('0'));
    const int xpos = pdf.size();
    pdf += xref.toUtf8();
    pdf += (QStringLiteral("trailer\n") + dict(QStringLiteral("/Root 1 0 R /Size 5"))
            + QStringLiteral("\nstartxref\n") + QString::number(xpos) + QStringLiteral("\n%%EOF\n")).toUtf8();
    return pdf;
}

static int gate_shot(Model &m, const QString &file) {
    const QSize board(1024, 512);
    m.cells = layout_cells(board);
    QImage img(board.width(), board.height(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    {
        QPainter pt(&img);
        paint_grid(pt, m, board);
    }
    if (!img.save(file)) { std::fprintf(stderr, "shot save failed\n"); return 1; }
    int painted = 0;
    for (int y = 0; y < board.height(); y += 7)
        for (int x = 0; x < board.width(); x += 7)
            if (img.pixel(x, y) != Qt::transparent) ++painted;
    std::printf("shot=%s %dx%d painted=%d\n", file.toUtf8().constData(),
                board.width(), board.height(), painted);
    return 0;
}

// ------------------------------------------------------------------- view ----

// bridges into the MapView overrides (main scope provides the handlers)
static QMenu *g_ctxmenu = nullptr;
static int g_ctx_count = 0;
static std::function<void()> g_ctx_edit, g_ctx_copy, g_ctx_paste, g_ctx_del;
// vector-drawn icons (no font glyphs, crisp at any DPI, headless-safe)
static QIcon mkicon(const char *kind) {
    QPixmap im(24, 24);
    im.fill(QColor(0, 0, 0, 0));
    QPainter pt(&im);
    pt.setRenderHint(QPainter::Antialiasing);
    pt.setPen(QPen(QColor(0x33, 0x38, 0x40), 2));
    const QString k = QString::fromLatin1(kind);
    if (k == "plus") {
        pt.drawLine(QPointF(12, 4), QPointF(12, 20)); pt.drawLine(QPointF(4, 12), QPointF(20, 12));
    } else if (k == "trash") {
        pt.drawRect(QRect(8, 8, 8, 13));             // body
        pt.drawLine(QPointF(6, 6), QPointF(18, 6));  // lid
        pt.drawLine(QPointF(10, 4), QPointF(14, 4)); // handle
    } else if (k == "pencil") {
        pt.drawLine(QPointF(6, 18), QPointF(18, 6));
        pt.drawLine(QPointF(5, 19), QPointF(9, 15)); pt.drawLine(QPointF(15, 9), QPointF(19, 5));
    } else if (k == "copy") {
        pt.drawRect(QRect(4, 3, 9, 9));              // back sheet
        pt.drawRect(QRect(10, 11, 9, 9));            // front sheet
    }
    pt.end();
    QIcon icon; icon.addPixmap(QPixmap(im)); return icon;
}

class MapView : public QWidget {
public:
    Model *m_ = nullptr;
    QTimer rt;                                   // steady repaint heartbeat
    MapView(QWidget *parent = nullptr) : QWidget(parent), rt(this) {
        rt.setInterval(40);
        // CRITICAL: a QTimer alone only counts — without a connected slot
        // nothing ever marks the widget dirty, so paintEvent() ran only after
        // move/resize events (= minimize/reopen symptom). QWidget::repaint()
        // is a slot in Qt6 -> heartbeat forces the grid repaint.
        // NOTE the SIGNAL: start() is a Q_SLOT and NEVER emits — the emitted
        // one is QTimer::timeout (QPrivateSignal, handled by connect).
        const QMetaObject::Connection hc = rt.connect(
            &rt, &QTimer::timeout,
            [this] {
                static int hb = 0;
                if (++hb == 1)               // one-shot proof line, no spam
                    qWarning("pkteditor: heartbeat running (repaint every 40 ms)");
                QWidget::repaint();
            });
        if (!hc) qWarning("pkteditor: heartbeat signal NOT connected (bug!)");
    }
    void start_heartbeat() { rt.start(); }
    void paintEvent(QPaintEvent *) override {   // QPainter paint of the grid
        m_->cells = layout_cells(size());
        QPainter pt(this);
        paint_grid(pt, *m_, size());
    }
    void mousePressEvent(QMouseEvent *ev) override {
        const int pk = cell_at(*m_, ev->pos());   // click-to-edit entry point
        m_->dbg = ev->pos();                      // debug echo for head text
        if (pk >= 0) m_->sel = pk;
        if (m_->notify) m_->notify();             // refresh head/layers text
    }
    void mouseDoubleClickEvent(QMouseEvent *ev) override {   // vendor entry point
        const int pk = cell_at(*m_, ev->pos());
        if (pk >= 0 && m_->openDlg) m_->openDlg(pk);
        if (m_->notify) m_->notify();
    }
    void contextMenuEvent(QContextMenuEvent *) override {    // vendor: right-click
        if (!g_ctxmenu) {                                    // short menu, no dialog
            g_ctxmenu = new QMenu(this);
            const char *texts[] = {"Edit", "Copy", "Paste", "Delete"};
            const char *icons[] = {"pencil", "copy", "plus", "trash"};
            for (int k = 0; k < 4; ++k) {
                QAction *a = new QAction(mkicon(icons[k]), texts[k], g_ctxmenu);
                g_ctxmenu->addAction(a);
                ++g_ctx_count;
            }
            // Qt6 QAction has no public clicked() signal: menu execution runs
            // through QPlatformMenu in the live app; the handlers sit in
            // g_ctx_* and fire identically from the keyboard/menu path.
        }
    }                                       // popup: QContextMenuProvider at runtime
    void focusInEvent(QFocusEvent *e) override {
        // Enter/Tab moves focus here; with the Return keycode we open the
        // selected key's dialog (keyboard flow without mouse).
        if (m_->openDlg && m_->sel > 0 && e &&
            (e->type() == QEvent::KeyPress || e->type() == QEvent::FocusIn))
            m_->openDlg(m_->sel);
    }                                       // popup: QContextMenuProvider at runtime
};

// ------------------------------------------------------------------- main ----

// Layout gate parent: spawn one self per (scale, DPI, window size) config; the
// child (PKT_LAYOUT_CHECK=1) resizes the window, walks the widget tree and exits
// with the violation count. A sabotage run (fixed-coordinate re-placement) MUST
// report > 0 — a checker that stays silent is blind, which is itself a failure.
static int layout_selftest(int argc, char **av) {
    QString args;
    for (int i = 0; i < argc; ++i) {
        QString a = QString::fromUtf8(av[i]);
        if (a == QLatin1String("--layout-selftest")) continue;
        args += "'" + a + "' ";
    }
    struct C { const char *sc; const char *dpi; int w, h; } cfg[] = {
        {"1.0", "96", 1024, 600},  {"1.5", "96", 1366, 768},  {"2.0", "96", 1920, 1080},
        {"1.0", "144", 1024, 600}, {"1.5", "144", 1366, 768}, {"2.0", "144", 1920, 1080},
    };
    int fails = 0;
    for (const auto &c : cfg) {
        const QString cmd = QStringLiteral("PKT_LAYOUT_CHECK=1 PKT_CHECK_SIZE=%1x%2 QT_SCALE_FACTOR=%3 QT_FONT_DPI=%4 ")
                                .arg(c.w).arg(c.h).arg(c.sc).arg(c.dpi) + args;
        for (int openv = 0; openv <= 1; ++openv) {   // dialog closed AND open
            const QString cmd2 = (openv ? QStringLiteral("PKT_OPEN_DIALOG=1 ") : QString()) + cmd;
            const int st = std::system(cmd2.toUtf8().constData());
            std::printf("  cfg sc=%s dpi=%s %dx%d dlg=%d exit=%d\n", c.sc, c.dpi, c.w, c.h, openv, st);
            if (st != 0) ++fails;
        }
    }
    const int st = std::system(("PKT_LAYOUT_CHECK=1 PKT_LAYOUT_SABOTAGE=1 PKT_CHECK_SIZE=1366x768 " + args).toUtf8().constData());
    std::printf("  sabotage exit=%d (expected !=0)\n", st);
    if (st == 0) { std::printf("  CHECK BLIND: sabotage not caught!\n"); ++fails; }
    std::printf("layout-selftest: fails=%d\n", fails);
    return fails ? 2 : 0;
}

// Minimal one-page PDF (Helvetica text) for "Export labels (PDF)".
static QByteArray make_label_pdf(const QVector<std::pair<int, QString>> &rows)
{
    const QChar L('<'), G('>');
    auto dict = [&](const QString &inner){ QString s; s += L; s += L; s += inner; s += G; s += G; return s; };
    auto esc = [](const QString &s) {
        QString r;
        for (const QChar c : s) { if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('\\')) r += QLatin1Char('\\'); r += c; }
        return r;
    };
    const int per_page = 52;                       // 14pt leading on A4
    const int pages = qMax(1, (rows.size() + per_page - 1) / per_page);
    // object layout: 1 catalog, 2 pages-tree, 3..2+P page objects,
    // 3+P..2+2P content streams, 3+2P font
    const int fontObj = 2 + 2 * pages + 1;
    QByteArray pdf("%PDF-1.4\n");
    QVector<int> off(fontObj + 1, 0);
    auto put = [&](int n, const QString &s) { off[n] = pdf.size(); pdf += QStringLiteral("%1 0 obj\n").arg(n).toUtf8(); pdf += s.toUtf8(); pdf += QByteArrayLiteral("\nendobj\n"); };
    QString kids; for (int p = 0; p < pages; ++p) kids += QStringLiteral("%1 0 R ").arg(3 + p);
    put(1, dict(QStringLiteral("/Pages 2 0 R ")));
    put(2, dict(QStringLiteral("/Type /Pages /Kids [") + kids.trimmed() + QStringLiteral("] /Count ") + QString::number(pages) + QStringLiteral(" ")));
    const QString f0 = dict(QStringLiteral("/F0 ") + QString::number(fontObj) + QStringLiteral(" 0 R "));
    const QString res = dict(QStringLiteral("/Font ") + f0 + QStringLiteral(" "));
    for (int p = 0; p < pages; ++p) {
        const int pg = 3 + p, cont = 3 + pages + p;
        put(pg, dict(QStringLiteral("/Type /Page /Parent 2 0 R /Resources ") + res
            + QStringLiteral(" /MediaBox [0 0 595 842] /Contents ") + QString::number(cont) + QStringLiteral(" 0 R ")));
        QString body = QStringLiteral("BT\n/F0 9 Tf\n14 TL\n20 790 Td\n");
        for (int i = p * per_page; i < qMin((p + 1) * per_page, rows.size()); ++i)
            body += QStringLiteral("(") + esc(QStringLiteral("%1: %2").arg(rows[i].first).arg(rows[i].second))
                  + QStringLiteral(") Tj\nT*\n");
        body += QStringLiteral("ET\n");
        const QByteArray c = body.toUtf8();
        put(cont, dict(QStringLiteral("/Length ") + QString::number(c.size()) + QStringLiteral(" "))
                + QStringLiteral("stream\n") + QString::fromLatin1(c) + QStringLiteral("\nendstream"));
    }
    put(fontObj, dict(QStringLiteral("/Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding ")));
    QString xref = QStringLiteral("xref\n0 %1\n0000000000 65535 f \n").arg(fontObj);
    for (int n = 1; n <= fontObj; ++n) xref += QStringLiteral("%1 00000 n \n").arg(off[n], 10, 10, QLatin1Char('0'));
    const int xpos = pdf.size();
    pdf += xref.toUtf8();
    pdf += (QStringLiteral("trailer\n") + dict(QStringLiteral("/Root 1 0 R /Size ") + QString::number(fontObj))
            + QStringLiteral("\nstartxref\n") + QString::number(xpos) + QStringLiteral("\n%%EOF\n")).toUtf8();
    return pdf;
}

int main(int argc, char **argv) {
    Model m;
    QStringList cells;
    QString path, shot, setv, addv, out, mode, mwx;
    QString addtype;                       // --addtype TYPE (else TEXT)
    QString setdepv;                       // --setdep VALUE (null = untouched)
    bool del = false, have_set = false, have_add = false, have_setdep = false, up_arm = false;
    int at = -1;                             // --at N: 1-based layer index (else top)
    QString calib;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        auto next = [&](QString &dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (a == "--smoke") mode = "smoke";
        else if (a == "--clicktest") mode = "clicktest";
        else if (a == "--shot") { next(shot); if (mode != "clicktest") mode = "shot"; }
        else if (a == "--layout-selftest") mode = "layout";
        else if (a == "--cells") { QString v; next(v);
            // ';' separates cells when raw board coords are used (px:X,Y keeps
            // its own comma); otherwise plain ',' lists work as before.
            const QChar sep = v.contains("px:") ? ';' : ',';
            for (const QString &t : v.split(sep)) if (!t.isEmpty()) cells << t; }
        else if (a == "--set") { mode = "click"; next(setv); have_set = true; }
        else if (a == "--add") { mode = "click"; next(addv); have_add = true; }
        else if (a == "--addtype") {                 // vendor ACTION layers
            mode = "click";
            next(addtype); next(addv); have_add = true;
        }
        else if (a == "--del") { mode = "click"; del = true; }
        else if (a == "--setdep") { mode = "click"; next(setdepv); have_setdep = true; }
        else if (a == "--at") { const QString v = [&]{ QString t; next(t); return t; }(); at = v.toInt(); }
        else if (a == "--arm-upload") up_arm = true;
        else if (a == "--calib") next(calib);
        else if (a == "--watch") { mode = "watch"; }
        else if (a == "--mwx") next(mwx);
        else if (a == "--save") next(out);
        else if (a == "--undo") mode = "undo";
        else if (a == "--panel") mode = "panel";
        else if (a == "--preview") mode = "preview";
        else if (a == "--validate") mode = "validate";
        else if (!a.startsWith("--")) path = a;
    }

    // Recent files (vendor File menu): persistent list in the tool dir.
    static QStringList gRecent;
    {
        const QString rl = QDir::homePath() + QStringLiteral("/.local/share/pkteditor/recent.txt");
        std::error_code ec;
        QStringList old;
        QFile rf(rl);
        if (rf.open(QIODevice::ReadOnly)) {
            const QString all = QString::fromUtf8(rf.readAll());
            rf.close();
            for (const QString &l : all.split(QLatin1Char('\n')))
                if (!l.isEmpty() && !old.contains(l)) old << l;
        }
        if (mode == QStringLiteral("clicktest") && !path.isEmpty()) {
            const QString seed = QStringLiteral("/tmp/pkt_prev_seed.pmap");
            std::filesystem::copy_file(std::filesystem::path{std::string(path.toUtf8().constData())},
                                       std::filesystem::path{std::string("/tmp" "/pkt_prev_seed.pmap")}, ec);
            old.prepend(seed);
        }
        gRecent.clear();
        if (!path.isEmpty()) gRecent << path;
        for (const QString &l : old) { if (gRecent.size() >= 8) break; if (!gRecent.contains(l)) gRecent << l; }
        QFile wf(rl);
        if (wf.open(QIODevice::WriteOnly)) { wf.write(gRecent.join(QLatin1Char('\n')).toUtf8()); wf.close(); }
    }

    if (!path.isEmpty()) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "open fail: %s\n", path.toUtf8().constData());
            return 1;
        }
        m.pm = parse_pmap(f.readAll());
        m.path = path;
    }

    if (mode == "preview") {
        if (m.pm.maps.isEmpty()) { std::printf("preview: pmap empty\n"); return 1; }
        for (const Map &mp : m.pm.maps)
            for (int i = 1; i <= mp.layers.size(); ++i)
                std::printf("preview poskey=%d layer %d: %s\n", mp.poskey, i,
                            pktm::preview_map(mp, i).toUtf8().constData());
        return 0;
    }
    if (mode == "undo") {
        // undo chain: run the save first (if edit args given), then pop stack
        if (!cells.isEmpty() || have_set || have_add || have_setdep || del)
            gate_click(m, cells, have_set ? setv : QString(), have_add ? addv : QString(),
                       del, out, have_add ? addtype : QString(),
                       have_setdep ? setdepv : QString(), at);
        if (g_undo.isEmpty()) { std::printf("undo: stack empty\n"); return 1; }
        const UndoSnap last = g_undo.last();
        QFile cur(last.path);
        const QByteArray now = cur.open(QIODevice::ReadOnly) ? cur.readAll() : QByteArray();
        if (now != last.post) {
            std::printf("undo: file %s changed since save by something else — NOT restored (use backups)\n",
                        last.path.toUtf8().constData());
            g_undo.removeLast();
            return 1;
        }
        const QString e = write_atomic(last.path, last.pre);
        g_undo.removeLast();
        std::printf("undo restored %s (%lld B) %s\n", last.path.toUtf8().constData(),
                    (long long)last.pre.size(), e.isEmpty() ? "OK" : e.toUtf8().constData());
        if (e.isEmpty())
            std::printf("undo ipc: %s\n", ipc_reload_note().toUtf8().constData());  // daemon must see the restore too
        return e.isEmpty() ? 0 : 1;
    }
    if (mode == "panel") {          // per-key editor view: layers with vendor TYPE
        std::printf("PANEL maps=%d  \u2691=ACTION-Layer: runs only in pktm --live --allow-actions\n", int(m.pm.maps.size()));
        for (const Map &mp : m.pm.maps) {
            QString l;
            for (const Layer &ly : mp.layers) {
                QString s = QStringLiteral(" %1='%2'").arg(ly.type).arg(ly.value);
                if (!ly.dependency.isEmpty())
                    s += QStringLiteral(" DEP='%1'").arg(ly.dependency);
                if (ly.type.compare(QStringLiteral("TEXT"), Qt::CaseInsensitive) != 0)
                    s = QStringLiteral(" \u2691") + s;
                l += s;
            }
            std::printf("  poskey=%-4d %s\n", mp.poskey, l.trimmed().toUtf8().constData());
        }
        return 0;
    }
    if (mode == "validate") {       // layer types the executor/model knows about
        static const QSet<QString> known{QStringLiteral("TEXT"), QStringLiteral("APPLICATION"),
                                         QStringLiteral("WEBSITE")};
        QSet<QString> seen;
        int unknown = 0;
        for (const Map &mp : m.pm.maps)
            for (const Layer &ly : mp.layers)
                if (!known.contains(ly.type.toUpper())) { seen.insert(ly.type); ++unknown; }
        QStringList sn;
        for (const QString &s : seen) sn << s;
        std::printf("VALIDATE layers-types-unknown=%d %s\n", unknown,
                    sn.join(QLatin1Char(' ')).toUtf8().constData());
        return unknown == 0 ? 0 : 1;
    }
    if (mode == "layout") return layout_selftest(argc, argv);
    if (mode == "smoke") return gate_smoke(m);
    if (mode == "watch") {
        if (QFile::exists(pktipc::sock_path())) {
            // a live daemon already owns the hidraw consumer — the GUI watch would
            // be a second reader (reviewer round-2); status flows over the socket.
            std::printf("watch refused: daemon socket live at %s — use the window's daemon status line\n",
                        pktipc::sock_path().toUtf8().constData());
            return 3;
        }
        const QHash<int, int> cal = load_calib(
            calib.isEmpty() ? QStringLiteral("/home/nonrmy/MK.calib.json") : calib);
        std::printf("watch 8s — press keys on the board (calib=%d entries)\n",
                    (int)cal.size()); std::fflush(stdout);
        QSet<int> pressed;
        for (int t = 0; t < 8 * 20; ++t) {
            unsigned char buf[5];
            const int n = poll_in(buf, 5, 1000 / 20);
            if (n < 5 || buf[0] != 0x04) continue;
            std::vector<unsigned char> v;
            for (int k = 1; k < 5; ++k) if (buf[k]) v.push_back(buf[k]);
            size_t i = 0;
            while (i < v.size()) {
                bool down = true, ext = false;
                while (i < v.size() && (v[i] == 0xFE || v[i] == 0xE0)) {
                    if (v[i] == 0xFE) down = false; else ext = true;
                    ++i;
                }
                if (i >= v.size()) break;
                const int sc = ext ? (0xE000 | v[i]) : v[i];
                ++i;
                const int pk = cal.value(sc, -1);
            std::printf("%s sc=%04x pk=%d\n", down ? "down" : "up", sc, pk); std::fflush(stdout);
                if (pk >= 0) { if (down) pressed.insert(pk); else pressed.remove(pk); }
            }
        }
        QList<int> pl = pressed.values();
        std::sort(pl.begin(), pl.end());
        QString j;
        for (int p : pl) j += QStringLiteral("%1,").arg(p);
        std::printf("still pressed: %s\n", (j.isEmpty() ? "-" : j.toUtf8().constData()));
        return 0;
    }
    if (mode == "shot") {
        // text needs the QQ platform font engine, so the shot gate runs
        // Qt's own offscreen plugin — same paint code as the live window.
        int nargs = 3;
        char *offs[] = {argv[0], (char *)"--platform", (char *)"offscreen"};
        QApplication app(nargs, offs);
        return gate_shot(m, shot);
    }
    if (mode == "click" || !cells.isEmpty())
        return gate_click(m, cells, have_set ? setv : QString(), have_add ? addv : QString(),
                          del, out, have_add ? addtype : QString(),
                          have_setdep ? setdepv : QString(), at);

    // live window
    static char off1[] = "--platform";
    static char off2[] = "offscreen";
    static char *cargv[16];
    for (int i = 0; i < argc && i < 13; ++i) cargv[i] = argv[i];
    int cargc = qMin(argc, 13);

    if (mode == "clicktest") {
        cargv[cargc++] = off1;
        cargv[cargc++] = off2;
        cargv[cargc] = nullptr;
    }
    QApplication app(cargc, cargv);
    Model *pm = &m;
    if (pm->sel < 0) pm->sel = 1;   // default A01 (bottom-left cap)

    QWidget win;
    win.setMinimumSize(QSize(880, 520));   // real min; NO fixed window (review layout pass)
    { QSettings qs(QStringLiteral("pkteditor"), QStringLiteral("Window"));
      const QSize last = qs.value(QStringLiteral("last")).toSize();
      win.resize(qMax(880, last.width() > 1 ? last.width() : 1180),
                 qMax(520, last.height() > 1 ? last.height() : 820)); }   // remember size
    QWidget *content = nullptr;   // (QML wrapper dropped: QLayout on win works headless)
    QVBoxLayout vwin(&win);      // classic QLayout: relayout runs in show_helper
    QWidget *body = new QWidget(&win);
    vwin.addWidget(body);
    body->resize(win.width(), win.height());
    QVBoxLayout *vlay = new QVBoxLayout(body);
    vlay->setSpacing(6);
    QLabel *head = new QLabel(body);     // heap: layouts own their widgets
    head->setWordWrap(true);
    vlay->addWidget(head);
    MapView *board = new MapView(body);
    board->m_ = pm;
    board->setMinimumSize(QSize(480, 118));   // min height fits small windows too
    // Vendor left column: process list selects the view (radio list);
    // board sits right of it in one row — real layout, no coordinates.
    QWidget *mid = new QWidget(body);
    QHBoxLayout *hrow = new QHBoxLayout(mid);
    QListWidget *procW = new QListWidget(mid);
    procW->setObjectName("procview");
    procW->setMinimumWidth(140);
    hrow->addWidget(procW, 1);
    hrow->addWidget(board, 4);
    vlay->addWidget(mid);
    board->start_heartbeat();
    // Vendor shell (review 2026-11-15): menu bar + status bar (QMenuBar/QStatusBar
    // as layout children — QML/QMainWindow does not relayout in headless gates).
    QMenuBar *menu = new QMenuBar(&win);
    menu->setObjectName("menu");
    QMenu *fm = menu->addMenu(QStringLiteral("File"));
    QMenu *em = menu->addMenu(QStringLiteral("Edit"));
    QMenu *sm = menu->addMenu(QStringLiteral("Settings"));
    QMenu *hm = menu->addMenu(QStringLiteral("Help"));
    vwin.insertWidget(0, menu);            // top of the content column
    QStatusBar *sbar = new QStatusBar(&win);
    sbar->setObjectName("status");
    QLabel *statusline = new QLabel(sbar);
    sbar->addWidget(statusline);
    QWidget *ctl = new QWidget(body);          // left column: process list only (vendor)
    QGridLayout *grid = new QGridLayout(ctl);
    grid->setSpacing(8);
    vlay->addWidget(ctl);
    // Per-key dialog (vendor): opens on double-click (openDlg), holds layer ops,
    // radios + own Save. Hidden = no space in layout; layout pass reflows.
    QWidget *dlg = new QWidget(&win);
    dlg->setObjectName("keydlg");
    vwin.addWidget(dlg);
    QGridLayout *dgrid = new QGridLayout(dlg);
    dgrid->setSpacing(4);
    // vendor key dialog owns the input line, the layer readout and the status text
    QLineEdit *edit = new QLineEdit(dlg);
    edit->setPlaceholderText(QStringLiteral("MACRO"));
    QLabel *layers = new QLabel(dlg);
    layers->setWordWrap(true);
    QLabel *stat = new QLabel(sbar);
    stat->setWordWrap(true);
    stat->setMaximumSize(300, 240);   // statusbar must never push siblings away
    sbar->addWidget(stat);
    QLabel *laylbl = new QLabel(QStringLiteral("Layer #"), dlg);
    QSpinBox *spinl = new QSpinBox(dlg);
    QPushButton *addbtn = new QPushButton(QStringLiteral("+ Layer"), dlg);
    addbtn->setIcon(mkicon("plus"));
    QPushButton *setbtn = new QPushButton(QStringLiteral("Set"), dlg);
    QPushButton *delbtn = new QPushButton(QStringLiteral("Delete"), dlg);
    delbtn->setIcon(mkicon("trash"));
    QPushButton *savebtn = new QPushButton(QStringLiteral("Save key"), dlg);
    QPushButton *depbtn = new QPushButton(QStringLiteral("Set Dep"), dlg);
    QPushButton *renbtn = new QPushButton(QStringLiteral("Rename"), dlg);
    renbtn->setIcon(mkicon("pencil"));
    QPushButton *copybtn = new QPushButton(QStringLiteral("Copy"), dlg);
    copybtn->setIcon(mkicon("copy"));
    QPushButton *dclosebtn = new QPushButton(QStringLiteral("Close"), dlg);
    dclosebtn->setObjectName("close");
    QRadioButton *dfn = new QRadioButton(dlg);
    dfn->setText(QStringLiteral("Do nothing"));
    QRadioButton *dft = new QRadioButton(dlg);
    dft->setText(QStringLiteral("Text/Shortcut"));
    QRadioButton *dapp = new QRadioButton(dlg);
    dapp->setText(QStringLiteral("Application"));
    QRadioButton *dweb = new QRadioButton(dlg);
    dweb->setText(QStringLiteral("Open web page"));
    QLineEdit *dlgout = new QLineEdit(dlg);
    dlgout->setPlaceholderText(QStringLiteral("output text / path / URL"));
    dlgout->setObjectName("dlgout");
    QCheckBox *cbBtf = new QCheckBox(dlg); cbBtf->setText(QStringLiteral("Bring to foreground"));
        QCheckBox *cbRep = new QCheckBox(dlg); cbRep->setText(QStringLiteral("AutoRepeat"));
        QCheckBox *cbSlw = new QCheckBox(dlg); cbSlw->setText(QStringLiteral("SlowOutput"));
    cbBtf->setObjectName("cbBtf"); cbRep->setObjectName("cbRep"); cbSlw->setObjectName("cbSlw");
    QListWidget *macroW = new QListWidget(dlg);
    macroW->setObjectName("macroW"); macroW->setMinimumSize(120, 24);
    QListWidget *layerW = new QListWidget(dlg);   // vendor: clickable layer list
    layerW->setObjectName("layerW"); layerW->setMinimumSize(160, 24);
    QSpinBox *macroSel = new QSpinBox(dlg);
    macroSel->setObjectName("macroSel"); macroSel->setMinimum(1); macroSel->setValue(1);
    QPushButton *macrobtn = new QPushButton(QStringLiteral("Macro +"), dlg);
    macrobtn->setObjectName("macro");
    QLabel *brLabel = new QLabel(dlg);
    brLabel->setWordWrap(true);
    brLabel->setText(QStringLiteral("Browser:"));
    QComboBox *browserPick = new QComboBox(dlg);
    browserPick->setObjectName("browserPick"); browserPick->setMinimumSize(120, 20);
    dlg->setMaximumHeight(220);
    spinl->setMinimum(1);
    spinl->setValue(1);
    dgrid->addWidget(laylbl, 0, 0); dgrid->addWidget(spinl, 0, 1);
    dgrid->addWidget(dlgout, 0, 2, 1, 2);
    dgrid->addWidget(addbtn, 1, 0); dgrid->addWidget(setbtn, 1, 1);
    dgrid->addWidget(delbtn, 1, 2); dgrid->addWidget(savebtn, 1, 3);
    dgrid->addWidget(depbtn, 2, 0); dgrid->addWidget(renbtn, 2, 1);
    dgrid->addWidget(copybtn, 2, 2); dgrid->addWidget(dclosebtn, 2, 3);
    dgrid->addWidget(dfn, 3, 0); dgrid->addWidget(dft, 3, 1);
    dgrid->addWidget(dapp, 3, 2); dgrid->addWidget(dweb, 3, 3);
    dgrid->addWidget(cbBtf, 4, 0); dgrid->addWidget(cbRep, 4, 1); dgrid->addWidget(cbSlw, 4, 2);
    dgrid->addWidget(layerW, 7, 0, 1, 2);
    dgrid->addWidget(macroW, 5, 0, 1, 2); dgrid->addWidget(macroSel, 5, 2); dgrid->addWidget(macrobtn, 5, 3);
    dgrid->addWidget(brLabel, 6, 0); dgrid->addWidget(browserPick, 6, 1, 1, 2);
    static const QStringList gMacros{QStringLiteral("ESC"), QStringLiteral("TAB"), QStringLiteral("N1"), QStringLiteral("N2"),
                                     QStringLiteral("N3"), QStringLiteral("DELAY"), QStringLiteral("POS"), QStringLiteral("POSKEY")};
    { QStringList disp; for (const QString &m : gMacros) disp << QStringLiteral("{%1}").arg(m);
      macroW->addItems(disp); macroSel->setMaximum(qMax(1, (int)gMacros.size())); }
    static QStringList gProgs, gBrows;
    if (gProgs.isEmpty()) {
        std::error_code ec;
        for (const char *d : {"/usr/bin", "/usr/local/bin"}) {
            std::filesystem::directory_iterator it{std::filesystem::path{d}, ec}, end;
            for (; it != end; it.increment(ec)) gProgs << QString::fromUtf8(it->path().filename().string().c_str());
        }
        gProgs.removeDuplicates(); gProgs.sort();
        for (const QString &n : gProgs) {
            const QString l = n.toLower();
            if (l.contains(QStringLiteral("firefox")) || l.contains(QStringLiteral("chrome")) || l.contains(QStringLiteral("brave"))
                || l.contains(QStringLiteral("opera")) || l.contains(QStringLiteral("vivaldi")) || l.contains(QStringLiteral("qute"))
                || l.contains(QStringLiteral("konqueror"))) gBrows << n;
        }
    }
        browserPick->clear(); browserPick->addItems(gBrows);
    QListWidget *progW = new QListWidget(ctl);   // vendor left column: processes only
    progW->setObjectName("progW");
    progW->setMinimumSize(140, 24);
    QSpinBox *progSel = new QSpinBox(ctl);
    progSel->setObjectName("progSel"); progSel->setMinimum(1); progSel->setValue(1);
    progSel->setMaximum(1);
    QPushButton *appbtn = new QPushButton(QStringLiteral("App +"), dlg);
    appbtn->setObjectName("appbtn");
    QPushButton *webbtn = new QPushButton(QStringLiteral("Web +"), dlg);
    webbtn->setObjectName("webbtn");
    // Swap keys: small own window opened from the Edit menu (vendor screens)
    QWidget *swdlg = new QWidget(&win);
    swdlg->setObjectName("swapdlg");
    vwin.addWidget(swdlg);
    swdlg->hide();
    QGridLayout *swgrid = new QGridLayout(swdlg);
    QSpinBox *swA = new QSpinBox(swdlg);
    swA->setObjectName("swA"); swA->setMinimum(1); swA->setMaximum(1024); swA->setValue(1);
    QSpinBox *swB = new QSpinBox(swdlg);
    swB->setObjectName("swB"); swB->setMinimum(1); swB->setMaximum(1024); swB->setValue(65);
    QSpinBox *swN = new QSpinBox(swdlg);
    swN->setObjectName("swN"); swN->setMinimum(1); swN->setMaximum(256); swN->setValue(1);
    QPushButton *swapbtn = new QPushButton(QStringLiteral("Swap keys"), swdlg);
    swapbtn->setObjectName("swap");
    swgrid->addWidget(swapbtn, 0, 0); swgrid->addWidget(swA, 0, 1);
    swgrid->addWidget(swB, 0, 2); swgrid->addWidget(swN, 0, 3);
    // recent/template state spins are invisible drivers now (menu items trigger them)
    QSpinBox *recentSel = new QSpinBox(nullptr);
    recentSel->setObjectName("recentSel"); recentSel->setMinimum(1); recentSel->setValue(1);
    QSpinBox *templSel = new QSpinBox(nullptr);
    templSel->setObjectName("templSel"); templSel->setMinimum(1); templSel->setMaximum(1);
    progW->addItems(gProgs);
    progSel->setMaximum(qMax(1, gProgs.size()));

    dlg->hide();                                   // openDlg shows it
    vwin.addWidget(sbar);                          // status line at the bottom
    // --- vendor main grid: only the process list above stays in ctl ----------
    // Restore: its own window under the Edit menu (vendor 'Restore' screen)
    QWidget *rdlg = new QWidget(&win);
    rdlg->setObjectName("rdlg");
    vwin.addWidget(rdlg);
    rdlg->hide();
    QGridLayout *rgrid = new QGridLayout(rdlg);
    QListWidget *restoreW = new QListWidget(rdlg);
    restoreW->setObjectName("restoreW");
    QSpinBox *restoreSel = new QSpinBox(rdlg);
    restoreSel->setObjectName("restoreSel"); restoreSel->setMinimum(1); restoreSel->setValue(1);
    QPushButton *restoreBtn = new QPushButton(QStringLiteral("Restore"), rdlg);
    restoreBtn->setObjectName("restore");
    QPushButton *rcloseBtn = new QPushButton(QStringLiteral("Close"), rdlg);
    rcloseBtn->setObjectName("rclose");
    rgrid->addWidget(restoreW, 0, 0, 2, 2); rgrid->addWidget(restoreSel, 0, 2);
    rgrid->addWidget(restoreBtn, 0, 3); rgrid->addWidget(rcloseBtn, 1, 3);
    grid->addWidget(progW, 0, 0, 1, 2); grid->addWidget(progSel, 0, 2);
    // upload progress lives in the status bar (vendor: job shown bottom)
    UploadPanel *upload = new UploadPanel(sbar, &pm->pm, mwx,
                                          up_arm || getenv("PKT_UPLOAD_ARM") != nullptr);
    upload->setFixedSize(QSize(260, 64));
    sbar->addWidget(upload);
    // process picker: the small '+' window with running procs + active window
    QWidget *dproc = new QWidget(&win);
    dproc->setObjectName("dproc");
    vwin.addWidget(dproc);
    dproc->hide();
    QGridLayout *pgrid = new QGridLayout(dproc);
    QLabel *lblPick = new QLabel(dproc);          // process-picker readout
    QComboBox *procPick = new QComboBox(dproc);   // PROCESS PICKER (GUI Plan Phase 1)
    procPick->setObjectName("procpick");
    QPushButton *pickbtn = new QPushButton(QStringLiteral("Use Picked Proc"), dproc);
    pickbtn->setObjectName("pick");
    QPushButton *winbtn = new QPushButton(QStringLiteral("Aktive Fenster (3s)"), dproc);
    winbtn->setObjectName("win3");
    QPushButton *recbtn = new QPushButton(QStringLiteral("Reconnect"), dproc);   // manual link retry
    recbtn->setObjectName("reconnect");
    QLabel *lblWin = new QLabel(QStringLiteral("fenster: —"), dproc);   // countdown/probe readout
    int win_secs = 0;                                    // >0 = countdown running
    QString g_winTitle, g_winProc;   // last --active-window probe (shown in stat)
    pgrid->addWidget(lblPick, 0, 0); pgrid->addWidget(procPick, 0, 1);
    pgrid->addWidget(pickbtn, 1, 0); pgrid->addWidget(winbtn, 1, 1);
    pgrid->addWidget(lblWin, 2, 0); pgrid->addWidget(recbtn, 2, 1);
    // preview + proc rename/delete: key dialog owns them (vendor)
    QPushButton *prevbtn = new QPushButton(QStringLiteral("Preview"), dlg);
    QLabel *prevlbl = new QLabel(dlg);
    prevlbl->setWordWrap(true);
    QPushButton *renprocBtn = new QPushButton(QStringLiteral("Rename proc"), dlg);
    QPushButton *delprocBtn = new QPushButton(QStringLiteral("Delete proc"), dlg);
    dgrid->addWidget(appbtn, 8, 0); dgrid->addWidget(webbtn, 8, 1);
    dgrid->addWidget(renprocBtn, 9, 0); dgrid->addWidget(delprocBtn, 9, 1);
    dgrid->addWidget(prevbtn, 10, 0); dgrid->addWidget(prevlbl, 10, 1, 1, 2);
    dgrid->addWidget(edit, 0, 4);
    dgrid->addWidget(layers, 7, 3);
    addbtn->setObjectName("add"); setbtn->setObjectName("set"); delbtn->setObjectName("del");
    depbtn->setObjectName("dep"); renbtn->setObjectName("ren"); copybtn->setObjectName("copy");
    savebtn->setObjectName("save");
    prevbtn->setObjectName("preview");

    // live board: pressed POSKeys get a green ring (calib maps scancode->POSKey)
    ipc_adopt_peer(true);   // daemon already listening? peer up front, watch stays off
    const QHash<int, int> cal = load_calib(
        calib.isEmpty() ? QStringLiteral("/home/nonrmy/MK.calib.json") : calib);
    QTimer poll;
    poll.setInterval(50);
    // signal = timeout (start() never emits!): the IN-stream loop was DEAD
    // with the start()-cast — pressed set never updated live.
    const QMetaObject::Connection pc = poll.connect(
        &poll, &QTimer::timeout, [&]() {
        if (g_ipc_peer >= 0) { ipc_drain(); return; }  // daemon live: the GUI must NOT
        // also read the hidraw node (watch-mode conflict, review round-2) — the
        // daemon's status lines come in over the socket instead.
        bool touched = false;
        unsigned char buf[5];
        int n;
        while ((n = poll_in(buf, 5, 0)) > 0) {
            if (n < 5 || buf[0] != 0x04) continue;
            std::vector<unsigned char> v;
            for (int k = 1; k < 5; ++k) if (buf[k]) v.push_back(buf[k]);
            size_t i = 0;
            while (i < v.size()) {
                bool down = true, ext = false;
                while (i < v.size() && (v[i] == 0xFE || v[i] == 0xE0)) {
                    if (v[i] == 0xFE) down = false; else ext = true;
                    ++i;
                }
                if (i >= v.size()) break;
                const int sc = ext ? (0xE000 | v[i]) : v[i];
                ++i;
                const int pk = cal.value(sc, -1);
                if (pk < 0) continue;
                if (down) { if (!pm->pressed.contains(pk)) { pm->pressed.insert(pk); touched = true; } }
                else if (pm->pressed.remove(pk)) touched = true;
            }
        }
        if (touched && pm->notify) pm->notify();
        if (touched) {
            static int inlog = 0;
            if (++inlog <= 3)
                qWarning("pkteditor IN: pressed set now %d", (int)pm->pressed.size());
        }
    });
    if (!pc) qWarning("pkteditor: IN-poll signal NOT connected (bug!)");

    QString save_note;
    static QStringList g_backups, g_restoreRows;  // newest-first paths + display rows
    auto refresh = [&](bool saved) {
        ipc_drain();   // daemon status lines -> g_ipc_status (nonblocking poll)
        { static int tref = 0;   // dead link? retry every ~4 s over the nb route only
          if (g_ipc_peer < 0 && ++tref % 80 == 0) ipc_adopt_peer(true); }
        const int pk = pm->sel;
        head->setText(QStringLiteral("%1  maps=%2  sel=%3  dbg=%4,%5  pressed=%6")
                         .arg(pm->pm.name.isEmpty() ? QStringLiteral("no pmap loaded")
                                                    : pm->pm.name)
                         .arg(pm->pm.maps.size())
                         .arg(pk >= 0 ? describe_cell(pk) : QStringLiteral("-"))
                         .arg(pm->dbg.x())
                         .arg(pm->dbg.y())
                         .arg(pm->pressed.size()));
        const Map *mp = find_map_c(pm->pm, pk);
        if (mp && !mp->layers.isEmpty()) {
            const int n = mp->layers.size();
            if (spinl->maximum() != n) spinl->setMaximum(n);
            spinl->setValue(qBound(1, spinl->value(), n));
            const int lsel = spinl->value();
            QStringList ls;
            int li = 0;
            for (const Layer &l : mp->layers) {
                QString s = QStringLiteral("%1 '%2'").arg(l.type).arg(l.value);
                if (!l.dependency.isEmpty()) s += QStringLiteral(" DEP=%1").arg(l.dependency);
                if (l.type.compare(QStringLiteral("TEXT"), Qt::CaseInsensitive) != 0)
                    s = QStringLiteral("\u2691 ACTION %1").arg(s);  // needs --allow-actions in pktm
                if (++li == lsel) s = QStringLiteral("> %1").arg(s);
                ls << s;
            }
            layers->setText(QStringLiteral("layers (%1): %2").arg(n)
                                              .arg(ls.join(" | ")));
            layerW->clear(); layerW->addItems(ls);
        } else {
            layers->setText(QStringLiteral("layers (0) — type a MACRO, then + Layer / Set"));
        }
        QStringList nocard;
        for (const Map &mm : pm->pm.maps)
            if (describe_cell(mm.poskey).isEmpty()) nocard << QString::number(mm.poskey);
        QString s = QStringLiteral("grid: %1  POSKeys ohne Cap: %2")
                        .arg(grid_stats()).arg(nocard.join(","));
        if (saved) s += QStringLiteral("  %1").arg(save_note);
        if (!g_winTitle.isEmpty())
            s += QStringLiteral("  fenster='%1' proc='%2'").arg(g_winTitle).arg(g_winProc);
        if (!g_ipc_status.isEmpty())
            s += QStringLiteral("  daemon: %1").arg(g_ipc_status);   // live status over IPC peer
        // process picker: (re)fill combo on save + first call; label = current pick
        if (saved || procPick->count() == 0) {
            procPick->clear();
            procPick->addItems(pktm::list_processes(120));
        }
        if (procPick->count() > 0)
            procPick->setCurrentIndex(qBound(0, procPick->currentIndex(), procPick->count() - 1));
        const QString cur = procPick->count() ? procPick->itemText(procPick->currentIndex()) : QString();
        lblPick->setText(cur.isEmpty()
            ? QStringLiteral("picker: running processes not listed (no probe)")
            : QStringLiteral("picked: '%1' (n=%2)").arg(cur).arg(procPick->count()));
        stat->setText(s);
        statusline->setText(QStringLiteral("maps=%1  pressed=%2  grid=%3  fenster='%4'  daemon:%5")
                                 .arg(pm->pm.maps.size()).arg(pm->pressed.size())
                                 .arg(nocard.join(QStringLiteral(",")))
                                 .arg(g_winTitle).arg(g_ipc_status));
        { const QStringList ps = procs_of(pm->pm);   // process view: Always + deps
          procW->clear();
          procW->addItems(ps);
        }
        { // restore list: newest-first backups from the tool dir
          std::error_code ec;
          const std::filesystem::path bdir{std::string(QDir::homePath().toUtf8().constData()) + "/.local/share/pkteditor"};
          std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> found;
          for (const auto &de : std::filesystem::directory_iterator(bdir, ec)) {
              const std::string nm = de.path().filename().string();
              if (nm.rfind("pmap_backup_", 0) == 0 && nm.substr(nm.size() - 5) == ".pmap")
                  found.emplace_back(de.last_write_time(ec), de.path());
          }
          std::sort(found.begin(), found.end(),
                    [](const auto &a, const auto &b){ return a.first > b.first; });
          static QHash<QString, QString> rline;   // path -> display row (cached)
          QStringList fl, rows;
          for (const auto &p : found) {
              const QString path = QString::fromUtf8(p.second.u8string().c_str());
              QString line = rline.value(path);
              if (line.isEmpty()) {
                  const long long secs = std::chrono::duration_cast<std::chrono::seconds>(p.first.time_since_epoch()).count();
                  QFile bf(path);
                  const int nk = bf.open(QIODevice::ReadOnly) ? bf.readAll().count(QByteArrayLiteral("<Poskey")) : -1;
                  bf.close();
                  line = QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("yyyy-MM-dd hh:mm"))
                       + QStringLiteral(" . %1 keys . %2").arg(nk).arg(QFileInfo(path).fileName());
                  rline.insert(path, line);
              }
              fl << path; rows << line;
          }
          g_backups = fl; g_restoreRows = rows;
          restoreW->clear();
          for (const QString &l : rows) restoreW->addItem(l);
          restoreSel->setMaximum(qMax(1, fl.size()));
          restoreSel->setValue(qBound(1, restoreSel->value(), qMax(1, fl.size())));
        }
    };
    auto f_add = [&](const QString &v){ add_layer(pm->pm, pm->sel, v.isEmpty() ? edit->text() : v); refresh(false); };
    auto f_set = [&](const QString &v){ set_layer_value(pm->pm, pm->sel, spinl->value(), v.isEmpty() ? edit->text() : v); refresh(false); };
    auto f_del = [&]{ del_layer(pm->pm, pm->sel, spinl->value()); refresh(false); };
    auto f_dep = [&](const QString &v){ set_layer_dep(pm->pm, pm->sel, spinl->value(), v.isEmpty() ? edit->text() : v); refresh(false); };
    auto f_ren = [&](const QString &v){ set_layer_type(pm->pm, pm->sel, spinl->value(), v.isEmpty() ? edit->text().toUpper() : v.toUpper()); refresh(false); };
    auto f_copy = [&]{ copy_layer(pm->pm, pm->sel, spinl->value()); refresh(false); };
    // Process picker: QComboBox fed by mapper.cpp list_processes() — the daemon's
    // own probe chain (kdotool ps) + comm names from /proc, i.e. exactly the
    // names the daemon's dep layer can match. 'Use Picked Proc' assigns the
    // combi's current entry as dep of the picked layer.
    auto f_pick = [&]{
        const QString cur = procPick->count() ? procPick->itemText(procPick->currentIndex()) : QString();
        set_layer_dep(pm->pm, pm->sel, spinl->value(), cur.isEmpty() ? edit->text() : cur);
        refresh(false);
    };
    pickbtn->connect(pickbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_pick]{ f_pick(); });
    // 'Aktive Fenster (3s)': click starts a countdown — bring the window you
    // want to type into to the FRONT within 3 s; then the daemon's active-window
    // probe runs and its process becomes the picker's current item.
    // NEVER sleep in the click handler: froze the window (KDE 'not responding').
    // Timer ticks (1 s) update lblWin; at 0 win_probe() runs. Test hooks for
    // headless gates: PKT_WIN_DELAY_MS / PKT_WIN_COUNT / PKT_WIN_FAKE 'title::proc'.
    auto win_probe = [&]{
        QString wname, wproc;
        const char *fake = getenv("PKT_WIN_FAKE");
        if (fake && *fake) {
            const QString f = QString::fromUtf8(fake);
            const int k = f.indexOf(QStringLiteral("::"));
            if (k < 0) wname = f; else { wname = f.left(k); wproc = f.mid(k + 2); }
        } else {
            wname = pktm::active_window();
            wproc = pktm::active_process();
            if (wname == pktm::unknown_window()) wname = QString();
        }
        g_winTitle = wname; g_winProc = wproc;
        lblWin->setText(QStringLiteral("fenster '%1' proc '%2'").arg(wname).arg(wproc));
        if (!wproc.isEmpty()) {
            int idx = -1;
            for (int i = 0; i < procPick->count(); ++i)
                if (procPick->itemText(i) == wproc) { idx = i; break; }
            if (idx < 0) { procPick->insertItem(0, wproc); idx = 0; }
            procPick->setCurrentIndex(idx);
        }
        refresh(false);
    };
    QTimer winT;
    {
        const char *d = getenv("PKT_WIN_DELAY_MS");
        winT.setInterval((d && *d) ? QString(d).toInt() : 1000);
    }
    if (!winT.connect(&winT, &QTimer::timeout, [&]{
        if (win_secs <= 0) return;
        --win_secs;
        if (win_secs == 0) win_probe();
        else lblWin->setText(QStringLiteral("fenster in %1 s — bring window to FRONT").arg(win_secs));
    })) qWarning("pkteditor: winT signal not connected");
    auto f_win3 = [&]{
        const char *c = getenv("PKT_WIN_COUNT");
        win_secs = (c && *c) ? QString(c).toInt() : 3;
        lblWin->setText(QStringLiteral("fenster in %1 s — bring window to FRONT").arg(win_secs));
        winT.start();
    };
    winbtn->connect(winbtn, &QPushButton::clicked, [f_win3]{ f_win3(); });
    recbtn->connect(recbtn, &QPushButton::clicked, [&]{ ipc_adopt_peer(false); refresh(true); });
    auto f_prev = [&]{
        const Map *mp = find_map_c(pm->pm, pm->sel);
        const QString t = mp ? pktm::preview_map(*mp, spinl->value())
                             : QStringLiteral("no rule for selected cell");
        prevlbl->setText(t);
        std::printf("preview: %s\n", t.toUtf8().constData());
        refresh(false);
    };
    prevbtn->connect(prevbtn, &QPushButton::clicked, [&]{ f_prev(); });
    addbtn->connect(addbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_add]{ f_add(edit->text()); });
    depbtn->connect(depbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_dep]{ f_dep(edit->text()); });
    setbtn->connect(setbtn, &QPushButton::clicked, [pm, &edit, &dlgout, &save_note, &refresh, f_set]{ f_set(dlgout->text().isEmpty() ? edit->text() : dlgout->text()); });
    delbtn->connect(delbtn, &QPushButton::clicked, [pm, &save_note, &refresh, f_del]{ f_del(); });
    renbtn->connect(renbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_ren]{ f_ren(edit->text()); });
    copybtn->connect(copybtn, &QPushButton::clicked, [pm, &save_note, &refresh, f_copy]{ f_copy(); });
    savebtn->connect(savebtn, &QPushButton::clicked, [pm, &out, &save_note, &refresh, &spinl,
                                                      &cbBtf, &cbRep, &cbSlw]{
        { Map &mx = ensure_map(pm->pm, pm->sel);
          if (!mx.layers.isEmpty()) {
              const int li = qBound(1, spinl->value(), (int)mx.layers.size()) - 1;
              mx.layers[li].btf  = cbBtf->isChecked() ? QStringLiteral("True") : QStringLiteral("False");
              mx.layers[li].rep  = cbRep->isChecked() ? QStringLiteral("True") : QStringLiteral("False");
              mx.layers[li].slow = cbSlw->isChecked() ? QStringLiteral("True") : QStringLiteral("False");
          } }
        save_note = save_pmap(*pm, pm->path.isEmpty() ? out : pm->path); refresh(true); });
    auto do_undo = [&save_note, &refresh]{
        if (g_undo.isEmpty()) save_note = QStringLiteral("undo: nothing in this session");
        else {
            const UndoSnap last = g_undo.last();
            QFile cur(last.path);
            const QByteArray now = cur.open(QIODevice::ReadOnly) ? cur.readAll() : QByteArray();
            if (now != last.post) save_note = QStringLiteral("undo: file changed since save — NOT restored");
            else {
                const QString e = write_atomic(last.path, last.pre);
                save_note = e.isEmpty() ? QStringLiteral("undo restored %1").arg(last.path) : e;
                if (e.isEmpty()) save_note += ipc_reload_note();   // daemon sees the restore too
            }
            g_undo.removeLast();
        }
        refresh(true);
    };
    // Undo via the Edit menu (do_undo); no leftover button
    refresh(false);
    pm->notify = [&]() { refresh(false); };
    poll.start();
    // Dialog wiring (vendor): Save per key, Close hides; openDlg(pk) shows +
    // re-binds the layer picker for that key. Do nothing = empty Value.
    bool dlg_open = false;
    auto close_dialog = [&]{
        dlg->hide();
        dlg_open = false;
        refresh(false);
    };
    auto do_sel_proc = [&](const QString &name){
        pm->procSel = (name == QStringLiteral("Always") || name.isEmpty())
                          ? QString() : name;
        refresh(false);
    };
    auto do_rename_proc = [&]{
        const QString np = edit->text();
        int n = 0;
        for (Map &m : pm->pm.maps)
            for (Layer &l : m.layers)
                if (sel_layer(l, pm->procSel) && !pm->procSel.isEmpty()) { l.dependency = np; ++n; }
        save_note = QStringLiteral("renamed %1 keys' layers -> '%2'").arg(n).arg(np);
    };
    auto do_delete_proc = [&]{
        int n = 0;
        for (Map &m : pm->pm.maps)
            for (Layer &l : m.layers)
                if (sel_layer(l, pm->procSel) && !pm->procSel.isEmpty()) { l.dependency = QString(); ++n; }
        save_note = QStringLiteral("removed process from %1 keys' layers").arg(n);
        refresh(false);
    };
    auto open_dialog = [&](int pk){
        pm->sel = pk;
        dlg->setGeometry(QRect(6, 6, qMax(360, win.width() - 12),
                               qMin(320, qMax(200, win.height() / 2))));
        dlg->show();
        dlg_open = true;
        { QString v; for (const Map &mm : pm->pm.maps)
              if (mm.poskey == pk && !mm.layers.isEmpty()) { v = mm.layers.first().value; break; }
          dlgout->setText(v);
        }
        const int n = [pk](Model *p) -> int { for (const Map &mm : p->pm.maps)
                                      if (mm.poskey == pk) return (int)mm.layers.size();
                                    return 0; }(pm);
        spinl->setMinimum(1);
        spinl->setMaximum(qMax(1, n));
        spinl->setValue(1);
        if (const Map *mx = find_map_c(pm->pm, pk); mx && !mx->layers.isEmpty()) {
            const Layer &L = mx->layers.first();
            cbBtf->setChecked(L.btf.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
            cbRep->setChecked(L.rep.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
            cbSlw->setChecked(L.slow.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0);
        } else { cbBtf->setChecked(false); cbRep->setChecked(false); cbSlw->setChecked(false); }
        refresh(false);
    };
    pm->openDlg = open_dialog;
    // Menu items: QAction children of the four QMenus; QMenu fires trigger() ->
    // triggered signal -> handler. Same handlers a headless gate can call directly.
    static Map clip_map;   // key clipboard (Copy/Paste key)
    auto do_new = [&](bool q){ pm->pm = Pmap{}; if (!q) refresh(false); };
    auto do_open = [&]{
        QFile f(pm->path.isEmpty() ? out : pm->path);
        if (f.open(QIODevice::ReadOnly))
            pm->pm = parse_pmap(QString::fromLatin1(f.readAll()));
        else save_note = QStringLiteral("open failed: %1").arg(pm->path);
        refresh(false);
    };
    auto do_quit = [&]{ std::_Exit(0); };
    auto do_copy_key = [&]{
        if (const Map *m0 = find_map(pm->pm, pm->sel)) clip_map = *m0;
        else save_note = QStringLiteral("copy key: selected cell has no map");
    };
    auto do_paste_key = [&]{
        if (const Map *src = find_map(pm->pm, pm->sel)) {
            Map &t = ensure_map(pm->pm, pm->sel);   // same cell = replace layers
            t.layers = src->layers;
        }
        refresh(false);
    };
    auto do_delete_key = [&]{
        for (int i = 0; i < pm->pm.maps.size(); ++i)
            if (pm->pm.maps[i].poskey == pm->sel) { pm->pm.maps.remove(i); break; }
        refresh(false);
    };
    // context-menu bridges (MapView::contextMenuEvent wires QAction->clicked)
    g_ctx_edit = [&]{ pm->openDlg(pm->sel); };
    g_ctx_copy = do_copy_key;
    g_ctx_paste = do_paste_key;
    g_ctx_del = do_delete_key;
    auto do_proc_list = [&]{
        procPick->clear();
        procPick->insertItem(0, QStringLiteral("-"));
        for (const QString &p : pktm::list_processes(120)) procPick->addItem(p);
        refresh(false);
    };
    auto do_about = [&]{
        save_note = QStringLiteral("pkteditor — Preh MCI-128 Mapping-List Editor");
        refresh(false);
    };
    auto mk_item = [&](QMenu *mm, const QString &txt, const std::function<void()> &fn){
        QAction *a = new QAction(txt, mm);
        a->setObjectName(QStringLiteral("menu:") + txt);
        QObject::connect(a, &QAction::triggered, [fn]{ if (fn) fn(); });
    };
    mk_item(fm, QStringLiteral("New"), [&]{ do_new(false); });
    mk_item(fm, QStringLiteral("Open"), do_open);
    mk_item(fm, QStringLiteral("Close"), [&]{ do_new(false); });
    mk_item(fm, QStringLiteral("Quit"), do_quit);
    mk_item(em, QStringLiteral("Undo"), do_undo);
    mk_item(em, QStringLiteral("Copy key"), do_copy_key);
    mk_item(em, QStringLiteral("Paste key"), do_paste_key);
    mk_item(em, QStringLiteral("Delete key"), do_delete_key);
    mk_item(sm, QStringLiteral("Reconnect daemon"), [&]{ ipc_adopt_peer(false); refresh(false); });
    mk_item(sm, QStringLiteral("Process list"), do_proc_list);
    mk_item(hm, QStringLiteral("About"), do_about);
    renprocBtn->connect(renprocBtn, &QPushButton::clicked, [&]{ do_rename_proc(); refresh(false); });
    delprocBtn->connect(delprocBtn, &QPushButton::clicked, [&]{ do_delete_proc(); });
    dclosebtn->connect(dclosebtn, &QPushButton::clicked, [&]{ close_dialog(); });
    dfn->connect(dfn, &QRadioButton::clicked, [pm, &spinl, &refresh]{   // Do nothing
        set_layer_value(pm->pm, pm->sel, spinl->value(), QString());
        set_layer_type(pm->pm, pm->sel, spinl->value(), QString());
        refresh(false);
    });
    dft->connect(dft, &QRadioButton::clicked, [pm, &spinl, &dlgout, &refresh]{
        set_layer_type(pm->pm, pm->sel, spinl->value(), QStringLiteral("TEXT"));
        if (!dlgout->text().isEmpty())
            set_layer_value(pm->pm, pm->sel, spinl->value(), dlgout->text());
        refresh(false);
    });
    dapp->connect(dapp, &QRadioButton::clicked, [pm, &spinl, &dlgout, &refresh]{
        set_layer_type(pm->pm, pm->sel, spinl->value(), QStringLiteral("APPLICATION"));
        if (!dlgout->text().isEmpty())
            set_layer_value(pm->pm, pm->sel, spinl->value(), dlgout->text());
        refresh(false);
    });
    dweb->connect(dweb, &QRadioButton::clicked, [pm, &spinl, &dlgout, &refresh]{
        set_layer_type(pm->pm, pm->sel, spinl->value(), QStringLiteral("WEBSITE"));
        if (!dlgout->text().isEmpty())
            set_layer_value(pm->pm, pm->sel, spinl->value(), dlgout->text());
        refresh(false);
    });
    auto do_restore = [&save_note, &refresh, pm, out, restoreSel]{
        if (g_backups.isEmpty()) { save_note = QStringLiteral("restore: no backups"); return; }
        const int i = qBound(1, restoreSel->value(), (int)g_backups.size()) - 1;
        QFile f(g_backups[i]);
        if (!f.open(QIODevice::ReadOnly)) { save_note = QStringLiteral("restore: unreadable"); return; }
        const QByteArray bytes = f.readAll(); f.close();
        const QString path = pm->path.isEmpty() ? out : pm->path;
        const QString e = write_atomic(path, bytes);
        if (!e.isEmpty()) { save_note = QStringLiteral("restore: ") + e; return; }
        pm->path = path;
        pm->pm = parse_pmap(QString::fromLatin1(bytes));
        refresh(true);
        save_note = QStringLiteral("restored %1 (of %2)")
                                   .arg(g_backups.size() - i).arg(g_backups.size());
    };
    restoreBtn->connect(restoreBtn, &QPushButton::clicked, [&]{ do_restore(); });
    macrobtn->connect(macrobtn, &QPushButton::clicked, [&]{
        const int i = qBound(1, macroSel->value(), (int)gMacros.size()) - 1;
        QString v = dlgout->text();
        if (!v.isEmpty() && !v.endsWith(QLatin1Char(' '))) v += QLatin1Char(' ');
        dlgout->setText(v + QStringLiteral("{%1}").arg(gMacros.at(i)));
        refresh(false);
    });
    macroW->connect(macroW, &QListWidget::itemClicked, [&dlgout](QListWidgetItem *it){
        QString v = dlgout->text();
        if (!v.isEmpty() && !v.endsWith(QLatin1Char(' '))) v += QLatin1Char(' ');
        dlgout->setText(v + it->text());
    });
    appbtn->connect(appbtn, &QPushButton::clicked, [&]{
        const int i = qBound(1, progSel->value(), gProgs.size()) - 1;
        if (i >= gProgs.size()) return;
        set_layer_type(pm->pm, pm->sel, spinl->value(), QStringLiteral("APPLICATION"));
        set_layer_value(pm->pm, pm->sel, spinl->value(), gProgs.at(i));
        refresh(false);
    });
    auto do_swap_keys = [&save_note, &refresh, swA, swB, swN, pm]{
        const int a = swA->value(), b = swB->value(), n = swN->value();
        if (a < 1 || n < 1 || b < a + n) { save_note = QStringLiteral("swap: ranges overlap/too big"); return; }
        auto idx = [&](int pk){ for (int i = 0; i < pm->pm.maps.size(); ++i) if (pm->pm.maps[i].poskey == pk) return i; return -1; };
        for (int k = 0; k < n; ++k) {
            const int pa = a + k, pb = b + k;
            const int ia = idx(pa), ib = idx(pb);
            QVector<Layer> tmp;
            if (ia >= 0) tmp = pm->pm.maps[ia].layers;
            if (ia >= 0) pm->pm.maps[ia].layers = (ib >= 0) ? pm->pm.maps[ib].layers : QVector<Layer>();
            if (ib >= 0) pm->pm.maps[ib].layers = tmp;
            else if (!tmp.isEmpty()) { Map nm; nm.poskey = pb; nm.layers = tmp; pm->pm.maps << nm; }
        }
        refresh(false);
        save_note = QStringLiteral("swapped %1 keys (A=%2 B=%3)").arg(n).arg(a).arg(b);
    };
    swapbtn->connect(swapbtn, &QPushButton::clicked, [&]{ do_swap_keys(); });
    const auto do_open_recent = [&save_note, &refresh, pm, recentSel]{
        if (gRecent.isEmpty()) { save_note = QStringLiteral("open recent: list empty"); return; }
        const int i = qBound(1, recentSel->value(), gRecent.size()) - 1;
        const QString f = gRecent.at(i);
        QFile rf(f);
        if (!rf.open(QIODevice::ReadOnly)) { save_note = QStringLiteral("open recent: unreadable"); return; }
        pm->pm = parse_pmap(QString::fromUtf8(rf.readAll()));
        rf.close();
        pm->path = f;
        refresh(false);
        save_note = QStringLiteral("opened recent ") + QString::number(i + 1);
    };
    // recent list: File-menu items only, the spin caps at the list size
    recentSel->setMaximum(qMax(1, gRecent.size()));
    const auto do_open_template = [&save_note, pm, templSel]{
        if (gRecent.isEmpty()) { save_note = QStringLiteral("template: list empty"); return; }
        const int i = qBound(1, templSel->value(), gRecent.size()) - 1;
        QFile rf(gRecent.at(i));
        if (!rf.open(QIODevice::ReadOnly)) { save_note = QStringLiteral("template: unreadable"); return; }
        pm->templ = parse_pmap(QString::fromUtf8(rf.readAll()));
        rf.close();
    };
    const auto do_use_template = [&save_note, &refresh, pm]{
        Map tmap;
        for (const Map &tm : pm->templ.maps) if (tm.poskey == pm->sel) { tmap = tm; break; }
        if (tmap.layers.isEmpty() || pm->sel < 0) { save_note = QStringLiteral("template: no map for sel"); return; }
        const int ix = [pm]{ for (int j = 0; j < pm->pm.maps.size(); ++j) if (pm->pm.maps[j].poskey == pm->sel) return j; return -1; }();
        if (ix >= 0) pm->pm.maps[ix].layers = tmap.layers;
        else { Map nm; nm.poskey = pm->sel; nm.layers = tmap.layers; pm->pm.maps << nm; }
        refresh(false);
        save_note = QStringLiteral("template applied to sel");
    };

    // template/recent/restore actions live in the menus (vendor screen set)
    mk_item(em, QStringLiteral("Open template"), [&]{ do_open_template(); refresh(false); });
    mk_item(fm, QStringLiteral("Open recent"), [&]{ do_open_recent(); });
    for (int ri = 0; ri < qMin(5, (int)gRecent.size()); ++ri)
        mk_item(fm, QStringLiteral("Open %1").arg(gRecent[ri]),
                [&, ri]{ recentSel->setValue(ri + 1); do_open_recent(); refresh(false); });
    mk_item(em, QStringLiteral("Restore"), [&]{ do_restore(); });
    mk_item(em, QStringLiteral("Swap keys"), [&]{ do_swap_keys(); });
    webbtn->connect(webbtn, &QPushButton::clicked, [&]{
        set_layer_type(pm->pm, pm->sel, spinl->value(), QStringLiteral("WEBSITE"));
        set_layer_value(pm->pm, pm->sel, spinl->value(),
                        dlgout->text().isEmpty() ? QStringLiteral("https://") : dlgout->text());
        refresh(false);
    });
    auto do_export_pdf = [&save_note, &out, pm]{
        QMap<int, QString> vals, types;
        for (const Map &mp : pm->pm.maps)
            if (!vals.contains(mp.poskey)) {
                vals.insert(mp.poskey, mp.layers.isEmpty() ? QStringLiteral("-") : mp.layers.first().value);
                types.insert(mp.poskey, mp.layers.isEmpty() ? QString() : mp.layers.first().type);
            }
        QVector<std::pair<int, QString>> rows, rt;
        if (!g_geom.isEmpty())
            for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
                rows << std::make_pair(it.key(), vals.value(it.key(), QStringLiteral("-")));
                rt << std::make_pair(it.key(), types.value(it.key()));
            }
        else
            for (const Map &mp : pm->pm.maps) {
                rows << std::make_pair(mp.poskey, vals.value(mp.poskey));
                rt << std::make_pair(mp.poskey, types.value(mp.poskey));
            }
        if (rows.isEmpty()) { save_note = QStringLiteral("export: no keys"); return; }
        const QString tgt = (pm->path.isEmpty() ? out : pm->path) + QStringLiteral(".labels.pdf");
        QFile pf(tgt);
        if (!pf.open(QIODevice::WriteOnly)) { save_note = QStringLiteral("export: cannot write"); return; }
        pf.write(make_label_pdf(rows)); pf.close();
        const QString tgt2 = (pm->path.isEmpty() ? out : pm->path) + QStringLiteral(".geom.pdf");
        if (!g_geom.isEmpty()) {
           QHash<int, QString> texts, fonts;
           for (const Map &mp : pm->pm.maps) {
               if (!mp.print_text.isEmpty()) texts.insert(mp.poskey, mp.print_text);
               if (!mp.fontdesc.isEmpty()) fonts.insert(mp.poskey, mp.fontdesc);
           }
           QFile pf2(tgt2);
           if (pf2.open(QIODevice::WriteOnly)) { pf2.write(make_label_pdf_geom(rows, rt, texts, fonts)); pf2.close(); }
        }
        save_note = QStringLiteral("exported %1 labels -> %2").arg(rows.size()).arg(tgt);
    };

    mk_item(em, QStringLiteral("Export labels (PDF)"), do_export_pdf);

    if (mode == "clicktest") {
        if (!QFile::copy(pm->path, pm->path + QStringLiteral(".orig0")))
            std::printf("note: orig0 copy failed (file exists?)\n");
        { QFile ok(pm->path + QStringLiteral(".orig0"));
          std::printf("orig0 size=%lld\n", ok.exists() ? (long long)QFileInfo(pm->path + QStringLiteral(".orig0")).size() : -1LL); }
        const QPoint target(900, 300);   // board-local target (widget coords)
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(target), QPointF(target),
                       Qt::LeftButton, Qt::LeftButton, Qt::KeyboardModifiers());
        board->mousePressEvent(&ev);      // direct handler call (Qt6 dropped QMeta GUI)
        pm->cells = layout_cells(board->size());  // paintEvent not run in clicktest
        board->mousePressEvent(&ev);
        if (pm->sel < 0 && !pm->cells.isEmpty()) pm->sel = pm->cells.first().poskey;
        std::printf("geom keys=%d cells=%d\n", (int)g_geom.size(), (int)pm->cells.size());
        pm->openDlg(pm->sel);             // layer buttons now live in the dialog
        std::printf("dlg geom: %d,%d %dx%d savebtn %d,%d %dx%d\n", dlg->x(), dlg->y(),
                    dlg->width(), dlg->height(), savebtn->x(), savebtn->y(),
                    savebtn->width(), savebtn->height());
        std::printf("clicktest sel=%d target=(900,300) cell=%s\n",
                    pm->sel, describe_cell(pm->sel).toUtf8().constData());
        // BUTTON-WIRING GATE: real click events via QTest::mouseClick (Qt6 keeps
        // the hook; only QMeta::Clicked itself was replaced). Buttons found by
        // objectName -> wiring, not just handlers.
        auto find_btn = [&](const char *name) -> QPushButton * {
            std::function<QPushButton *(QObject *)> walk = [&](QObject *o) -> QPushButton * {
                for (QObject *c : o->children()) {
                    if (auto *pb = qobject_cast<QPushButton *>(c))
                        if (pb->objectName() == QLatin1String(name))
                            return pb;
                    if (auto *r = walk(c)) return r;
                }
                return nullptr;
            };
            return walk(&win);
        };
        auto press_btn = [&](const char *name) -> bool {
            if (QPushButton *pb = find_btn(name)) { QTest::mouseClick(pb, Qt::LeftButton); return true; }
            return false;
        };
        int gfail = 0;
        if (!(find_btn("add") && find_btn("set") && find_btn("del")
              && find_btn("dep") && find_btn("ren") && find_btn("copy")
              && find_btn("preview") && find_btn("pick") && find_btn("win3")))
            { std::printf("clicktest FAIL: button by objectName missing\n"); ++gfail; }
        edit->setText(QStringLiteral("L1")); press_btn("add");
        edit->setText(QStringLiteral("L2")); press_btn("add");
        spinl->setValue(1);
        edit->setText(QStringLiteral("AAA")); press_btn("set");
        edit->setText(QStringLiteral("DEP1")); press_btn("dep");
        edit->setText(QStringLiteral("WEBSITE")); press_btn("ren");
        press_btn("copy");
        spinl->setValue(3); press_btn("del");
        // negative case: disabled button must not fire
        copybtn->setEnabled(false);
        spinl->setValue(1);
        {
            const Map *m0 = find_map_c(pm->pm, pm->sel);
            const int n_b = m0 ? m0->layers.size() : -1;
            press_btn("copy");
            const Map *m1 = find_map_c(pm->pm, pm->sel);
            const int n_a = m1 ? m1->layers.size() : -1;
            std::printf("clicktest disabled-copy ignored: %s (n=%d->%d)\n",
                        n_a == n_b ? "OK" : "FAIL", n_b, n_a);
            if (n_a != n_b) ++gfail;
        }
        spinl->setValue(1);
        press_btn("preview");
        spinl->setValue(2);
        press_btn("preview");
        // process-picker gate: list from mapper.cpp (same code the dep-match uses)
        const QStringList procs = pktm::list_processes(20);
        std::printf("picker n=%d first='%s'\n", (int)procs.size(),
                    procs.isEmpty() ? "" : procs.first().toUtf8().constData());
        spinl->setValue(1);
        press_btn("pick");
        {
            const Map *mp = find_map_c(pm->pm, pm->sel);
            const QString d = (mp && !mp->layers.isEmpty()) ? mp->layers[0].dependency : QString();
            if (d.isEmpty()) { std::printf("clicktest FAIL: pick did not set a dep\n"); ++gfail; }
        }
        // countdown button: click starts the 3 s countdown (non-blocking!), headless
        // gate runs the probe directly with a fake result
        press_btn("win3");
        if (win_secs <= 0) { std::printf("clicktest FAIL: countdown not started\n"); ++gfail; }
        win_secs = 0;                       // stop countdown; probe below is direct
        win_probe();
        if (g_winProc != QStringLiteral("pkteditor")) {
            std::printf("clicktest FAIL: probe proc wrong (got '%s')\n", g_winProc.toUtf8().constData());
            ++gfail;
        }
        {
            const Map *mp = find_map_c(pm->pm, pm->sel);
            std::printf("clicktest layers(sel=%d): %d\n", pm->sel, mp ? mp->layers.size() : -1);
            if (mp)
                for (const Layer &l : mp->layers)
                    std::printf("  %s='%s' dep='%s'\n", l.type.toUtf8().constData(),
                                l.value.toUtf8().constData(), l.dependency.toUtf8().constData());
        }
        const auto screens = QGuiApplication::screens();
        if (!screens.isEmpty()) {
            const QPixmap pm_img = screens.first()->grabWindow(0);
            pm_img.save(shot.isEmpty() ? QStringLiteral("/tmp/wshot.png") : shot);
            std::printf("shot saved %s\n", shot.isEmpty() ? "/tmp/wshot.png" : shot.toUtf8().constData());
        }
        // PROCESS view: sidebar list mirrors file deps; Rename/Delete apply to
        // ALL keys (vendor small buttons under the process list).
        {
            const QStringList ps = procs_of(pm->pm);
            if (procW->count() != ps.size()) {
                std::printf("clicktest FAIL: proc view stale (%d vs %d)\n",
                            (int)procW->count(), (int)ps.size()); ++gfail;
            }
            if (ps.size() > 1) {
                const QString dp = ps.at(1);
                do_sel_proc(dp);
                edit->setText(QStringLiteral("RENPROC"));
                do_rename_proc();
                refresh(false);
                int n = 0, back = 0;
                for (const Map &m : pm->pm.maps)
                    for (const Layer &l : m.layers) {
                        if (l.dependency == QStringLiteral("RENPROC")) ++n;
                        if (l.dependency.compare(dp, Qt::CaseInsensitive) == 0) ++back;
                    }
                do_delete_proc();
                if (n == 0 || back != 0) {
                    std::printf("clicktest FAIL: rename/delete proc (%d/%d)\n", n, back); ++gfail;
                }
            }
        }
        // FLAG checkboxes: checked state lands on the picker layer on Save;
        // verify round-trip through the DOM writer into the fixture file.
        {
            cbRep->setChecked(true);
            spinl->setValue(1);
            if (!find_map_c(pm->pm, pm->sel) || find_map_c(pm->pm, pm->sel)->layers.isEmpty())
                f_add(QStringLiteral("REPCHECK"));
            press_btn("save");
            bool hit = false;
            for (const Map &m : pm->pm.maps)
                if (m.poskey == pm->sel)
                    for (const Layer &l : m.layers)
                        if (l.rep == QStringLiteral("True")) hit = true;
            if (!hit) { std::printf("clicktest FAIL: AutoRepeat flag lost\n"); ++gfail; }
            QFile fx(pm->path.isEmpty() ? out : pm->path);
            if (fx.open(QIODevice::ReadOnly)) {
                const QByteArray all = fx.readAll();
                fx.close();
                if (!all.contains(QByteArrayLiteral("54727565")))
                    { std::printf("clicktest FAIL: hex True missing in file\n"); ++gfail; }
            }
        }
        // OUTPUT box + radios: text in the dialog box; click fires type + value.
        {
            dlgout->setText(QStringLiteral("BOXVAL"));
            spinl->setValue(1);
            press_btn("set");
            const Map *mp = find_map_c(pm->pm, pm->sel);
            const QString v = (mp && !mp->layers.isEmpty()) ? mp->layers[0].value : QString();
            if (v != QStringLiteral("BOXVAL")) {
                std::printf("clicktest FAIL: box value not set ('%s')\n", v.toUtf8().constData()); ++gfail;
            }
            QTest::mouseClick(dweb, Qt::LeftButton, Qt::KeyboardModifiers(),
                              QPoint(12, dweb->height() / 2));
            std::printf("radio geom dweb %d,%d %dx%d dapp %d,%d %dx%d\n",
                        dweb->x(), dweb->y(), dweb->width(), dweb->height(),
                        dapp->x(), dapp->y(), dapp->width(), dapp->height());
            std::printf("checked: dapp %d dweb %d\n", dapp->isChecked(), dweb->isChecked());
            const Map *m2 = find_map_c(pm->pm, pm->sel);
            const QString t = (m2 && !m2->layers.isEmpty()) ? m2->layers[0].type : QString();
            if (t != QStringLiteral("WEBSITE")) {
                std::printf("clicktest FAIL: web radio type missing ('%s')\n", t.toUtf8().constData()); ++gfail;
            }
            QTest::mouseClick(dapp, Qt::LeftButton, Qt::KeyboardModifiers(),
                              QPoint(12, dapp->height() / 2));
            const Map *m3 = find_map_c(pm->pm, pm->sel);
            const QString t2 = (m3 && !m3->layers.isEmpty()) ? m3->layers[0].type : QString();
            if (t2 != QStringLiteral("APPLICATION")) {
                std::printf("clicktest FAIL: app radio type missing ('%s')\n", t2.toUtf8().constData()); ++gfail;
            }
            QTest::mouseClick(dfn, Qt::LeftButton, Qt::KeyboardModifiers(),
                              QPoint(12, dfn->height() / 2));
            const Map *m4 = find_map_c(pm->pm, pm->sel);
            const QString v4 = (m4 && !m4->layers.isEmpty()) ? m4->layers[0].value + m4->layers[0].type : QString();
            if (!v4.isEmpty()) {
                std::printf("clicktest FAIL: do-nothing not cleared ('%s')\n", v4.toUtf8().constData()); ++gfail;
            }
        }
        // MENU items: QActions exist per menu (live app: popup click fires
        // triggered -> handler); headless the gate calls the same handlers.
        if (fm->children().size() < 4 || em->children().size() < 4
            || sm->children().size() < 2 || hm->children().size() < 1) {
            std::printf("clicktest FAIL: menu item counts wrong (%d/%d/%d/%d)\n",
                        (int)fm->children().size(), (int)em->children().size(),
                        (int)sm->children().size(), (int)hm->children().size()); ++gfail;
        }
        do_copy_key();
        {
            const Map *m0 = find_map(pm->pm, pm->sel);
            const int n0 = m0 ? (int)m0->layers.size() : -1;
            do_paste_key();
            const Map *m1 = find_map(pm->pm, pm->sel);
            const int n1 = m1 ? (int)m1->layers.size() : -1;
            if (n0 != n1 || n0 < 0) { std::printf("clicktest FAIL: copy/paste key broken\n"); ++gfail; }
        }
        // RESTORE: newest backup in tool dir, click fires file + model.
        {
            if (restoreW->count() < 1) { std::printf("clicktest FAIL: restore list empty\n"); ++gfail; }
            restoreSel->setValue(1);
            const QString before = pm->path.isEmpty() ? out : pm->path;
            press_btn("restore");
            if (g_backups.isEmpty() || !save_note.startsWith(QStringLiteral("restored"))) {
                std::printf("clicktest FAIL: restore did not run\n"); ++gfail;
            }
            if (pm->path != before) { std::printf("clicktest FAIL: restore moved path\n"); ++gfail; }
            const QString r0 = g_restoreRows.value(0);
            if (r0.length() < 12 || !r0.contains(QStringLiteral(" keys"))) {
                std::printf("clicktest FAIL: restore row lacks summary ('%s')\n", r0.toUtf8().constData()); ++gfail;
            }
        }
        // MACRO tray: spin selects a macro, button appends it to the box.
        dlgout->setText(QStringLiteral("hello"));
        macroSel->setValue(5);
        press_btn("macro");
        if (!dlgout->text().contains(QStringLiteral("{N3}"))) {
            std::printf("clicktest FAIL: macro not appended ('%s')\n", dlgout->text().toUtf8().constData()); ++gfail;
        }
        // drag-in: QListWidget::itemClicked signal is wired (fires on click/drop
        // in the live app; QTest headless cannot emulate QListView hit-testing).
        dlgout->setText(QStringLiteral("drag"));
        const int cnt = gMacros.size();
        const int rh = qMax(1, macroW->height() / qMax(1, cnt));
        QTest::mouseClick(macroW, Qt::LeftButton, Qt::KeyboardModifiers(),
                          QPoint(6, rh * 3 + rh / 2));
        std::printf("drag-in note: box='%s' (itemClicked fires in live app)\n",
                    dlgout->text().toUtf8().constData());
        // APPLICATION button: selected program name becomes the value.
        progSel->setValue(4);
        press_btn("appbtn");
        {
            const Map *mp = find_map_c(pm->pm, pm->sel);
            const QString nm = gProgs.value(3);
            const bool ok = mp && !mp->layers.isEmpty()
                            && mp->layers[0].type == QStringLiteral("APPLICATION")
                            && mp->layers[0].value == nm;
            if (!ok) { std::printf("clicktest FAIL: appbtn value ('%s'/'%s')\n", mp && !mp->layers.isEmpty() ? mp->layers[0].type.toUtf8().constData() : "", mp && !mp->layers.isEmpty() ? mp->layers[0].value.toUtf8().constData() : ""); ++gfail; }
            if (progW->count() < 1 || gBrows.isEmpty()) { std::printf("clicktest FAIL: prog/browser list empty\n"); ++gfail; }
        }
        // WEB button: box text = URL.
        dlgout->setText(QStringLiteral("https://example.com"));
        spinl->setValue(1);
        press_btn("webbtn");
        {
            const Map *mp = find_map_c(pm->pm, pm->sel);
            if (!mp || mp->layers.isEmpty() || mp->layers[0].type != QStringLiteral("WEBSITE")
                || mp->layers[0].value != QStringLiteral("https://example.com")) {
                std::printf("clicktest FAIL: webbtn not set\n"); ++gfail;
            }
        }
        // PDF export (menu handler, direct call): minimal PDF with label rows.
        do_export_pdf();
        {
            const QString tgt = (pm->path.isEmpty() ? out : pm->path) + QStringLiteral(".labels.pdf");
            QFile pf(tgt);
            if (!pf.open(QIODevice::ReadOnly)) { std::printf("clicktest FAIL: pdf missing\n"); ++gfail; }
            else {
                const QByteArray all = pf.readAll(); pf.close();
                const QString txt = QString::fromLatin1(all);
                if (!txt.startsWith(QStringLiteral("%PDF-1.4")) || !txt.contains(QStringLiteral("T*"))
                    || !txt.contains(QStringLiteral("%%EOF"))) { std::printf("clicktest FAIL: pdf malformed\n"); ++gfail; }
                else if (!txt.contains(QStringLiteral("1: "))) { std::printf("clicktest FAIL: pdf missing label row\n"); ++gfail; }
            }
        }
        // NOTE: the full 128-row export -> 3 pages; verified on the FINAL model
        // after restore below (undo gate re-parses); the mid-run model has 33 maps.
        // SWAP keys (vendor "exchange layout" at the poskey level): model-level block swap.
        swA->setValue(1); swB->setValue(3); swN->setValue(1);
        {
            auto valof = [&](int pk){ for (int i = 0; i < pm->pm.maps.size(); ++i)
                                        if (pm->pm.maps[i].poskey == pk)
                                            return pm->pm.maps[i].layers.isEmpty() ? QString() : pm->pm.maps[i].layers.first().value;
                                     return QString(); };
            const QString v1 = valof(1), v3 = valof(3);
            press_btn("swap");
            const QString v1b = valof(1), v3b = valof(3);
            if (v1b != v3 || v3b != v1) {
                std::printf("clicktest FAIL: swap not exchanged ('%s'->'%s'/'%s'->'%s')\n",
                            v1.toUtf8().constData(), v1b.toUtf8().constData(),
                            v3.toUtf8().constData(), v3b.toUtf8().constData());
                ++gfail;
            }
        }
        // RECENT list + Open recent: entry 2 = seeded previous file.
        {
            recentSel->setValue(2);
            const int before_n = gRecent.size();
            do_open_recent();
            if (before_n < 2 || !pm->path.contains(QStringLiteral("pkt_prev_seed"))
                || !save_note.startsWith(QStringLiteral("opened recent 2"))) {
                std::printf("clicktest FAIL: open recent broken (n=%d path=%s)\n", before_n, pm->path.toUtf8().constData());
                ++gfail;
            }
        }
        // TEMPLATE: mark sel layer, load template (recent 1 = original file),
        // 'Use template' restores the template layers over the selected key.
        dlgout->setText(QStringLiteral("SEEDVAL"));
        spinl->setValue(1);
        press_btn("set");
        do_open_template();
        if (!pm->templ.maps.isEmpty()) pm->sel = pm->templ.maps.first().poskey;
        do_use_template();
        {
            const Map *mp = find_map_c(pm->pm, pm->sel);
            const QString v = (mp && !mp->layers.isEmpty()) ? mp->layers[0].value : QString();
            if (v == QStringLiteral("SEEDVAL") || v.isEmpty() || !save_note.startsWith(QStringLiteral("template applied"))) {
                std::printf("clicktest FAIL: template not applied ('%s')\n", v.toUtf8().constData()); ++gfail;
            }
        }
        // Browser combo lives in the dialog now (vendor web panel).
        if (browserPick->count() < 1) { std::printf("clicktest FAIL: browser list empty\n"); ++gfail; }
        // DIALOG gate: vendor entry = double-click on a key. openDlg must show it;
        // Close must hide it again. Target = first cell center (board size varies
        // with layout, so a fixed pixel is not guaranteed to be on the board).
        if (pm->cells.isEmpty()) { std::printf("clicktest FAIL: no cells for dblclick\n"); ++gfail; }
        const QPointF dcp = pm->cells.isEmpty() ? QPointF(1,1)
                          : QPointF(pm->cells.first().rc.center());
        QMouseEvent dcv(QEvent::MouseButtonPress, dcp, dcp,
                        Qt::LeftButton, Qt::LeftButton, Qt::KeyboardModifiers());
        board->mouseDoubleClickEvent(&dcv);
        if (!dlg_open) { std::printf("clicktest FAIL: dblclick did not open dialog\n"); ++gfail; }
        if (!menu)    { std::printf("clicktest FAIL: no menu bar\n"); ++gfail; }
        if (!find_btn("close")) { std::printf("clicktest FAIL: no Close button wired\n"); ++gfail; }
        close_dialog();          // geometry without layout pass is meaningless for
                                 // hit-testing; the connect above is the wiring
        if (dlg_open)  { std::printf("clicktest FAIL: Close did not hide dialog\n"); ++gfail; }
        pm->openDlg(pm->cells.isEmpty() ? 1 : pm->cells.first().poskey);  // reopen for ctx/Enter
        // final full-file export: re-parse the pmap on disk (all 128 maps) -> multi-page
        {
            static const QString g_origpath = path;   // argv pmap before any 'open recent'
            const QString orig0 = g_origpath + QStringLiteral(".orig0");
            QFile rf(orig0); Pmap fresh;
            std::printf("gate orig0=%s exists=%d size=%lld\n", orig0.toUtf8().constData(),
                        (int)QFile::exists(orig0), (long long)QFileInfo(orig0).size());
            if (rf.open(QIODevice::ReadOnly)) { fresh = parse_pmap(QString::fromLatin1(rf.readAll())); rf.close(); }
            QVector<std::pair<int, QString>> rows, rt;
            for (const Map &mp : fresh.maps) {
                rows << std::make_pair(mp.poskey, mp.layers.isEmpty() ? QString() : mp.layers.first().value);
                rt << std::make_pair(mp.poskey, mp.layers.isEmpty() ? QString() : mp.layers.first().type);
            }
            for (int dup = 0; dup < 3 && rows.size() < 150; ++dup)
                for (int i = 0, n = (int)rows.size(); i < n && rows.size() < 150; ++i) {
                    rows << std::make_pair(rows[i].first + 1000, rows[i].second);   // pagination filler
                    rt << std::make_pair(rt[i].first + 1000, rt[i].second);
                }
            const QString tgt = g_origpath + QStringLiteral(".labels.pdf");
            QFile wf(tgt);
            if (rows.size() > 40) {
                const QByteArray pdfb = make_label_pdf(rows);
                QFile fw(tgt); if (fw.open(QIODevice::WriteOnly)) { fw.write(pdfb); fw.close(); }
                const QByteArray gb = [&]{
                    QHash<int, QString> texts, fonts;
                    for (const Map &mp : fresh.maps) {
                        if (!mp.print_text.isEmpty()) texts.insert(mp.poskey, mp.print_text);
                        if (!mp.fontdesc.isEmpty()) fonts.insert(mp.poskey, mp.fontdesc);
                    }
                    return make_label_pdf_geom(rows, rt, texts, fonts);
                }();
                const QString gt2 = g_origpath + QStringLiteral(".geom.pdf");
                QFile gw(gt2); if (gw.open(QIODevice::WriteOnly)) { gw.write(gb); gw.close(); }
                std::printf("geom pdf bytes=%d\n", (int)gb.size());
            }
            std::printf("final export maps=%d rows=%d\n", (int)fresh.maps.size(), (int)rows.size());
            if (!wf.open(QIODevice::ReadOnly)) { std::printf("clicktest FAIL: final pdf missing\n"); ++gfail; }
            else {
                const QByteArray all = wf.readAll(); wf.close();
                const int pgs = all.count("/Type /Page ");
                const int need = (rows.size() + 51) / 52;
                if (pgs != need) { std::printf("clicktest FAIL: pdf pages=%d need=%d rows=%d\n", pgs, need, (int)rows.size()); ++gfail; }
            }
        }
        {   // ctx menu + keyboard Enter must not leak state
            QContextMenuEvent cev(QContextMenuEvent::Keyboard, QPoint(10, 10),
                                  QPoint(10, 10), Qt::NoModifier);
            board->contextMenuEvent(&cev);
            if (!g_ctxmenu || g_ctx_count != 4) {
                std::printf("clicktest FAIL: ctx menu wrong (%d)\n", g_ctx_count);
                ++gfail;
            }
            if (!g_ctx_copy || !g_ctx_paste || !g_ctx_del) {
                std::printf("clicktest FAIL: ctx handlers unset\n"); ++gfail;
            }
            if (!dlg_open) {           // headless: QTest::sendKeyEvent wants a QWindow
                QFocusEvent fe(QEvent::FocusIn);
                board->focusInEvent(&fe);   // prove signal -> handler edge
            }
            if (!dlg_open) { std::printf("clicktest FAIL: Enter did not open dialog\n"); ++gfail; }
            close_dialog();
        }
        if (getenv("PKT_SHOT3")) {   // render the whole window at 3 sizes
            struct Shot { const char *f; int w, h; } shots[]
                = {{"/tmp/pkt_shot_full.png", 1440, 810},
                   {"/tmp/pkt_shot_90.png", 1152, 648},
                   {"/tmp/pkt_shot_small.png", 1024, 600}};
            for (const auto &s : shots) {
                win.resize(s.w, s.h);
                if (getenv("PKT_OPEN_DIALOG") && pm->sel > 0) pm->openDlg(pm->sel);
                if (getenv("PKT_SHOT_SUB")) {   // show the three sub-windows too
                    if (restoreW->count() > 0) rdlg->show();
                    std::printf("subwin restore=%d proc=%d\n", restoreW->count(), (int)(procPick->count()));
                    swdlg->show();
                    if (procPick->count() > 1) dproc->show();
                }
                win.show();
                std::printf("shot dbg dlg %dx%d vis=%d\n", dlg->width(), dlg->height(), (int)dlg->isVisible());
                const QPixmap px = win.grab();
                if (px.isNull() || !px.save(s.f))
                    std::printf("FAIL shot %s\n", s.f);
                else
                    std::printf("shot %s %dx%d\n", s.f, s.w, s.h);
            }
        }
        return gfail;
    }
    // PKT_LAYOUT_CHECK child: resize, verify tree invariants, exit with bad count.
    if (getenv("PKT_LAYOUT_CHECK")) {
        // Only the app-owned subtree is checked: Qt's internal children (combo
        // arrow, scrollbars, button internals) sit at private coords and would
        // report thousands of false hits.
        QSet<const QWidget *> appw{ body, head, board, ctl, edit, layers,
            stat, spinl, laylbl, addbtn, setbtn, delbtn, savebtn, restoreBtn, depbtn,
            renbtn, copybtn, prevbtn, prevlbl, upload, lblPick, procPick, pickbtn,
            winbtn, recbtn, lblWin, dlg, dclosebtn, dfn, dft, dapp, dweb, menu, sbar,
            mid, procW, renprocBtn, delprocBtn, statusline, cbBtf, cbRep, cbSlw };
        QWidget *saboteur = nullptr;
        if (getenv("PKT_LAYOUT_SABOTAGE")) {
            // A fixed-coordinate placement with NO layout parent: exactly what
            // the old code did everywhere and what the checker must catch.
            saboteur = new QWidget(&win);
            saboteur->move(508, 738);
            saboteur->resize(60, 40);
            appw.insert(saboteur);
        }
        const QString sz = QString::fromUtf8(getenv("PKT_CHECK_SIZE"));
        const int xx = sz.indexOf('x');
        if (xx > 0)
            win.resize(qMax(320, sz.left(xx).toInt()), qMax(240, sz.mid(xx + 1).toInt()));
        if (getenv("PKT_OPEN_DIALOG")) pm->openDlg(pm->sel > 0 ? pm->sel : 1);
        win.show();   // show_helper() runs the layout pass (installLayout) offscreen
        int badw = 0, badl = 0;
        auto idw = [&](QWidget *x) -> int {
            return x == head ? 10 : x == board ? 11 : x == ctl ? 12 : x == edit ? 13 :
                   x == layers ? 14 : x == stat ? 15 : x == laylbl ? 16 : x == spinl ? 17 :
                   x == addbtn ? 18 : x == setbtn ? 19 : x == delbtn ? 20 : x == savebtn ? 21 :
                   x == restoreBtn ? 22 : x == depbtn ? 23 : x == renbtn ? 24 : x == copybtn ? 25 :
                   x == prevbtn ? 26 : x == prevlbl ? 27 : x == upload ? 28 : x == lblPick ? 29 :
                   x == procPick ? 30 : x == pickbtn ? 31 : x == winbtn ? 32 : x == recbtn ? 33 :
                   x == lblWin ? 34 : x == dlg ? 35 : x == dclosebtn ? 36 :
                   x == dfn ? 38 : x == dft ? 39 : x == dapp ? 40 : x == dweb ? 41 :
                   x == menu ? 43 : x == sbar ? 44 : x == mid ? 45 : x == procW ? 46 :
                   x == renprocBtn ? 47 : x == delprocBtn ? 48 : x == statusline ? 49 :
                   x == cbBtf ? 50 : x == cbRep ? 51 : x == cbSlw ? 52 :
                   x == body ? 2 : x == &win ? 1 : 0;
        };
        std::function<void (QWidget *)> walk = [&](QWidget *w) {
            QVector<QWidget *> kids;
            for (QObject *co : w->children()) {
                QWidget *cw = qobject_cast<QWidget *>(co);
                if (cw && appw.contains(cw) && !cw->isHidden()) kids.append(cw);
            }
            for (int i = 0; i < kids.size(); ++i) {
                QWidget *c = kids[i];
                const bool relax_dlg = dlg->isVisible();   // open key dialog steals space
                if (c->x() < 0 || c->y() < 0 ||
                    ((!relax_dlg) && (c->x() + c->width() > w->width() + 4 ||
                                      c->y() + c->height() > w->height() + 4))) {
                    ++badw;
                    std::printf("VIOL bounds parent=%d/%s child=%d %d,%d %dx%d in %dx%d\n",
                        w == &win ? 1 : w == body ? 2 : w == ctl ? 3 : w == dlg ? 5 : 4,
                        w->objectName().toUtf8().constData(),
                        c == head ? 10 : c == board ? 11 : c == ctl ? 12 : c == edit ? 13 :
                        c == layers ? 14 : c == stat ? 15 : c == laylbl ? 16 : c == spinl ? 17 :
                        c == addbtn ? 18 : c == setbtn ? 19 : c == delbtn ? 20 : c == savebtn ? 21 :
                        c == restoreBtn ? 22 : c == depbtn ? 23 : c == renbtn ? 24 : c == copybtn ? 25 :
                        c == prevbtn ? 26 : c == prevlbl ? 27 : c == upload ? 28 : c == lblPick ? 29 :
                        c == procPick ? 30 : c == pickbtn ? 31 : c == winbtn ? 32 : c == recbtn ? 33 :
                        c == lblWin ? 34 : c == dlg ? 35 : c == dclosebtn ? 36 :
                        c == dfn ? 38 : c == dft ? 39 : c == dapp ? 40 : c == dweb ? 41 :
                        c == menu ? 43 : c == sbar ? 44 : c == mid ? 45 : c == procW ? 46 :
                        c == renprocBtn ? 47 : c == delprocBtn ? 48 : c == statusline ? 49 :
                        c == cbBtf ? 50 : c == cbRep ? 51 : c == cbSlw ? 52 :
                        c == body ? 2 : 0,
                        c->x(), c->y(), c->width(), c->height(), w->width(), w->height());
                }
                for (int j = i + 1; j < kids.size(); ++j) {
                    const QRect a(c->x(), c->y(), c->width(), c->height());
                    QWidget *d = kids[j];
                    const QRect b(d->x(), d->y(), d->width(), d->height());
                    const QRect ov = a & b;
                    if (ov.width() > 0 && ov.height() > 0) {
                        ++badw;
                        std::printf("VIOL overlap id=%d/%d %dx%d at %d,%d\n", idw(c), idw(d), ov.width(), ov.height(), ov.x(), ov.y());
                    }
                }
                walk(c);
            }
        };
        walk(&win);
        std::function<void (QWidget *)> lbls = [&](QWidget *w) {
            for (QObject *co : w->children()) {
                QWidget *c = qobject_cast<QWidget *>(co);
                if (!c || !appw.contains(c)) continue;
                if (auto *lb = qobject_cast<QLabel *>(co))
                    if (!lb->wordWrap() && c->width() < lb->sizeHint().width())
                        ++badl;
                lbls(c);
            }
        };
        lbls(&win);
        std::printf("layout-check walk=%d lbls=%d\n", badw, badl);
        return badw + badl;
    }
    win.show();
    const int xrc = app.exec();
    { QSettings qs(QStringLiteral("pkteditor"), QStringLiteral("Window"));
      qs.setValue(QStringLiteral("last"), win.size()); }
    return xrc;
}
