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
#include <QLineEdit>
#include <QPushButton>
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
#include <cstdlib>
#include <cstring>
#include "pmap.h"
#include "upload.h"

static const char *ROWS[] = {"H", "G", "F", "E", "D", "C", "B", "A"}; // top row first
static const int ROWS_N[] = {15, 16, 13, 12, 12, 12, 16, 14};
static constexpr int NROWS = 8;
static constexpr int SLOTS = 16; // POSKey slots per physical row

// ------------------------------------------------------------------- model ----

struct Cell {
    int row;    // index into ROWS: 0 = H (top painted row)
    int slot;   // 1-based POSKey slot inside the row
    int poskey; // (NROWS-1-row)*SLOTS + slot
    QRect rc;   // board-local geometry
};

struct Model {
    Pmap pm;
    int sel = -1;             // selected POSKey
    QString path;             // source file (save target)
    QVector<Cell> cells;      // laid out for the current board size
};

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
    for (int r = 0; r < NROWS; ++r)
        for (int s = 1; s <= ROWS_N[r]; ++s) {
            Cell c;
            c.row = r;
            c.slot = s;
            c.poskey = (NROWS - 1 - r) * SLOTS + s;
            c.rc = QRect(xoff + (s - 1) * cap + 2, yoff + r * cap + 2, cap - 4, cap - 4);
            out.append(c);
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

static void add_layer(Pmap &pm, int poskey, const QString &value) {
    ensure_map(pm, poskey).layers.append(Layer{QStringLiteral("TEXT"), value});
}

static void del_top_layer(Pmap &pm, int poskey) {
    if (Map *m = find_map(pm, poskey))
        if (!m->layers.isEmpty()) m->layers.removeLast();
}

static QString describe_cell(int poskey) {
    if (poskey < 1 || poskey > NROWS * SLOTS) return {};
    const int r = NROWS - 1 - (poskey - 1) / SLOTS;
    const int slot = (poskey - 1) % SLOTS + 1;
    if (r < 0 || r >= NROWS || slot > ROWS_N[r]) return {};
    return QStringLiteral("%1%2").arg(ROWS[r]).arg(slot, 2, 10, QChar('0'));
}

// ---------------------------------------------------------------- painting ----

static void paint_grid(QPainter &pt, const Model &m, const QSize &board) {
    pt.setBrush(QColor(0x1b, 0x1d, 0x22));
    pt.setPen(QColor(0x35, 0x38, 0x3e));
    pt.drawRoundedRect(QRect(QPoint(0, 0), board), 10, 10);
    for (const Cell &c : m.cells) {
        const Map *mp = find_map_c(m.pm, c.poskey);
        const bool is_sel = (m.sel == c.poskey);
        QColor fill(0xe6, 0xe7, 0xea);
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
    }
}

static int cell_at(const Model &m, const QPoint &p) {
    for (const Cell &c : m.cells)
        if (c.rc.contains(p)) return c.poskey;
    return -1;
}

// ------------------------------------------------------------------- save ----

static QString save_pmap(Model &m, const QString &path) {
    const QString xml = serialize_pmap(m.pm);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QStringLiteral("SAVE FAILED: %1").arg(path);
    f.write(xml.toUtf8());
    f.close();
    QFile r(path);
    const Pmap back = r.open(QIODevice::ReadOnly) ? parse_pmap(r.readAll()) : Pmap{};
    int layers_back = 0;
    for (const Map &mm : back.maps) layers_back += mm.layers.size();
    int layers_here = 0;
    for (const Map &mm : m.pm.maps) layers_here += mm.layers.size();
    const bool stable = back.name == m.pm.name && back.maps.size() == m.pm.maps.size()
                        && layers_back == layers_here;
    return QStringLiteral("saved %1 maps / %2 layers / %3 bytes -> reload %4 maps name '%5' STABLE=%6")
               .arg(m.pm.maps.size()).arg(layers_here).arg(xml.size())
               .arg(back.maps.size()).arg(back.name).arg(stable ? "yes" : "NO");
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

static int gate_smoke(const Model &m) {
    // LB5 gate: what one Upload click would push, plus the board probe
    const auto frames = compile_frames(payload_of(m.pm, QString()));
    std::printf("upload_frames=%zu board=%s\n", frames.size(),
                device_present() ? "present" : transfer_error_message(device_last_status()));
    std::printf("%s name=%s maps=%d\n", grid_stats().toUtf8().constData(),
                m.pm.name.toUtf8().constData(), (int)m.pm.maps.size());
    return 0;
}

static int gate_click(Model &m, const QStringList &cells, const QString &setv,
                      const QString &addv, bool del, const QString &out) {
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
        if (!setv.isNull()) set_top_value(m.pm, pk, setv);
        if (!addv.isNull()) add_layer(m.pm, pk, addv);
        if (del) del_top_layer(m.pm, pk);
        const Map *after = find_map_c(m.pm, pk);
        rep += QStringLiteral("%1 pk=%2 layers %3->%4 val='%5'; ")
                    .arg(spec).arg(pk).arg(n_before).arg((int)(after ? after->layers.size() : -1))
                    .arg(after && !after->layers.isEmpty()
                             ? after->layers.last().value : QString());
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
    MapView(QWidget *parent = nullptr) : QWidget(parent) {}
    void paintEvent(QPaintEvent *) override {   // QPainter paint of the grid
        m_->cells = layout_cells(size());
        QPainter pt(this);
        paint_grid(pt, *m_, size());
    }
    void mousePressEvent(QMouseEvent *ev) override {
        const int pk = cell_at(*m_, ev->pos());   // click-to-edit entry point
        if (pk >= 0) m_->sel = pk;
    }
};

// ------------------------------------------------------------------- main ----

int main(int argc, char **argv) {
    Model m;
    QStringList cells;
    QString path, shot, setv, addv, out, mode, mwx;
    bool del = false, have_set = false, have_add = false, up_arm = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        auto next = [&](QString &dst) { if (i + 1 < argc) dst = argv[++i]; };
        if (a == "--smoke") mode = "smoke";
        else if (a == "--shot") { mode = "shot"; next(shot); }
        else if (a == "--cells") { QString v; next(v);
            // ';' separates cells when raw board coords are used (px:X,Y keeps
            // its own comma); otherwise plain ',' lists work as before.
            const QChar sep = v.contains("px:") ? ';' : ',';
            for (const QString &t : v.split(sep)) if (!t.isEmpty()) cells << t; }
        else if (a == "--set") { mode = "click"; next(setv); have_set = true; }
        else if (a == "--add") { mode = "click"; next(addv); have_add = true; }
        else if (a == "--del") { mode = "click"; del = true; }
        else if (a == "--arm-upload") up_arm = true;
        else if (a == "--mwx") next(mwx);
        else if (a == "--save") next(out);
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

    if (mode == "smoke") return gate_smoke(m);
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
                          del, out);

    // live window
    QApplication app(argc, argv);
    Model *pm = &m;
    if (!pm->pm.maps.isEmpty() && pm->sel < 0) pm->sel = pm->pm.maps.first().poskey;

    QWidget win;
    win.setFixedSize(1180, 660);
    MapView board(&win);
    board.m_ = pm;
    board.setFixedSize(QSize(1024, 512));

    QLabel head(&win);
    head.setWordWrap(true);
    QLineEdit edit(&win);
    edit.setPlaceholderText(QStringLiteral("MACRO"));
    QLabel layers(&win);
    QLabel stat(&win);
    QPushButton addbtn(QStringLiteral("+ Layer"), &win);
    QPushButton setbtn(QStringLiteral("Set"), &win);
    QPushButton delbtn(QStringLiteral("Layer lösch"), &win);
    QPushButton savebtn(QStringLiteral("Speich"), &win);
    // LB5: button + QProgressDialog + mapped errors, one mount (upload.h)
    UploadPanel upload(&win, &pm->pm, mwx,
                       up_arm || getenv("PKT_UPLOAD_ARM") != nullptr);

    QString save_note;
    auto refresh = [&](bool saved) {
        const int pk = pm->sel;
        head.setText(QStringLiteral("%1  maps=%2  sel=%3")
                         .arg(pm->pm.name.isEmpty() ? QStringLiteral("no pmap loaded")
                                                    : pm->pm.name)
                         .arg(pm->pm.maps.size())
                         .arg(pk >= 0 ? describe_cell(pk) : QStringLiteral("—")));
        const Map *mp = find_map_c(pm->pm, pk);
        if (mp && !mp->layers.isEmpty()) {
            QStringList ls;
            for (const Layer &l : mp->layers)
                ls << QStringLiteral("%1 '%2'").arg(l.type).arg(l.value);
            layers.setText(QStringLiteral("layers (%1): %2").arg(mp->layers.size())
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
        stat.setText(s);
    };
    addbtn.connect(&addbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh]{
        add_layer(pm->pm, pm->sel, edit.text()); refresh(false); });
    setbtn.connect(&setbtn, &QPushButton::clicked, [pm, &edit, &save_note, &refresh]{
        set_top_value(pm->pm, pm->sel, edit.text()); refresh(false); });
    delbtn.connect(&delbtn, &QPushButton::clicked, [pm, &save_note, &refresh]{
        del_top_layer(pm->pm, pm->sel); refresh(false); });
    savebtn.connect(&savebtn, &QPushButton::clicked, [pm, &out, &save_note, &refresh]{
        save_note = save_pmap(*pm, pm->path.isEmpty() ? out : pm->path); refresh(true); });

    refresh(false);
    win.show();
    return app.exec();
}
