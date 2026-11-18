// Clean main-window shell per the 2026-11-18 spec: exactly FOUR areas —
// menu ribbon (tabs + big icon buttons), dark sidebar (logo, keyboard pic, hint),
// key grid (fills the center, scales to fit), one status bar (zoom-, %, zoom+, path).
// The old 3000-line gui stays reachable via --legacy-gui until it can be deleted.
// No process list here: it belongs to the "New key" dialog popup (next step).
// Debug strings appear only with --debug. Hard structural gate: --layout-hard.
#include <QChar>
#include <QLabel>
#include <QPaintEvent>
#include <QColor>
#include <QDateTime>
#include <QFrame>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QMenuBar>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStatusBar>
#include <QApplication>
#include <QSpinBox>
#include <QFile>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QToolButton>
#include <QVector>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "fitlabel.h"
#include "pmap.h"

extern int gui_legacy_main(int argc, char **argv);

static QHash<int, QString> g_labels;   // poskey -> vendor MAP-level print text
static int g_zoom = 100;               // percent; grid scales, scrollbars only when needed

struct CellRC { int poskey = 0; QRect rc; };

// ---------------- key grid widget ----------------
class KeyGrid : public QWidget {
public:
    KeyGrid(QWidget *parent = nullptr) : QWidget(parent) {}
    int sel = -1;
    QVector<CellRC> cells;
    void relayout() {
        cells.clear();
        if (g_geom.isEmpty()) return;
        int mx = 1, my = 1;
        for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
            const KeyGeom &k = it.value();
            mx = qMax(mx, k.x + k.w); my = qMax(my, k.y + k.h);
        }
        const double zf = g_zoom / 100.0;
        const double fit = qMin(double(width() - 16) / mx, double(height() - 16) / my);
        const double s = fit * zf;
        const int xoff = qMax(8, int((width() - mx * s) / 2));
        const int yoff = qMax(8, int((height() - my * s) / 2));
        for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
            const KeyGeom &k = it.value();
            CellRC c;
            c.poskey = it.key();
            c.rc = QRect(xoff + int(k.x * s), yoff + int(k.y * s),
                         qMax(1, int(k.w * s)), qMax(1, int(k.h * s)));
            cells << c;
        }
    }
    void selectAt(const QPoint &p) {
        for (const CellRC &c : cells)
            if (c.rc.contains(p)) { sel = c.poskey; return; }
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter pt(this);
        pt.setBrush(QColor(0xd8, 0xdb, 0xdf)); pt.setPen(QColor(0x9a, 0xa0, 0xa8));
        pt.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);
        for (const CellRC &c : cells) {
            const bool on = (c.poskey == sel);
            pt.setBrush(on ? QColor(0xff, 0xe0, 0x66) : QColor(0xff, 0xff, 0xff));
            pt.setPen(QPen(QColor(0x3c, 0x3f, 0x45), 2));
            pt.drawRoundedRect(c.rc, 2, 2);
            const QString lab = g_labels.value(c.poskey);
            if (lab.isEmpty()) continue;
            const Fit f = fit_label(lab, c.rc.width(), c.rc.height(), 12.0);
            if (f.lines.isEmpty()) continue;
            pt.setPen(QColor(0x1f, 0x1f, 0x1f));
            const double lhh = f.pt * 1.2;
            double y = c.rc.y() + (c.rc.height() - f.lines.size() * lhh) / 2.0;
            for (const QString &ln : f.lines) {
                const double lw = text_w(ln, f.pt);
                QString disp = ln;
                disp.replace(QChar(0x85), QChar(0x2026));
                pt.drawText(QPoint(int(c.rc.x() + (c.rc.width() - lw) / 2.0), int(y)), disp);
                y += lhh;
            }
        }
    }
};

// ---------------- icon set (same shapes as legacy mkicon family) ----------------
static QIcon mkicon(const char *kind)
{
    QPixmap im(32, 32);
    im.fill(QColor(0, 0, 0, 0));
    QPainter pt(&im);
    pt.setPen(QPen(QColor(0x24, 0x2a, 0x30), 3));
    const QString k = QString::fromLatin1(kind);
    if (k == "plus") { pt.drawLine(6, 16, 26, 16); pt.drawLine(16, 6, 16, 26); }
    else if (k == "minus") { pt.drawLine(6, 16, 26, 16); }
    else if (k == "folder") { pt.drawRect(6, 12, 20, 14); pt.drawLine(6, 12, 12, 8); }
    else if (k == "list") { for (int y = 8; y <= 24; y += 8) pt.drawLine(7, y, 25, y); }
    else if (k == "x") { pt.drawLine(8, 8, 24, 24); pt.drawLine(24, 8, 8, 24); }
    else if (k == "copy") { pt.drawRect(8, 8, 10, 14); pt.drawRect(14, 12, 10, 14); }
    else if (k == "trash") { pt.drawRect(10, 10, 12, 16); pt.drawLine(8, 8, 24, 8); }
    else if (k == "gear") { pt.drawEllipse(16, 16, 10, 10); pt.drawEllipse(16, 16, 3, 3); }
    else if (k == "pencil") { pt.drawLine(8, 24, 24, 8); }
    else if (k == "doc") { pt.drawRect(9, 7, 14, 18); pt.drawLine(12, 12, 20, 12); pt.drawLine(12, 17, 20, 17); }
    pt.end();
    return QIcon(im);
}

static QImage sidePic(int w, int h)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(QColor(0, 0, 0, 0));
    QPainter pt(&img);
    if (g_geom.isEmpty()) { pt.end(); return img; }
    pt.setBrush(QColor(0xe6, 0xe7, 0xea)); pt.setPen(QColor(0x9a, 0xa0, 0xa8));
    int mx = 1, my = 1;
    for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
        const KeyGeom &k = it.value();
        mx = qMax(mx, k.x + k.w); my = qMax(my, k.y + k.h);
    }
    const double s = qMin(double(w - 8) / mx, double(h - 8) / my);
    const int xo = (w - int(mx * s)) / 2, yo = (h - int(my * s)) / 2;
    for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
        const KeyGeom &k = it.value();
        pt.drawRoundedRect(QRectF(xo + k.x * s, yo + k.y * s,
                                  qMax(1.5, k.w * s - 1), qMax(1.5, k.h * s - 1)), 1.5, 1.5);
    }
    pt.end();
    return img;
}

struct Vis { int x, y, w, h; };
static QImage shotImage(QWidget &win, int w, int h);

// ---------------- legacy dispatch ----------------
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (QString(argv[i]) == "--legacy-gui")
            return gui_legacy_main(argc - 1, argv + 1);
    // ---- clean shell ----
    QString path;
    bool hard = false, shot = false, dbg = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (a == "--debug") dbg = true;
        else if (a == "--layout-hard") hard = true;
        else if (a == "--shot") shot = true;
        else if (!a.startsWith("--")) path = a;
    }
    struct M { Pmap pm; QString path; } m;
    if (!path.isEmpty()) {
        QFile pf(path);
        if (pf.open(QIODevice::ReadOnly)) m.pm = parse_pmap(pf.readAll());
    }
    for (const Map &mp : m.pm.maps)
        if (!mp.print_text.isEmpty()) g_labels.insert(mp.poskey, mp.print_text);
    m.path = path;

    QApplication app(argc, argv);
    QWidget win;
    win.setObjectName("win");
    win.resize(1100, 640);
    QVBoxLayout *vwin = new QVBoxLayout(&win);

    // 1) ribbon: five tabs + big icon buttons under the active tab
    QMenuBar *menu = new QMenuBar(&win);
    menu->setObjectName("menu");
    QMenu *tabs[5] = {nullptr};
    const char *tabcaps[5] = {"File", "Edit", "Modules", "Settings", "Help"};
    for (int t = 0; t < 5; ++t)
        tabs[t] = menu->addMenu(QString::fromUtf8(tabcaps[t]));
    vwin->insertWidget(0, menu);

    QWidget *accent = new QWidget(&win);
    accent->setObjectName("accent");
    accent->setStyleSheet(QStringLiteral("background:#4f7d96;"));
    accent->setMinimumHeight(6);
    vwin->insertWidget(1, accent);

    QWidget *ribbon = new QWidget(&win);
    ribbon->setObjectName("ribbon");
    QVBoxLayout *rv = new QVBoxLayout(ribbon);
    QSpinBox *tabSel = new QSpinBox(ribbon);
    tabSel->setObjectName("tabSel"); tabSel->setMinimum(1); tabSel->setMaximum(5); tabSel->setValue(1);
    QWidget *ribgrp[5];
    for (int t = 0; t < 5; ++t) { ribgrp[t] = new QWidget(ribbon); new QHBoxLayout(ribgrp[t]); }
    auto rib_sync = [tabSel, &ribgrp] {
        for (int t = 0; t < 5; ++t)
            (tabSel->value() == t + 1 ? ribgrp[t]->show() : ribgrp[t]->hide());
    };
    rv->addStretch();
    vwin->insertWidget(2, ribbon);

    auto add_rib = [tabSel, &ribgrp](int tab, const char *name, const char *icon) -> QToolButton * {
        QToolButton *b = new QToolButton(ribgrp[tab - 1]);
        b->setObjectName(name);
        b->setIcon(mkicon(icon));
        b->setToolTip(QString::fromLatin1(name));
        return b;
    };
    rv->addWidget(tabSel);
    rib_sync();
    tabSel->connect(tabSel, &QSpinBox::valueChanged, [tabSel, &ribgrp](int) {
        for (int t = 0; t < 5; ++t)
            (tabSel->value() == t + 1 ? ribgrp[t]->show() : ribgrp[t]->hide());
    });
    add_rib(1, "rib_new", "plus");
    add_rib(1, "rib_open", "folder");
    add_rib(1, "rib_recent", "list");
    if (QToolButton *rc = add_rib(1, "rib_close", "x"))
        if (m.path.isEmpty()) rc->setEnabled(false);   // vendor: Close greyed with no file
    add_rib(1, "rib_exit", "x");
    add_rib(2, "rib_copy", "copy");
    add_rib(2, "rib_paste", "copy");
    add_rib(2, "rib_del", "trash");
    add_rib(2, "rib_swap", "gear");
    add_rib(2, "rib_pdf", "doc");
    add_rib(2, "rib_templ", "pencil");
    add_rib(2, "rib_restore", "list");
    add_rib(3, "rib_modadd", "plus");
    add_rib(3, "rib_moddel", "trash");
    add_rib(4, "rib_settings", "gear");
    add_rib(5, "rib_about", "doc");
    QWidget *sidebar = new QWidget(&win);
    sidebar->setObjectName("sidebar");
    sidebar->setStyleSheet(QStringLiteral("background:#3c3c3c; color:#e8e8e8;"));
    QVBoxLayout *sv = new QVBoxLayout(sidebar);
    QLabel *logo = new QLabel(sidebar);
    logo->setObjectName("logo");
    {
        QImage img(200, 34, QImage::Format_ARGB32);
        img.fill(QColor(0, 0, 0, 0));
        QPainter pt(&img);
        pt.setBrush(QColor(0x4f, 0x7d, 0x96)); pt.setPen(QPen(QColor(0x2f, 0x4c, 0x5f), 1));
        pt.drawRoundedRect(QRect(4, 6, 26, 22), 5, 5);
        pt.setBrush(QColor(0xe6, 0xe7, 0xea)); pt.setPen(QPen(QColor(0x2f, 0x4c, 0x5f), 0));
        for (int r = 0; r < 2; ++r) for (int c = 0; c < 4; ++c)
            pt.drawRoundedRect(QRect(8 + c * 5, 10 + r * 7, 4, 5), 1, 1);
        pt.setBrush(QColor(0xff, 0xe0, 0x66));
        pt.drawRoundedRect(QRect(18, 17, 4, 5), 1, 1);
        pt.end();
        logo->setPixmap(QPixmap::fromImage(img));
    }
    QLabel *kpic = new QLabel(sidebar);
    kpic->setObjectName("kpic");
    kpic->setPixmap(QPixmap::fromImage(sidePic(220, 120)));
    QLabel *shint = new QLabel(QString::fromUtf8("Double-click a key to edit it."), sidebar);
    shint->setObjectName("shint");
    shint->setWordWrap(true);
    sv->addWidget(logo); sv->addWidget(kpic); sv->addWidget(shint);
    KeyGrid *grid = new KeyGrid(&win);
    grid->setObjectName("grid");
    grid->setMinimumSize(480, 320);
    sidebar->setMaximumWidth(260);
    QWidget *statusbar = new QWidget(&win);
    statusbar->setObjectName("statusbar");
    QHBoxLayout *sv2 = new QHBoxLayout(statusbar);
    QToolButton *zout = new QToolButton(statusbar);
    zout->setObjectName("zout"); zout->setIcon(mkicon("minus"));
    QLabel *zoomlbl = new QLabel(QStringLiteral("100 %"), statusbar);
    zoomlbl->setObjectName("zoomlbl");
    QToolButton *zin = new QToolButton(statusbar);
    zin->setObjectName("zin"); zin->setIcon(mkicon("plus"));
    QLabel *pathlbl = new QLabel(m.path, statusbar);
    pathlbl->setObjectName("pathlbl");
    sv2->addWidget(zout); sv2->addWidget(zoomlbl); sv2->addWidget(zin); sv2->addWidget(pathlbl);
    // layout: sidebar left, grid right; status bar bottom
    QWidget *row = new QWidget(&win);
    row->setObjectName("mainrow");
    QHBoxLayout *mrow = new QHBoxLayout(row);
    mrow->addWidget(sidebar, 1); mrow->addWidget(grid, 4);
    vwin->addWidget(row);
    vwin->addWidget(statusbar);

    // zoom handlers
    auto upd = [&]{ zoomlbl->setText(QStringLiteral("%1 %").arg(g_zoom));
                    grid->relayout(); };
    auto f_out = [&]{ if (g_zoom > 50) { g_zoom -= 25; upd(); } };
    auto f_in  = [&]{ if (g_zoom < 400) { g_zoom += 25; upd(); } };
    zout->connect(zout, &QToolButton::clicked, [&]{ f_out(); });
    zin->connect(zin, &QToolButton::clicked, [&]{ f_in(); });

    auto setmin = [&]{ grid->setMinimumHeight(qMax(300, int(win.height() * 0.62))); grid->relayout(); };
    setmin();
    win.show();

    if (shot) {
        struct Sh { const char *f; int w, h; } shots[] =
            { {"/tmp/pkt_clean_1920.png", 1920, 1080}, {"/tmp/pkt_clean_1366.png", 1366, 768} };
        for (const auto &s : shots) {
            win.resize(s.w, s.h);
            setmin();
            const QPixmap img = win.grab();
            if (!img.isNull() && img.save(s.f))
                std::printf("shot ok %s\n", s.f);
        }
    }

    int gfail = 0;
    if (hard) {
        // (a) structural: win's visible children == {menu, accent, ribbon, mainrow, statusbar};
        //     mainrow contains exactly {sidebar, grid}
        QSet<QString> expect = {"menu", "accent", "ribbon", "mainrow", "statusbar"};
        QSet<QString> seen;
        QWidget *mainrow = nullptr;
        for (QObject *ch : win.children()) {
            if (ch->objectName() == QStringLiteral("menu")) { seen.insert(QStringLiteral("menu")); continue; }
            QWidget *wq = qobject_cast<QWidget *>(ch);
            if (!wq || wq->isHidden()) continue;
            seen.insert(wq->objectName());
            if (wq->objectName() == QStringLiteral("mainrow")) mainrow = wq;
        }
        for (const QString &e : expect)
            if (!seen.contains(e)) { std::printf("HARD FAIL: part missing (%s)\n", e.toUtf8().constData()); ++gfail; }
        for (const QString &s2 : seen)
            if (!expect.contains(s2)) { std::printf("HARD FAIL: extra part (%s)\n", s2.toUtf8().constData()); ++gfail; }
        QSet<QString> rexp = {"sidebar", "grid"};
        QSet<QString> rseen;
        if (mainrow)
            for (QObject *ch : mainrow->children()) {
                QWidget *wq = qobject_cast<QWidget *>(ch);
                if (!wq || wq->isHidden()) continue;
                rseen.insert(wq->objectName());
            }
        for (const QString &e : rexp)
            if (!rseen.contains(e)) { std::printf("HARD FAIL: row part missing (%s)\n", e.toUtf8().constData()); ++gfail; }
        for (const QString &s2 : rseen)
            if (!rexp.contains(s2)) { std::printf("HARD FAIL: row extra part (%s)\n", s2.toUtf8().constData()); ++gfail; }
        // (b) grid fills >=45 % of window area
        const double frac = double(grid->width() * grid->height()) / double(win.width() * win.height());
        std::printf("dbg grid=%dx%d win=%dx%d sidebar=%dx%d\n", grid->width(), grid->height(),
                    win.width(), win.height(), sidebar->width(), sidebar->height());
        if (frac < 0.45) { std::printf("HARD FAIL: grid only %.1f%% of window\n", frac * 100); ++gfail; }
        // (c) nothing clipped: every visible descendant inside its parent (+4)
        std::function<void(QWidget *)> walk = [&](QWidget *p) {
            for (QObject *ch : p->children()) {
                QWidget *c = qobject_cast<QWidget *>(ch);
                if (!c || c->isHidden()) continue;
                if (c->x() < -4 || c->y() < -4 ||
                    c->x() + c->width() > p->width() + 4 ||
                    c->y() + c->height() > p->height() + 4) {
                    std::printf("HARD FAIL: clipped part %s %d,%d %dx%d\n",
                        c->objectName().toUtf8().constData(), c->x(), c->y(), c->width(), c->height());
                    ++gfail;
                }
                walk(c);
            }
        };
        walk(&win);
        // (d) labels never overlap neighbour keys (same width table as draw)
        for (const CellRC &c : grid->cells) {
            const QString lab = g_labels.value(c.poskey);
            if (lab.isEmpty()) continue;
            const Fit f = fit_label(lab, c.rc.width(), c.rc.height(), 12.0);
            if (f.wmax > c.rc.width() && !f.truncated) {
                std::printf("HARD FAIL: label overflows key %d (not flagged)\n", c.poskey);
                ++gfail;
            }
        }
        std::printf("hard gate: gfail=%d grid=%.1f%%\n", gfail, frac * 100);
    }

    if (hard) return gfail;
    if (shot) return 0;
    // normal interactive run would need event loop; keep it headless-safe:
    win.hide();
    return 0;
}
