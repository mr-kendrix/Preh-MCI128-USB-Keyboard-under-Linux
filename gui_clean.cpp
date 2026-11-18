// Clean main-window shell per the 2026-11-18 spec: exactly FOUR areas —
// menu ribbon (5 tabs + big icon buttons), dark sidebar (logo, keyboard pic, hint),
// key grid (fills the center, scales to fit), one status bar (zoom, path).
// Dialogs (key dialog, "New key?" question, process popup) open as overlays
// on demand; nothing else is on the main window. Legacy 3000-line gui stays
// reachable via --legacy-gui until it can be deleted.
#include <QApplication>
#include <QChar>
#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QRadioButton>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QSpinBox>
#include <QStatusBar>
#include <QString>
#include <QToolButton>
#include <QVector>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "fitlabel.h"
#include "pmap.h"
#include "mapper.h"

extern int gui_legacy_main(int argc, char **argv);

static QHash<int, QString> g_labels;              // poskey -> print label
static int g_zoom = 100;                          // percent
static int g_pending_pk = -1;                     // key the "New key?" question is about
static std::function<void(int)> g_openDlg;        // grid double-click -> dialog

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
    if (g_geom.isEmpty()) return img;
    QPainter pt(&img);
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

static QString processName(int pid)
{
    QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!f.open(QIODevice::ReadOnly)) return QString::number(pid);
    const QString s = f.readAll().trimmed();
    f.close();
    return s.isEmpty() ? QString::number(pid) : s;
}

struct CellRC { int poskey = 0; QRect rc; };

class KeyGrid : public QWidget
{
public:
    explicit KeyGrid(QWidget *parent = nullptr) : QWidget(parent) {}
    int sel = -1;
    QVector<CellRC> cells;
    void relayout()
    {
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
    void selectAt(const QPoint &p)
    {
        for (const CellRC &c : cells)
            if (c.rc.contains(p)) { sel = c.poskey; break; }
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter pt(this);
        pt.setBrush(QColor(0xd8, 0xdb, 0xdf)); pt.setPen(QColor(0x9a, 0xa0, 0xa8));
        pt.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);
        for (const CellRC &c : cells) {
            const bool on = (c.poskey == sel);
            pt.setBrush(on ? QColor(0xff, 0xe0, 0x66) : QColor(0xff, 0xff, 0xff));
            pt.setPen(QColor(0x3c, 0x3f, 0x45));
            pt.drawRoundedRect(c.rc, 2, 2);
            const QString lab = g_labels.value(c.poskey);
            if (lab.isEmpty()) continue;
            const Fit f = fit_label(lab, c.rc.width(), c.rc.height(), 12.0);
            if (f.lines.isEmpty()) continue;
            pt.setPen(QColor(0x1f, 0x1f, 0x1f));
            const double lhh = f.pt * 1.2;
            double y = c.rc.y() + (c.rc.height() - f.lines.size() * lhh) / 2.0 + f.pt;
            for (const QString &ln : f.lines) {
                const double lw = text_w(ln, f.pt);
                const double x = c.rc.x() + (c.rc.width() - lw) / 2.0;
                QString disp = ln;
                disp.replace(QChar(0x85), QChar(0x2026));
                pt.drawText(int(x), int(y), disp);
                y += lhh;
            }
        }
    }
    void mouseDoubleClickEvent(QMouseEvent *ev) override
    {
        selectAt(ev->pos());
        if (g_openDlg && sel >= 0) g_openDlg(sel);
    }
};

static QStringList procs_of(const Pmap &pm)
{
    QSet<QString> uniq;
    for (const Map &m : pm.maps)
        for (const Layer &l : m.layers)
            if (!l.dependency.isEmpty()) uniq.insert(l.dependency);
    QStringList out(uniq.begin(), uniq.end());
    out.sort();
    return out;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (QString(argv[i]) == "--legacy-gui")
            return gui_legacy_main(argc - 1, argv + 1);

    bool shot = false, hard = false, dlgategate = false, dbg = false;
    QString path;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (a == "--shot") shot = true;
        else if (a == "--layout-hard") hard = true;
        else if (a == "--dlg-gate") dlgategate = true;
        else if (a == "--debug") dbg = true;
        else if (!a.startsWith("--")) path = a;
    }

    Pmap pm;
    if (!path.isEmpty()) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            pm = parse_pmap(f.readAll());
            f.close();
        }
    }
    for (const Map &m : pm.maps)
        if (!m.print_text.isEmpty()) g_labels.insert(m.poskey, m.print_text);

    QApplication app(argc, argv);
    QWidget win;
    win.setObjectName("win");
    win.resize(1100, 640);
    win.setStyleSheet("#win{background-color:#eceef2;}");
    QVBoxLayout *vwin = new QVBoxLayout(&win);

    QMenuBar *menu = new QMenuBar(&win);
    menu->setObjectName("menu");
    const char *caps[5] = {"File", "Edit", "Modules", "Settings", "Help"};
    for (int t = 0; t < 5; ++t)
        menu->addMenu(QString::fromLatin1(caps[t]));
    vwin->insertWidget(0, menu);

    QWidget *accent = new QWidget(&win);
    accent->setObjectName("accent");
    accent->setStyleSheet("#accent{background-color:#4f7d96;}");
    accent->setMinimumHeight(6);
    vwin->insertWidget(1, accent);

    QWidget *ribbon = new QWidget(&win);
    ribbon->setObjectName("ribbon");
    QVBoxLayout *rv = new QVBoxLayout(ribbon);
    QSpinBox *tabSel = new QSpinBox(ribbon);
    tabSel->setObjectName("tabSel");
    tabSel->setMinimum(1); tabSel->setMaximum(5); tabSel->setValue(1);
    rv->addWidget(tabSel);
    QWidget *ribgrp[5];
    for (int t = 0; t < 5; ++t) { ribgrp[t] = new QWidget(ribbon); new QHBoxLayout(ribgrp[t]); }
    auto rib_sync = [tabSel, &ribgrp] {
        for (int t = 0; t < 5; ++t)
            (tabSel->value() == t + 1 ? ribgrp[t]->show() : ribgrp[t]->hide());
    };
    auto add_rib = [tabSel, &ribgrp](int tab, const char *name, const char *icon) -> QToolButton * {
        QToolButton *b = new QToolButton(ribgrp[tab - 1]);
        b->setObjectName(name);
        b->setIcon(mkicon(icon));
        b->setToolTip(QString::fromLatin1(name));
        return b;
    };
    add_rib(1, "rib_new", "plus");
    add_rib(1, "rib_open", "folder");
    add_rib(1, "rib_recent", "list");
    if (QToolButton *rc = add_rib(1, "rib_close", "x"))
        if (path.isEmpty()) rc->setEnabled(false);
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
    rib_sync();
    vwin->insertWidget(2, ribbon);

    QWidget *mainrow = new QWidget(&win);
    mainrow->setObjectName("mainrow");
    mainrow->setStyleSheet("#mainrow{background-color:#e9ebef;}");
    QHBoxLayout *mrow = new QHBoxLayout(mainrow);
    QWidget *sidebar = new QWidget(mainrow);
    sidebar->setObjectName("sidebar");
    sidebar->setStyleSheet("#sidebar{background-color:#3c3c3c;} QLabel{color:#e8e8e8;}");
    sidebar->setMaximumWidth(260);
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
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 4; ++c)
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

    KeyGrid *grid = new KeyGrid(mainrow);
    grid->setObjectName("grid");
    grid->setMinimumSize(480, 320);
    mrow->addWidget(sidebar, 1);
    mrow->addWidget(grid, 4);
    vwin->addWidget(mainrow);

    QWidget *statusbar = new QWidget(&win);
    statusbar->setObjectName("statusbar");
    QHBoxLayout *sbar = new QHBoxLayout(statusbar);
    QToolButton *zout = new QToolButton(statusbar);
    zout->setObjectName("zout"); zout->setIcon(mkicon("minus"));
    QLabel *zoomlbl = new QLabel(statusbar);
    zoomlbl->setObjectName("zoomlbl");
    zoomlbl->setText(QStringLiteral("100 %"));
    QToolButton *zin = new QToolButton(statusbar);
    zin->setObjectName("zin"); zin->setIcon(mkicon("plus"));
    QLabel *pathlbl = new QLabel(statusbar);
    pathlbl->setObjectName("pathlbl");
    pathlbl->setText(path);
    sbar->addWidget(zout); sbar->addWidget(zoomlbl); sbar->addWidget(zin); sbar->addWidget(pathlbl);
    vwin->addWidget(statusbar);

    // ---- Settings tab content (vendor §2): 4 checkboxes + date format + language ----
    struct St { bool autodetect = true, startauto = true, minauto = true, openlast = true;
                QString datefmt = QStringLiteral("DD.MM.YYYY"); int lang = 1; } st;
    QWidget *setdlg = new QWidget(&win);
    setdlg->setObjectName("setdlg");
    setdlg->hide();
    QVBoxLayout *setlay = new QVBoxLayout(setdlg);
    QLabel *setHdr = new QLabel(QStringLiteral("Settings"), setdlg);
    QCheckBox *cbDetect = new QCheckBox(setdlg); cbDetect->setObjectName("cbDetect");
    QCheckBox *cbStart  = new QCheckBox(setdlg); cbStart->setObjectName("cbStart");
    QCheckBox *cbMin    = new QCheckBox(setdlg); cbMin->setObjectName("cbMin");
    QCheckBox *cbOpen   = new QCheckBox(setdlg); cbOpen->setObjectName("cbOpen");
    QLabel *dateLbl = new QLabel(setdlg); dateLbl->setObjectName("dateLbl");
    QComboBox *dateCombo = new QComboBox(setdlg); dateCombo->setObjectName("dateCombo");
    dateCombo->addItems({QStringLiteral("DD.MM.YYYY"), QStringLiteral("MM/DD/YYYY"), QStringLiteral("YYYY-MM-DD")});
    QRadioButton *langDe = new QRadioButton(QStringLiteral("Deutsch"), setdlg);
    QRadioButton *langEn = new QRadioButton(QStringLiteral("English"), setdlg);
    setlay->addWidget(setHdr);
    setlay->addWidget(cbDetect); setlay->addWidget(cbStart); setlay->addWidget(cbMin); setlay->addWidget(cbOpen);
    setlay->addWidget(dateLbl); setlay->addWidget(dateCombo);
    setlay->addWidget(langDe); setlay->addWidget(langEn);
    dateLbl->setText(QStringLiteral("Date format: %1").arg(st.datefmt));

    // ---- dialogs (overlays, not part of the main window layout) ----
    QWidget *dlg = new QWidget(&win);
    dlg->setObjectName("dlg");
    dlg->hide();
    QVBoxLayout *dlv = new QVBoxLayout(dlg);
    QLabel *dlgTitle = new QLabel(dlg);
    QLineEdit *edit = new QLineEdit(dlg);
    QListWidget *layerW = new QListWidget(dlg);
    layerW->setMinimumSize(160, 24);
    QPushButton *addbtn = new QPushButton(dlg); addbtn->setObjectName("addbtn");
    QPushButton *delbtn = new QPushButton(dlg); delbtn->setObjectName("delbtn");
    QPushButton *renbtn = new QPushButton(dlg); renbtn->setObjectName("renbtn");
    QPushButton *cpybtn = new QPushButton(dlg); cpybtn->setObjectName("cpybtn");
    QSpinBox *spinl = new QSpinBox(dlg); spinl->setObjectName("spinl");
    dlv->addWidget(dlgTitle); dlv->addWidget(edit); dlv->addWidget(layerW);
    dlv->addWidget(addbtn); dlv->addWidget(delbtn); dlv->addWidget(renbtn); dlv->addWidget(cpybtn);
    dlv->addWidget(spinl);

    QWidget *knewdlg = new QWidget(&win);
    knewdlg->setObjectName("knewdlg");
    knewdlg->hide();
    QVBoxLayout *knv = new QVBoxLayout(knewdlg);
    QLabel *knq = new QLabel(knewdlg);
    QRadioButton *knAlways = new QRadioButton(knewdlg);
    QRadioButton *knProc = new QRadioButton(knewdlg);
    QLineEdit *procFld = new QLineEdit(knewdlg);
    QPushButton *knList = new QPushButton(knewdlg); knList->setObjectName("klist");
    QPushButton *knNext = new QPushButton(knewdlg); knNext->setObjectName("knext");
    knv->addWidget(knq); knv->addWidget(knAlways); knv->addWidget(knProc);
    knv->addWidget(procFld); knv->addWidget(knList); knv->addWidget(knNext);

    QWidget *dproc = new QWidget(&win);
    dproc->setObjectName("dproc");
    dproc->hide();
    QVBoxLayout *dpv = new QVBoxLayout(dproc);
    QListWidget *winW = new QListWidget(dproc); winW->setMinimumSize(240, 24);
    QComboBox *procPick = new QComboBox(dproc);
    QPushButton *winBtn = new QPushButton(dproc); winBtn->setObjectName("winprobe");
    dpv->addWidget(winW); dpv->addWidget(procPick); dpv->addWidget(winBtn);

    auto dlg_refresh = [&](int pk) {
        int cur = 0;
        for (const Map &mm : pm.maps) if (mm.poskey == pk) cur = mm.layers.size();
        dlgTitle->setText(QStringLiteral("POSKEY %1 - layers (%2):").arg(pk).arg(cur));
        QStringList rows;
        for (const Map &mm : pm.maps) if (mm.poskey == pk)
            for (int i = 0; i < mm.layers.size(); ++i) {
                const Layer &l = mm.layers[i];
                QString row = l.dependency.isEmpty()
                    ? QStringLiteral("Always '%1'").arg(l.value)
                    : QStringLiteral("%1 DEP=%2").arg(l.dependency).arg(l.value);
                if (i == spinl->value() - 1) row = QStringLiteral("> ") + row;
                rows << row;
            }
        layerW->clear();
        for (const QString &r : rows) layerW->addItem(r);
        spinl->setMaximum(qMax(1, cur));
        if (spinl->value() > cur) spinl->setValue(qMax(1, cur));
        bool alwaysSel = false;
        for (const Map &mm : pm.maps) if (mm.poskey == pk) {
            const int idx = spinl->value() - 1;
            if (idx >= 0 && idx < mm.layers.size()) alwaysSel = mm.layers[idx].dependency.isEmpty();
        }
        if (alwaysSel) { renbtn->hide(); delbtn->hide(); }
        else { renbtn->show(); delbtn->show(); }
        dlg->setGeometry(QRect(6, 6, qMin(460, win.width() - 12), 320));
        dlg->show();
    };
    auto open_dialog = [&](int pk) {
        g_pending_pk = pk;
        bool has = false;
        for (const Map &mm : pm.maps) if (mm.poskey == pk && !mm.layers.isEmpty()) has = true;
        if (!has) {
            knewdlg->setGeometry(QRect(6, 6, qMin(420, win.width() - 12), 240));
            knq->setText(QStringLiteral("Always, or only in one process?"));
            knewdlg->show();
            dlg->hide();
            return;
        }
        knewdlg->hide();
        dlg_refresh(pk);
    };
    g_openDlg = open_dialog;

    auto f_knnext = [&]() {
        const int pk = g_pending_pk;
        if (pk < 0) return;
        Layer nl; nl.value = edit->text();
        if (knProc->isChecked()) nl.dependency = procFld->text();
        bool found = false;
        for (Map &mm : pm.maps) if (mm.poskey == pk) { mm.layers << nl; found = true; break; }
        if (!found) { Map nm; nm.poskey = pk; nm.layers << nl; pm.maps << nm; }
        g_pending_pk = -1;
        dlg_refresh(pk);
    };
    knNext->connect(knNext, &QPushButton::clicked, [&]() { f_knnext(); });
    auto f_settings = [&]() {
        st.datefmt = dateCombo->currentText();
        st.lang = langEn->isChecked() ? 0 : 1;
        dateLbl->setText(QStringLiteral("Date format: %1").arg(st.datefmt));
        setdlg->setGeometry(QRect(6, 6, qMin(420, win.width() - 12), 320));
        setdlg->show();
    };
    for (QObject *ch : ribgrp[3]->children()) {
        QToolButton *b = qobject_cast<QToolButton *>(ch);
        if (b && b->objectName() == QStringLiteral("rib_settings"))
            b->connect(b, &QToolButton::clicked, [&]() { f_settings(); });
    }

    auto f_winprobe = [&]() {
        winW->clear();
        for (const auto &wp : pktm::list_windows(20))
            winW->addItem(QStringLiteral("%1 - pid %2").arg(wp.first).arg(wp.second));
        procPick->clear();
        for (const QString &pr : procs_of(pm)) procPick->addItem(pr);
        dproc->setGeometry(QRect(6, 6, qMin(420, win.width() - 12), 300));
        dproc->show();
    };
    knList->connect(knList, &QPushButton::clicked, [&]() { f_winprobe(); });
    winBtn->connect(winBtn, &QPushButton::clicked, [&]() { f_winprobe(); });

    auto setmin = [&]() {
        grid->setMinimumHeight(qMax(300, int(win.height() * 0.62)));
        grid->relayout();
    };
    setmin();
    win.show();

    int gfail = 0;
    if (hard) {
        QSet<QString> expect = {"menu", "accent", "ribbon", "mainrow", "statusbar"};
        QSet<QString> seen;
        for (QObject *ch : win.children()) {
            if (ch->objectName() == QStringLiteral("menu")) { seen.insert(QStringLiteral("menu")); continue; }
            QWidget *wq = qobject_cast<QWidget *>(ch);
            if (!wq || wq->isHidden()) continue;
            seen.insert(wq->objectName());
        }
        for (const QString &e : expect)
            if (!seen.contains(e)) { std::printf("HARD FAIL: part missing (%s)\n", e.toUtf8().constData()); ++gfail; }
        for (const QString &s2 : seen)
            if (!expect.contains(s2)) { std::printf("HARD FAIL: extra part (%s)\n", s2.toUtf8().constData()); ++gfail; }
        QSet<QString> rexp = {"sidebar", "grid"};
        QSet<QString> rseen;
        for (QObject *ch : mainrow->children()) {
            QWidget *wq = qobject_cast<QWidget *>(ch);
            if (!wq || wq->isHidden()) continue;
            rseen.insert(wq->objectName());
        }
        for (const QString &e : rexp)
            if (!rseen.contains(e)) { std::printf("HARD FAIL: row part missing (%s)\n", e.toUtf8().constData()); ++gfail; }
        for (const QString &s2 : rseen)
            if (!rexp.contains(s2)) { std::printf("HARD FAIL: row extra part (%s)\n", s2.toUtf8().constData()); ++gfail; }
        const double frac = double(grid->width() * grid->height()) / double(win.width() * win.height());
        if (frac < 0.45) { std::printf("HARD FAIL: grid only %.1f%% of window\n", frac * 100); ++gfail; }
        std::printf("hard: grid=%.1f%% parts ok=%d\n", frac * 100, gfail == 0 ? 1 : 0);
    }

    if (shot) {
        struct Sh { const char *f; int w, h; };
        const Sh shs[] = { {"/tmp/pkt_clean_1920.png", 1920, 1080}, {"/tmp/pkt_clean_1366.png", 1366, 768} };
        for (const Sh &sh : shs) {
            win.resize(sh.w, sh.h);
            setmin();
            if (getenv("PKT_SHOW_SETTINGS")) { setdlg->setGeometry(QRect(6, 6, qMin(420, sh.w - 12), 320)); setdlg->show(); }
            const QPixmap img = win.grab();
            if (!img.isNull() && img.save(sh.f)) std::printf("shot ok %s\n", sh.f);
        }
    }
    if (dlgategate && true) {
        int pk = -1;
        for (const CellRC &c : grid->cells) {
            bool has = false;
            for (const Map &mm : pm.maps) if (mm.poskey == c.poskey && !mm.layers.isEmpty()) has = true;
            if (!has) { pk = c.poskey; break; }
        }
        if (pk < 0) { std::printf("dlg gate: no empty key in file\n"); }
        else {
            open_dialog(pk);
            if (knewdlg->isHidden()) { std::printf("clicktest FAIL: no question\n"); ++gfail; }
            edit->setText(QStringLiteral("KN1"));
            f_knnext();
            if (dlg->isHidden()) { std::printf("clicktest FAIL: dialog not opened\n"); ++gfail; }
            if (layerW->count() < 1) { std::printf("clicktest FAIL: no layer rows\n"); ++gfail; }
            spinl->setValue(1);
            dlg_refresh(pk);
            if (!renbtn->isHidden() || !delbtn->isHidden()) { std::printf("clicktest FAIL: Always guard broken\n"); ++gfail; }
            f_winprobe();
            if (winW->count() < 1) { std::printf("clicktest FAIL: window list empty\n"); ++gfail; }
            st.datefmt = QStringLiteral("YYYY-MM-DD");
            dateCombo->setCurrentIndex(2);
            f_settings();
            if (setdlg->isHidden()) { std::printf("clicktest FAIL: settings dialog not shown\n"); ++gfail; }
            if (dateLbl->text() != QStringLiteral("Date format: YYYY-MM-DD")) { std::printf("clicktest FAIL: date format not applied\n"); ++gfail; }
            setdlg->hide();
            std::printf("dlg gate done\n");
        }
    }
    return gfail ? 1 : 0;
}
