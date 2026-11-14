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
#include <QWidget>
#include <QLabel>
#include <QPoint>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QComboBox>
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
#include <filesystem>
#include <system_error>
#include <QHash>
#include <QSet>
#include <QTimer>
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
    QString path;             // source file (save target)
    QVector<Cell> cells;      // board-local geometry
    QPoint dbg;               // last click point (debug)
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

static void paint_grid(QPainter &pt, const Model &m, const QSize &board) {
    pt.setBrush(QColor(0x1b, 0x1d, 0x22));
    pt.setPen(QColor(0x35, 0x38, 0x3e));
    pt.drawRoundedRect(QRect(QPoint(0, 0), board), 10, 10);
    for (const Cell &c : m.cells) {
        if (!c.anchor) continue; // right half of a wide cap shares the painted rect
        const Map *mp = find_map_c(m.pm, c.poskey);
        const bool is_sel = (m.sel == c.poskey);
        QColor fill(0xe6, 0xe7, 0xea);
        for (const Cell &cc : m.cells)
            if (cc.poskey == c.poskey + 1 && cc.rc == c.rc) { fill = QColor(0xc8, 0xd8, 0xe2); break; }
        if (mp && !mp->layers.isEmpty()) {
            const int n = qMin(mp->layers.size(), 4);
            fill = QColor(0xb8 - 8 * n, 0xd6 - 4 * n, 0xf2 - 4 * n);
        }
        if (is_sel) fill = QColor(0xff, 0x8a, 0x3d);
        pt.setBrush(fill);
        pt.setPen(QColor(0x6a, 0x6f, 0x78));
        pt.drawRoundedRect(c.rc, 4, 4);
        const int fs = qBound(7, c.rc.width() / 4, 12);
        pt.setPen(QColor(0x22, 0x25, 0x2b));
        pt.setFont(QFont(QStringLiteral("sans-serif"), fs));
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
            pt.setPen(QPen(QColor(0xc0, 0x50, 0x18), 2));
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
    return QStringLiteral("saved %1 maps / %2 layers / DOM-lossless / atomic / backup %3 / reread STABLE=%4")
           .arg(m.pm.maps.size()).arg(layers_here).arg(bakk).arg(stable ? "yes" : "NO");
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
};

// ------------------------------------------------------------------- main ----

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
    if (mode == "smoke") return gate_smoke(m);
    if (mode == "watch") {
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
    win.setFixedSize(1180, 740);
    MapView board(&win);
    board.m_ = pm;
    board.move(8, 64);
    board.setFixedSize(QSize(1164, 560));
    board.start_heartbeat();

    QLabel head(&win);
    head.move(10, 10);
    head.setWordWrap(true);
    QLineEdit edit(&win);
    edit.move(8, 640);
    edit.setPlaceholderText(QStringLiteral("MACRO"));
    QLabel layers(&win);
    layers.move(10, 662);
    QLabel stat(&win);
    stat.move(10, 690);
    QPushButton addbtn(QStringLiteral("+ Layer"), &win);
    QPushButton setbtn(QStringLiteral("Set"), &win);
    QPushButton delbtn(QStringLiteral("Delete Layer"), &win);
    QPushButton savebtn(QStringLiteral("Save"), &win);
    QPushButton undobtn(QStringLiteral("Undo"), &win);
    QPushButton depbtn(QStringLiteral("Set Dep"), &win);
    QPushButton renbtn(QStringLiteral("Type"), &win);
    QPushButton copybtn(QStringLiteral("Copy"), &win);
    QSpinBox spinl(&win);              // LAYER PICKER: 1..n, clamped by refresh
    QLabel laylbl(QStringLiteral("Layer #"), &win);
    spinl.setMinimum(1);
    spinl.setValue(1);
    laylbl.move(140, 648);
    spinl.move(138, 664);
    addbtn.move(260, 662);      setbtn.move(384, 662);    delbtn.move(508, 662);
    savebtn.move(632, 662);    undobtn.move(756, 662);
    depbtn.move(260, 700);      // 2nd row: edit text -> dependency of picked layer
    renbtn.move(384, 700);      // rename TYPE of picked layer
    copybtn.move(508, 700);     // duplicate picked layer at end
    addbtn.setObjectName("add");   setbtn.setObjectName("set");   delbtn.setObjectName("del");
    depbtn.setObjectName("dep");   renbtn.setObjectName("ren");   copybtn.setObjectName("copy");
    savebtn.setObjectName("save"); undobtn.setObjectName("undo");
    QPushButton prevbtn(QStringLiteral("Preview"), &win);
    QLabel prevlbl(&win);
    prevbtn.move(632, 700);
    prevbtn.setObjectName("preview");
    prevlbl.move(10, 726);
    // LB5: button + QProgressDialog + mapped errors, one mount (upload.h)
    UploadPanel upload(&win, &pm->pm, mwx,
                       up_arm || getenv("PKT_UPLOAD_ARM") != nullptr);
    QLabel lblPick(&win);               // process-picker readout
    lblPick.move(10, 750);
    QComboBox procPick(&win);           // PROCESS PICKER (GUI Plan Phase 1)
    procPick.setObjectName("procpick");
    procPick.move(8, 728);
    QPushButton pickbtn(QStringLiteral("Use Picked Proc"), &win);
    pickbtn.setObjectName("pick");
    pickbtn.move(756, 738);
    upload.setFixedSize(QSize(260, 90));

    // live board: pressed POSKeys get a green ring (calib maps scancode->POSKey)
    const QHash<int, int> cal = load_calib(
        calib.isEmpty() ? QStringLiteral("/home/nonrmy/MK.calib.json") : calib);
    QTimer poll;
    poll.setInterval(50);
    // signal = timeout (start() never emits!): the IN-stream loop was DEAD
    // with the start()-cast — pressed set never updated live.
    const QMetaObject::Connection pc = poll.connect(
        &poll, &QTimer::timeout, [&]() {
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
    auto refresh = [&](bool saved) {
        const int pk = pm->sel;
        head.setText(QStringLiteral("%1  maps=%2  sel=%3  dbg=%4,%5  pressed=%6")
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
            if (spinl.maximum() != n) spinl.setMaximum(n);
            spinl.setValue(qBound(1, spinl.value(), n));
            const int lsel = spinl.value();
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
            layers.setText(QStringLiteral("layers (%1): %2").arg(n)
                                              .arg(ls.join(" | ")));
        } else {
            layers.setText(QStringLiteral("layers (0) — type a MACRO, then + Layer / Set"));
        }
        QStringList nocard;
        for (const Map &mm : pm->pm.maps)
            if (describe_cell(mm.poskey).isEmpty()) nocard << QString::number(mm.poskey);
        QString s = QStringLiteral("grid: %1  POSKeys ohne Cap: %2")
                        .arg(grid_stats()).arg(nocard.join(","));
        if (saved) s += QStringLiteral("  %1").arg(save_note);
        // process picker: (re)fill combo on save + first call; label = current pick
        if (saved || procPick.count() == 0) {
            procPick.clear();
            procPick.addItems(pktm::list_processes(120));
        }
        if (procPick.count() > 0)
            procPick.setCurrentIndex(qBound(0, procPick.currentIndex(), procPick.count() - 1));
        const QString cur = procPick.count() ? procPick.itemText(procPick.currentIndex()) : QString();
        lblPick.setText(cur.isEmpty()
            ? QStringLiteral("picker: running processes not listed (no probe)")
            : QStringLiteral("picked: '%1' (n=%2)").arg(cur).arg(procPick.count()));
        stat.setText(s);
    };
    auto f_add = [&](const QString &v){ add_layer(pm->pm, pm->sel, v.isEmpty() ? edit.text() : v); refresh(false); };
    auto f_set = [&](const QString &v){ set_layer_value(pm->pm, pm->sel, spinl.value(), v.isEmpty() ? edit.text() : v); refresh(false); };
    auto f_del = [&]{ del_layer(pm->pm, pm->sel, spinl.value()); refresh(false); };
    auto f_dep = [&](const QString &v){ set_layer_dep(pm->pm, pm->sel, spinl.value(), v.isEmpty() ? edit.text() : v); refresh(false); };
    auto f_ren = [&](const QString &v){ set_layer_type(pm->pm, pm->sel, spinl.value(), v.isEmpty() ? edit.text().toUpper() : v.toUpper()); refresh(false); };
    auto f_copy = [&]{ copy_layer(pm->pm, pm->sel, spinl.value()); refresh(false); };
    // Process picker: QComboBox fed by mapper.cpp list_processes() — the daemon's
    // own probe chain (kdotool ps) + comm names from /proc, i.e. exactly the
    // names the daemon's dep layer can match. 'Use Picked Proc' assigns the
    // combi's current entry as dep of the picked layer.
    auto f_pick = [&]{
        const QString cur = procPick.currentText();
        set_layer_dep(pm->pm, pm->sel, spinl.value(), cur.isEmpty() ? edit.text() : cur);
        refresh(false);
    };
    pickbtn.connect(&pickbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_pick]{ f_pick(); });
    auto f_prev = [&]{
        const Map *mp = find_map_c(pm->pm, pm->sel);
        const QString t = mp ? pktm::preview_map(*mp, spinl.value())
                             : QStringLiteral("no rule for selected cell");
        prevlbl.setText(t);
        std::printf("preview: %s\n", t.toUtf8().constData());
        refresh(false);
    };
    prevbtn.connect(&prevbtn, &QPushButton::clicked, [&]{ f_prev(); });
    addbtn.connect(&addbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_add]{ f_add(edit.text()); });
    depbtn.connect(&depbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_dep]{ f_dep(edit.text()); });
    setbtn.connect(&setbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_set]{ f_set(edit.text()); });
    delbtn.connect(&delbtn, &QPushButton::clicked, [pm, &save_note, &refresh, f_del]{ f_del(); });
    renbtn.connect(&renbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh, f_ren]{ f_ren(edit.text()); });
    copybtn.connect(&copybtn, &QPushButton::clicked, [pm, &save_note, &refresh, f_copy]{ f_copy(); });
    savebtn.connect(&savebtn, &QPushButton::clicked, [pm, &out, &save_note, &refresh]{
        save_note = save_pmap(*pm, pm->path.isEmpty() ? out : pm->path); refresh(true); });
    undobtn.connect(&undobtn, &QPushButton::clicked, [&save_note, &refresh]{
        if (g_undo.isEmpty()) save_note = QStringLiteral("undo: nothing in this session");
        else {
            const UndoSnap last = g_undo.last();
            QFile cur(last.path);
            const QByteArray now = cur.open(QIODevice::ReadOnly) ? cur.readAll() : QByteArray();
            if (now != last.post) save_note = QStringLiteral("undo: file changed since save — NOT restored");
            else {
                const QString e = write_atomic(last.path, last.pre);
                save_note = e.isEmpty() ? QStringLiteral("undo restored %1").arg(last.path) : e;
            }
            g_undo.removeLast();
        }
        refresh(true); });

    refresh(false);
    pm->notify = [&]() { refresh(false); };
    poll.start();
    if (mode == "clicktest") {
        const QPoint target(900, 300);   // board-local target (widget coords)
        QMouseEvent ev(QEvent::MouseButtonPress, QPointF(target), QPointF(target),
                       Qt::LeftButton, Qt::LeftButton, Qt::KeyboardModifiers());
        board.mousePressEvent(&ev);      // direct handler call (Qt6 dropped QMeta GUI)
        pm->cells = layout_cells(board.size());  // paintEvent not run in clicktest
        board.mousePressEvent(&ev);
        std::printf("clicktest sel=%d target=(900,300) cell=%s\n",
                    pm->sel, describe_cell(pm->sel).toUtf8().constData());
        // BUTTON-WIRING GATE: real click events via QTest::mouseClick (Qt6 keeps
        // the hook; only QMeta::Clicked itself was replaced). Buttons found by
        // objectName -> wiring, not just handlers.
        auto find_btn = [&](const char *name) -> QPushButton * {
            for (QObject *w : win.children())
                if (auto *pb = qobject_cast<QPushButton *>(w))
                    if (pb->objectName() == QLatin1String(name))
                        return pb;
            return nullptr;
        };
        auto press_btn = [&](const char *name) -> bool {
            if (QPushButton *pb = find_btn(name)) { QTest::mouseClick(pb, Qt::LeftButton); return true; }
            return false;
        };
        int gfail = 0;
        if (!(find_btn("add") && find_btn("set") && find_btn("del")
              && find_btn("dep") && find_btn("ren") && find_btn("copy")
              && find_btn("preview") && find_btn("pick")))
            { std::printf("clicktest FAIL: button by objectName missing\n"); ++gfail; }
        edit.setText(QStringLiteral("L1")); press_btn("add");
        edit.setText(QStringLiteral("L2")); press_btn("add");
        spinl.setValue(1);
        edit.setText(QStringLiteral("AAA")); press_btn("set");
        edit.setText(QStringLiteral("DEP1")); press_btn("dep");
        edit.setText(QStringLiteral("WEBSITE")); press_btn("ren");
        press_btn("copy");
        spinl.setValue(3); press_btn("del");
        // negative case: disabled button must not fire
        copybtn.setEnabled(false);
        spinl.setValue(1);
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
        spinl.setValue(1);
        press_btn("preview");
        spinl.setValue(2);
        press_btn("preview");
        // process-picker gate: list from mapper.cpp (same code the dep-match uses)
        const QStringList procs = pktm::list_processes(20);
        std::printf("picker n=%d first='%s'\n", (int)procs.size(),
                    procs.isEmpty() ? "" : procs.first().toUtf8().constData());
        spinl.setValue(1);
        press_btn("pick");
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
        return gfail;
    }
    win.show();
    return app.exec();
}
