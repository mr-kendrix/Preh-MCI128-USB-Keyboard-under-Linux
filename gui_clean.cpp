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
#include <QDateTime>
#include <filesystem>
#include <QDir>
#include <algorithm>
#include <vector>
#include <ctime>
#include <chrono>
#include <QAction>
#include <functional>
#include "pdfexport.h"
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
static const Pmap *g_pm = nullptr;     // live model for tile coloring
static int g_ctx_pk = -1;              // right-clicked poskey
static int g_ctx_count = 0;            // right-click counter (gate)
static QMenu *g_ctx_menu = nullptr;    // context menu
static QVector<Layer> g_clip;          // clipboard for context copy/paste
static std::function<void()> g_ctx_edit, g_ctx_copy, g_ctx_paste, g_ctx_del;

class KeyGrid : public QWidget
{
public:
    explicit KeyGrid(QWidget *parent = nullptr) : QWidget(parent) {}
    int sel = -1;
    QString curproc;                   // selected process: drives tile colors
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
            QColor brush(0xff, 0xff, 0xff);   // no function: light
            const Map *mm = nullptr;
            if (g_pm) for (const Map &m : g_pm->maps) if (m.poskey == c.poskey) { mm = &m; break; }
            if (mm) {
                brush = QColor(0xb4, 0xb8, 0xbe);              // no layers: unprogrammed gray
                bool mine = false, foreign = false;
                for (const Layer &l : mm->layers) {
                    if (l.dependency.isEmpty() || l.dependency == curproc) mine = true;
                    else foreign = true;
                }
                if (mine) brush = QColor(0xa6, 0xce, 0xa6);     // own process: green
                else if (foreign) brush = QColor(0x79, 0x7d, 0x96); // other process: blue-gray
            }
            if (c.poskey == sel) brush = QColor(0xff, 0xe0, 0x66); // selected: yellow
            pt.setBrush(brush);
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
    void mousePressEvent(QMouseEvent *ev) override
    {
        if (ev->button() == Qt::RightButton) {
            selectAt(ev->pos());
            g_ctx_pk = sel;
            ++g_ctx_count;
            if (g_ctx_menu && sel >= 0) g_ctx_menu->popup(ev->pos());
        }
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
    QMenu *fmenus[5] = { nullptr };
    for (int t = 0; t < 5; ++t)
        fmenus[t] = menu->addMenu(QString::fromLatin1(caps[t]));
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
    static QString undoDir;                      // undo/<timestamp>.pmap copies
    static int g_undo_seq = 0;                   // same-second saves stay distinct
    static QString recentFile;                   // recent list persisted here
    static QStringList g_recent;                 // newest first, max 8
    g_pm = &pm;
    undoDir = QDir(QDir::homePath()).filePath(QStringLiteral(".local/share/pkteditor/undo"));
    recentFile = QDir(QDir::homePath()).filePath(QStringLiteral(".local/share/pkteditor/recent.txt"));
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
    QLabel *phead = new QLabel(QString::fromUtf8("Process:"), sidebar); phead->setObjectName("phead");
    QComboBox *procSel = new QComboBox(sidebar); procSel->setObjectName("procSel");
    procSel->addItem(QStringLiteral("Always"));
    for (const QString &pn : procs_of(pm)) procSel->addItem(pn);
    sv->addWidget(logo); sv->addWidget(kpic); sv->addWidget(phead); sv->addWidget(procSel); sv->addWidget(shint);

    KeyGrid *grid = new KeyGrid(mainrow);
    grid->setObjectName("grid");
    g_ctx_menu = new QMenu(grid);
    g_ctx_menu->setObjectName("ctxmenu");
    {   auto mkctx = [&](const QString &t, std::function<void()> fn) {
            QAction *a = new QAction(t, g_ctx_menu);
            a->setObjectName(QStringLiteral("ctx:") + t);
            a->connect(a, &QAction::triggered, [fn]{ if (fn) fn(); });
        };
        auto eEdit = [&]{ if (g_openDlg && g_ctx_pk >= 0) g_openDlg(g_ctx_pk); };
        auto eCopy = [&]{ g_clip.clear();
            for (const Map &m : pm.maps) if (m.poskey == g_ctx_pk) { g_clip = m.layers; break; } };
        auto ePaste = [&]{ if (g_clip.isEmpty()) return;
            for (Map &m : pm.maps) if (m.poskey == g_ctx_pk) { m.layers = g_clip; break; } };
        auto eDel = [&]{ for (Map &m : pm.maps) if (m.poskey == g_ctx_pk) { m.layers.clear(); break; } };
        g_ctx_edit = eEdit; g_ctx_copy = eCopy; g_ctx_paste = ePaste; g_ctx_del = eDel;
        mkctx("Edit", eEdit); mkctx("Copy", eCopy); mkctx("Paste", ePaste); mkctx("Delete", eDel);
    }
    grid->setMinimumSize(480, 320);
    mrow->addWidget(sidebar, 1);
    mrow->addWidget(grid, 4);
    procSel->setCurrentIndex(0);
    grid->curproc = procSel->currentText();
    auto f_proc = [&]() { grid->curproc = procSel->currentText(); };
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
    // persistence of settings (§2) — same ini as legacy
    static QString setIni;
    auto save_set = [&]() {
        QFile sf(setIni);
        if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        sf.write((QStringLiteral("autodetect=") + QString::number(st.autodetect ? 1 : 0) + "\n"
                  "startauto=" + QString::number(st.startauto ? 1 : 0) + "\n"
                  "minauto=" + QString::number(st.minauto ? 1 : 0) + "\n"
                  "openlast=" + QString::number(st.openlast ? 1 : 0) + "\n"
                  "datefmt=" + st.datefmt + "\n"
                  "lang=" + QString::number(st.lang) + "\n").toUtf8());
        sf.close();
    };
    auto load_set = [&]() {
        setIni = QDir(QDir::homePath()).filePath(QStringLiteral(".local/share/pkteditor/settings.ini"));
        QFile sf(setIni);
        if (!sf.open(QIODevice::ReadOnly | QIODevice::Text)) return;
        while (!sf.atEnd()) {
            const QString line = QString::fromUtf8(sf.readLine()).trimmed();
            const int eq = line.indexOf('=');
            if (eq <= 0) continue;
            const QString k = line.left(eq), v = line.mid(eq + 1);
            if (k == "autodetect") st.autodetect = v == "1";
            else if (k == "startauto") st.startauto = v == "1";
            else if (k == "minauto") st.minauto = v == "1";
            else if (k == "openlast") st.openlast = v == "1";
            else if (k == "datefmt") st.datefmt = v;
            else if (k == "lang") st.lang = v.toInt();
        }
        sf.close();
    };
    load_set();
    cbDetect->setChecked(st.autodetect); cbStart->setChecked(st.startauto);
    cbMin->setChecked(st.minauto); cbOpen->setChecked(st.openlast);
    cbDetect->connect(cbDetect, &QCheckBox::stateChanged, [&](int x){ st.autodetect = (x & Qt::Checked) != 0; save_set(); });
    cbStart->connect(cbStart, &QCheckBox::stateChanged, [&](int x){ st.startauto = (x & Qt::Checked) != 0; save_set(); });
    cbMin->connect(cbMin, &QCheckBox::stateChanged, [&](int x){ st.minauto = (x & Qt::Checked) != 0; save_set(); });
    cbOpen->connect(cbOpen, &QCheckBox::stateChanged, [&](int x){ st.openlast = (x & Qt::Checked) != 0; save_set(); });


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
    QPushButton *delKeyBtn = new QPushButton(dlg); delKeyBtn->setObjectName("delKeyBtn");
    QPushButton *cancelBtn = new QPushButton(dlg); cancelBtn->setObjectName("cancelBtn");
    QPushButton *saveBtn  = new QPushButton(dlg); saveBtn->setObjectName("saveBtn");
    QPushButton *undoBtn  = new QPushButton(dlg); undoBtn->setObjectName("undoBtn");
    QSpinBox *spinl = new QSpinBox(dlg); spinl->setObjectName("spinl");
    // vendor §5 right column: Appearance (Key/Caption color, caption, font) + Function radios
    QLineEdit *capFld = new QLineEdit(dlg); capFld->setObjectName("capFld");
    QCheckBox *btfFld = new QCheckBox(dlg); btfFld->setObjectName("btfFld");
    QCheckBox *repFld = new QCheckBox(dlg); repFld->setObjectName("repFld");
    QCheckBox *slwFld = new QCheckBox(dlg); slwFld->setObjectName("slwFld");
    QComboBox *forePick = new QComboBox(dlg); forePick->setObjectName("forePick");
    QComboBox *backPick = new QComboBox(dlg); backPick->setObjectName("backPick");
    for (int i = 0; i < 112; ++i) { const QString nm = QStringLiteral("Slot %1").arg(i);
        forePick->addItem(nm); backPick->addItem(nm); }
    QRadioButton *fnNone = new QRadioButton(QStringLiteral("Do nothing"), dlg); fnNone->setObjectName("fnNone");
    QRadioButton *fnText = new QRadioButton(QStringLiteral("Text-Shortcut"), dlg); fnText->setObjectName("fnText");
    QRadioButton *fnApp  = new QRadioButton(QStringLiteral("Application"), dlg); fnApp->setObjectName("fnApp");
    QRadioButton *fnWeb  = new QRadioButton(QStringLiteral("Web"), dlg); fnWeb->setObjectName("fnWeb");
    QLabel *appearLbl = new QLabel(dlg); appearLbl->setObjectName("appearLbl");
    // vendor 6.10 Application dialog fields + 6.12 Web fields (inline in dlg)
    QLineEdit *appPath = new QLineEdit(dlg); appPath->setObjectName("appPath");
    QLineEdit *appPars = new QLineEdit(dlg); appPars->setObjectName("appPars");
    QLineEdit *appShort = new QLineEdit(dlg); appShort->setObjectName("appShort");
    QLineEdit *webLink = new QLineEdit(dlg); webLink->setObjectName("webLink");
    QSpinBox *webTo = new QSpinBox(dlg); webTo->setObjectName("webTo"); webTo->setMinimum(1); webTo->setMaximum(120);
    QLineEdit *webPars = new QLineEdit(dlg); webPars->setObjectName("webPars");
    dlv->addWidget(appearLbl);
    dlv->addWidget(capFld); dlv->addWidget(forePick); dlv->addWidget(backPick);
    dlv->addWidget(btfFld); dlv->addWidget(repFld); dlv->addWidget(slwFld);
    dlv->addWidget(fnNone); dlv->addWidget(fnText); dlv->addWidget(fnApp); dlv->addWidget(fnWeb);
    dlv->addWidget(appPath); dlv->addWidget(appPars); dlv->addWidget(appShort);
    dlv->addWidget(webLink); dlv->addWidget(webTo); dlv->addWidget(webPars);
    dlv->addWidget(delKeyBtn); dlv->addWidget(cancelBtn); dlv->addWidget(saveBtn); dlv->addWidget(undoBtn);
    dlv->addWidget(spinl);

    // ---- Restore dialog: timestamped backups (vendor Restore folder list) ----
    QWidget *rdlg = new QWidget(&win);
    rdlg->setObjectName("rdlg");
    rdlg->hide();
    QVBoxLayout *rlv = new QVBoxLayout(rdlg);
    QLabel *rHdr = new QLabel(rdlg); rHdr->setObjectName("rHdr");
    QListWidget *rList = new QListWidget(rdlg); rList->setObjectName("rList");
    static QStringList g_backups;                // names newest-first
    QPushButton *rPick = new QPushButton(rdlg); rPick->setObjectName("rPick");
    rlv->addWidget(rHdr); rlv->addWidget(rList); rlv->addWidget(rPick);

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

    static QVector<Map> g_snap;                   // snapshot for Cancel
    static int pk_cur = -1;                      // key of the open dialog
    static QVector<Layer> g_layers;              // view of the open dialog's layers
    static QStringList g_deps;                   // parallel: dependency per layer
    auto dlg_refresh = [&](int pk) {
        int cur = 0;
        for (const Map &mm : pm.maps) if (mm.poskey == pk) cur = mm.layers.size();
        dlgTitle->setText(QStringLiteral("POSKEY %1 - layers (%2):").arg(pk).arg(cur));
        QStringList rows;
        g_layers.clear(); g_deps.clear();
        for (const Map &mm : pm.maps) if (mm.poskey == pk)
            for (int i = 0; i < mm.layers.size(); ++i) {
                const Layer &l = mm.layers[i];
                g_layers << l; g_deps << l.dependency;
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
        pk_cur = pk;
        g_snap = pm.maps;
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
    static QString g_copy;                       // vendor: copy still pending
    static QString g_newtype;                    // type from the function radio for the next add
    static int g_row_hint = -1;                  // gate: explicit layer index
    auto f_copy = [&]() { const int i = layerW->currentRow();
        if (i >= 0 && i < g_layers.size()) g_copy = g_layers[i].value; };
    auto f_rename = [&]() { const int i = (g_row_hint >= 0 ? g_row_hint : layerW->currentRow());
        if (pk_cur >= 0 && i >= 0) for (Map &mm : pm.maps) if (mm.poskey == pk_cur && i < mm.layers.size()) { mm.layers[i].value = edit->text(); break; }
        dlg_refresh(pk_cur); };
    auto f_delete = [&]() { const int i = (g_row_hint >= 0 ? g_row_hint : layerW->currentRow());
        if (pk_cur >= 0 && i >= 0) for (Map &mm : pm.maps) if (mm.poskey == pk_cur && i < mm.layers.size()) { mm.layers.remove(i); break; }
        dlg_refresh(pk_cur); };
    auto f_appear = [&]() {
        for (Map &mm : pm.maps) if (mm.poskey == pk_cur) {
            if (!capFld->text().isEmpty()) mm.print_text = capFld->text();
            const int fi = forePick->currentIndex(), bi = backPick->currentIndex();
            if (fi >= 0) mm.fore = fi;
            if (bi >= 0) mm.back = bi;
            break;
        }
        dlg_refresh(pk_cur);
    };
    auto f_flags = [&]() { const int i = (g_row_hint >= 0 ? g_row_hint : layerW->currentRow());
        if (pk_cur >= 0 && i >= 0) for (Map &mm : pm.maps) if (mm.poskey == pk_cur && i < mm.layers.size()) {
            mm.layers[i].btf  = btfFld->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
            mm.layers[i].rep  = repFld->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
            mm.layers[i].slow = slwFld->isChecked() ? QStringLiteral("true") : QStringLiteral("false");
            break;
        }
        dlg_refresh(pk_cur);
    };
    auto f_fnapply = [&]() { const int i = (g_row_hint >= 0 ? g_row_hint : layerW->currentRow());
        if (pk_cur < 0 || i < 0) return;
        for (Map &mm : pm.maps) if (mm.poskey == pk_cur && i < mm.layers.size()) {
            Layer &l = mm.layers[i];
            if (l.type == QStringLiteral("APPLICATION") && !appPath->text().isEmpty()) {
                QString v = appPath->text();
                if (!appPars->text().isEmpty()) v += QStringLiteral("::") + appPars->text();
                l.value = v;
            } else if (l.type == QStringLiteral("WEBSITE") && !webLink->text().isEmpty()) {
                l.value = webLink->text();
            }
            break;
        }
        dlg_refresh(pk_cur);
    };
    auto f_settype = [&](const QString &t) { g_newtype = t; };
    auto f_applytype = [&]() { const int i = (g_row_hint >= 0 ? g_row_hint : layerW->currentRow());
        for (Map &mm : pm.maps) if (mm.poskey == pk_cur && i >= 0 && i < mm.layers.size()) { mm.layers[i].type = g_newtype; break; }
        dlg_refresh(pk_cur);
    };
    auto f_addlayer = [&]() { if (pk_cur < 0) return; Layer nl; nl.value = edit->text(); nl.type = g_newtype.isEmpty() ? QStringLiteral("TEXT") : g_newtype;
        for (Map &mm : pm.maps) if (mm.poskey == pk_cur) { mm.layers << nl; break; }
        dlg_refresh(pk_cur); };
    addbtn->connect(addbtn, &QToolButton::clicked, [&]() { f_addlayer(); });
    renbtn->connect(renbtn, &QToolButton::clicked, [&]() { f_rename(); });
    delbtn->connect(delbtn, &QToolButton::clicked, [&]() { f_delete(); });
    cpybtn->connect(cpybtn, &QToolButton::clicked, [&]() { f_copy(); });
    fnNone->connect(fnNone, &QRadioButton::clicked, [&]() { f_settype(QStringLiteral("NOTHING")); });
    fnText->connect(fnText, &QRadioButton::clicked, [&]() { f_settype(QStringLiteral("TEXT")); });
    fnApp->connect(fnApp, &QRadioButton::clicked, [&]() { f_settype(QStringLiteral("APPLICATION")); });
    fnWeb->connect(fnWeb, &QRadioButton::clicked, [&]() { f_settype(QStringLiteral("WEBSITE")); });
    auto f_app_appear = [&]() { f_appear(); };
    auto f_delkey = [&]() { for (int i = pm.maps.size() - 1; i >= 0; --i) if (pm.maps[i].poskey == pk_cur) pm.maps.remove(i);
        dlg->hide(); };
    auto f_cancel = [&]() { pm.maps = g_snap; dlg_refresh(pk_cur); };
    auto f_backup = [&]() {
        QFile rf(path);
        if (!rf.open(QIODevice::ReadOnly)) return;
        const QByteArray pre = rf.readAll(); rf.close();
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(undoDir.toStdString()), ec);
        const long secs = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const QString stamp = QStringLiteral("undo-") + QString::number(secs) + '-' + QString::number(++g_undo_seq);
        QFile wf(undoDir + QLatin1Char('/') + stamp + QStringLiteral(".pmap"));
        if (wf.open(QIODevice::WriteOnly)) { wf.write(pre); wf.close(); }
    };
    auto f_save = [&]() {
        f_backup();
        const QString txt = serialize_pmap(pm);
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) { f.write(txt.toUtf8()); f.close(); }
        else { std::printf("dlg FAIL: save open %s\n", path.toUtf8().constData()); }
    };
    auto f_undo = [&]() {
        std::error_code ec;
        std::vector<std::string> names;
        for (const auto &e : std::filesystem::directory_iterator(std::filesystem::path(undoDir.toStdString()), ec))
            if (e.path().filename().string().size() > 4) names.push_back(e.path().filename().string());
        if (names.empty()) { std::printf("undo empty\n"); return; }
        std::sort(names.begin(), names.end());
        const QString latest = undoDir + QLatin1Char('/') + QString::fromStdString(names.back());
        QFile rf(latest);
        if (!rf.open(QIODevice::ReadOnly)) { std::printf("undo read fail\n"); return; }
        const QByteArray content = rf.readAll(); rf.close();
        QFile wf(path);
        if (!wf.open(QIODevice::WriteOnly)) { std::printf("undo write fail\n"); return; }
        wf.write(content); wf.close();
        pm = parse_pmap(QString::fromUtf8(content));
    };
    auto f_app_applytype = [&]() { f_applytype(); };
    auto f_refresh_backups = [&]() {
        rList->clear();
        std::error_code ec;
        std::vector<std::string> names;
        for (const auto &e : std::filesystem::directory_iterator(std::filesystem::path(undoDir.toStdString()), ec))
            names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end(), std::greater<std::string>());
        g_backups.clear();
        for (const auto &n : names) { g_backups << QString::fromStdString(n); rList->addItem(QString::fromStdString(n)); }
        rHdr->setText(QStringLiteral("Restore (%1):").arg(rList->count()));
    };
    auto f_restore = [&]() {
        const int i = rList->currentRow();
        if (i < 0 || i >= g_backups.size()) return;
        const QString latest = g_backups[i];
        QFile rf(undoDir + QLatin1Char('/') + latest);
        if (!rf.open(QIODevice::ReadOnly)) { std::printf("restore read fail\n"); return; }
        const QByteArray content = rf.readAll(); rf.close();
        QFile wf(path);
        if (!wf.open(QIODevice::WriteOnly)) { std::printf("restore write fail\n"); return; }
        wf.write(content); wf.close();
        pm = parse_pmap(QString::fromUtf8(content));
    };
    rPick->connect(rPick, &QPushButton::clicked, [&]() { f_restore(); });
    auto f_openrestore = [&]() {
        f_refresh_backups();
        rdlg->setGeometry(QRect(6, 6, qMin(420, win.width() - 12), 300));
        rdlg->show();
    };
    auto f_close_restore = [&]() { rdlg->hide(); };
    auto load_recent = [&]() {
        QFile rf(recentFile);
        if (rf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            g_recent.clear();
            while (!rf.atEnd()) { const QString l = QString::fromUtf8(rf.readLine()).trimmed(); if (!l.isEmpty()) g_recent << l; }
            rf.close();
        }
    };
    auto save_recent = [&]() {
        QFile rf(recentFile);
        if (!rf.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        for (int i = 0; i < g_recent.size() && i < 8; ++i) rf.write((g_recent[i] + "\n").toUtf8());
        rf.close();
    };
    auto push_recent = [&](const QString &p) {
        g_recent.removeAll(p);
        g_recent.prepend(p);
        save_recent();
    };
    auto do_open_file = [&](const QString &p) {
        QFile of(p);
        if (!of.open(QIODevice::ReadOnly)) return;
        pm = parse_pmap(of.readAll()); of.close();
        path = p;
        g_pm = &pm;
        pathlbl->setText(path);
        grid->relayout();
    };
    auto f_undo2 = [&]() { f_undo(); };
    auto mk_item = [&](QMenu *mnu, const QString &txt, const std::function<void()> &fn) {
        QAction *a = new QAction(txt, mnu);
        a->setObjectName(QStringLiteral("menu:") + txt);
        QObject::connect(a, &QAction::triggered, [fn]{ if (fn) fn(); });
    };
    load_recent();
    if (!path.isEmpty()) push_recent(path);
    if (fmenus[0]) {
        mk_item(fmenus[0], QStringLiteral("Restore"), [&]{ f_openrestore(); });
        mk_item(fmenus[0], QStringLiteral("Close"), [&]{ if (dlg) dlg->hide(); });
        for (const QString &rp : g_recent) {
            const QString disp = rp.section(QChar('/'), -1);
            const QString actual = rp;
            mk_item(fmenus[0], disp, [&]{ do_open_file(actual); });
        }
    }
    auto f_export = [&]() {
        QVector<std::pair<int, QString>> rows, rt;
        for (const CellRC &c : grid->cells) {
            const Map *mm = nullptr;
            for (const Map &m2 : pm.maps) if (m2.poskey == c.poskey) { mm = &m2; break; }
            rows << std::make_pair(c.poskey, (mm && !mm->layers.isEmpty()) ? mm->layers.first().value : QStringLiteral("-"));
            rt << std::make_pair(c.poskey, (mm && !mm->layers.isEmpty()) ? mm->layers.first().type : QString());
        }
        std::printf("export dbg: rows=%d geom=%d\n", rows.size(), g_geom.size());
        if (rows.isEmpty()) return;
        const QString stem = path.isEmpty() ? QStringLiteral("pkt-export") : path;
        QFile pf(stem + QStringLiteral(".labels.pdf"));
        if (pf.open(QIODevice::WriteOnly)) { pf.write(pdfx::make_label_pdf(rows)); pf.close(); }
        QHash<int, QString> texts, fonts;
        for (const Map &m2 : pm.maps) { if (!m2.print_text.isEmpty()) texts.insert(m2.poskey, m2.print_text); if (!m2.fontdesc.isEmpty()) fonts.insert(m2.poskey, m2.fontdesc); }
        QFile p2(stem + QStringLiteral(".geom.pdf"));
        if (p2.open(QIODevice::WriteOnly)) { p2.write(pdfx::make_label_pdf_geom(rows, rt, texts, fonts)); p2.close(); }
        QFile p3(stem + QStringLiteral(".cut.pdf"));
        if (p3.open(QIODevice::WriteOnly)) { p3.write(pdfx::make_cut_sheet(texts, fonts)); p3.close(); }
    };
    if (fmenus[1]) {
        mk_item(fmenus[1], QStringLiteral("Undo"), [&]{ f_undo2(); });
        mk_item(fmenus[1], QStringLiteral("Export labels (PDF)"), [&]() { f_export(); });
    }
    undoBtn->connect(undoBtn, &QPushButton::clicked, [&]() { f_undo2(); });
    delKeyBtn->connect(delKeyBtn, &QPushButton::clicked, [&]() { f_delkey(); });
    cancelBtn->connect(cancelBtn, &QPushButton::clicked, [&]() { f_cancel(); });
    saveBtn->connect(saveBtn, &QPushButton::clicked, [&]() { f_save(); });

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
        if (frac < 0.40) { std::printf("HARD FAIL: grid only %.1f%% of window\n", frac * 100); ++gfail; }
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
            {   // vendor: rename a dependency layer works when spin > 1
                for (Map &mm : pm.maps) if (mm.poskey == pk && mm.layers.size() > 1) break;
                // ensure a dependency layer exists: add one via the question flow again
                g_pending_pk = pk;
                knProc->setChecked(true);
                procFld->setText(QStringLiteral("Notepad"));
                f_knnext();
                dlg_refresh(pk);
                if (!g_layers.isEmpty()) layerW->setCurrentRow(g_layers.size() - 1);
                const int row = qBound(0, layerW->currentRow(), g_layers.size() - 1);
                if (g_layers.isEmpty()) { std::printf("clicktest FAIL: no layers\\n"); ++gfail; }
                else {
                const QString before = g_layers[row].value;
                Q_UNUSED(before);
                edit->setText(QStringLiteral("Renamed"));
                g_row_hint = row;
                f_rename();

                if (g_layers.isEmpty() || g_layers[qBound(0, row, g_layers.size() - 1)].value != QStringLiteral("Renamed")) { std::printf("clicktest FAIL: rename not applied\n"); ++gfail; }
                Q_UNUSED(before);
                g_row_hint = row;
                f_delete();
                dlg_refresh(pk);
                if (layerW->count() != g_layers.size()) { std::printf("clicktest FAIL: delete not mirrored\n"); ++gfail; }
                // appearance: caption + colors applied to the Map
                capFld->setText(QStringLiteral("GT-CAP"));
                forePick->setCurrentIndex(5);
                backPick->setCurrentIndex(7);
                f_app_appear();
                bool appOk = false;
                for (const Map &mm : pm.maps) if (mm.poskey == pk) { appOk = (mm.print_text == QStringLiteral("GT-CAP") && mm.fore == 5 && mm.back == 7); break; }
                if (!appOk) { std::printf("clicktest FAIL: appearance not applied\n"); ++gfail; }
                // function radio WEBSITE -> next add gets type WEBSITE
                f_settype(QStringLiteral("WEBSITE"));
                g_row_hint = -1; layerW->clear(); // row hint reset
                f_addlayer();
                bool webOk = false;
                for (const Map &mm : pm.maps) if (mm.poskey == pk) { webOk = (!mm.layers.isEmpty() && mm.layers.last().type == QStringLiteral("WEBSITE")); break; }
                if (!webOk) { std::printf("clicktest FAIL: web layer type not stored\n"); ++gfail; }
                // right-click context menu: Edit/Copy/Paste/Delete via g_ctx_*
                if (g_ctx_pk < 0) g_ctx_pk = pk;
                { int have = 0;
                  for (QObject *ch : g_ctx_menu->children()) {
                      QAction *a = qobject_cast<QAction *>(ch);
                      if (!a) continue;
                      const QString n = a->objectName();
                      if (n == QStringLiteral("ctx:Edit") || n == QStringLiteral("ctx:Copy") ||
                          n == QStringLiteral("ctx:Paste") || n == QStringLiteral("ctx:Delete")) ++have;
                  }
                  if (have != 4) { std::printf("clicktest FAIL: ctx menu items missing\n"); ++gfail; } }
                g_ctx_copy();
                if (g_clip.isEmpty()) { std::printf("clicktest FAIL: copy empty\n"); ++gfail; }
                g_ctx_paste();
                g_ctx_del();
                for (const Map &m : pm.maps) if (m.poskey == g_ctx_pk && !m.layers.isEmpty()) { std::printf("clicktest FAIL: ctx delete left layers\n"); ++gfail; }
                // gate: export dbg rows>0? not needed here
                g_newtype.clear();
                // footer: Delete-all removes the key, Cancel restores the snapshot, Save round-trips
                bool found = false;
                for (const Map &mm : pm.maps) if (mm.poskey == pk) found = true;
                if (!found) { std::printf("clicktest FAIL: key already gone\n"); ++gfail; }
                pk_cur = pk;
                f_delkey();
                bool still = false;
                for (const Map &mm : pm.maps) if (mm.poskey == pk) still = true;
                if (still) { std::printf("clicktest FAIL: delete-all not removing\n"); ++gfail; }
                f_cancel();
                { bool same = (pm.maps.size() == g_snap.size());
                  for (int q = 0; q < pm.maps.size() && same; ++q) same = (pm.maps[q].poskey == g_snap[q].poskey && pm.maps[q].layers.size() == g_snap[q].layers.size());
                  if (!same) { std::printf("clicktest FAIL: cancel not restoring\n"); ++gfail; } }
                f_save();
                { QFile vf(path);
                  if (!vf.open(QIODevice::ReadOnly)) { std::printf("clicktest FAIL: saved file unreadable\n"); ++gfail; }
                  else { const Pmap bk = parse_pmap(vf.readAll()); vf.close();
                         if (bk.maps.size() != pm.maps.size()) { std::printf("clicktest FAIL: save roundtrip count differs\n"); ++gfail; } }
                }
                // per-layer flags: Bring to front / AutoRepeat / SlowOutput
                if (!g_layers.isEmpty()) {
                    btfFld->setChecked(true); repFld->setChecked(false); slwFld->setChecked(false);
                    g_row_hint = qMin(row, g_layers.size() - 1);
                    f_flags();
                    const int fi = qBound(0, g_row_hint, g_layers.size() - 1);
                    { bool ok = false;
                      for (const Map &mm : pm.maps) if (mm.poskey == pk && fi < mm.layers.size()) { ok = (mm.layers[fi].btf == QStringLiteral("true")); break; }
                      if (!ok) { std::printf("clicktest FAIL: btf flag not set\n"); ++gfail; } }
                    f_save();
                    { QFile vf2(path);
                      bool foundb = false;
                      if (vf2.open(QIODevice::ReadOnly)) { const QString all = vf2.readAll(); vf2.close();
                        const QString hx = to_hex_cp1252(QStringLiteral("true"));
                        foundb = all.contains(QStringLiteral("<BringToForeground>")) && all.contains(hx); }
                      if (!foundb) { std::printf("clicktest FAIL: btf not serialized\n"); ++gfail; } }
                    btfFld->setChecked(false);
                    g_row_hint = fi;
                    f_flags();
                    {
                      const int fi2 = fi;
                      webLink->setText(QStringLiteral("https://undo.test"));
                      appPath->setText(QStringLiteral("/usr/bin/soffice"));
                      f_fnapply();
                      const QString want = QStringLiteral("https://undo.test");
                      bool valOk = false;
                      for (const Map &mm : pm.maps) if (mm.poskey == pk && fi2 < mm.layers.size())
                          valOk = (mm.layers[fi2].value == want);
                      if (!valOk) { std::printf("clicktest FAIL: web link not applied\n"); ++gfail; }
                      f_save();
                      QFile vf3(path);
                      bool linkIn = false;
                      if (vf3.open(QIODevice::ReadOnly)) { const QString all = vf3.readAll(); vf3.close();
                          linkIn = all.contains(to_hex_cp1252(want)); }
                      if (!linkIn) { std::printf("clicktest FAIL: link not serialized\n"); ++gfail; }
                    }
                    { bool ok = false;
                      for (const Map &mm : pm.maps) if (mm.poskey == pk && fi < mm.layers.size()) { ok = (mm.layers[fi].btf == QStringLiteral("false")); break; }
                      if (!ok) { std::printf("clicktest FAIL: btf clear not applied\n"); ++gfail; } }
                }
                // vendor Undo: second save then restore newest backup
                {
                    edit->setText(QStringLiteral("UNDO-MARK"));
                    f_save();                       // copies pre-save content to undo dir
                    std::error_code ec;
                    int nBak = 0; QString latest;
                    for (const auto &ent : std::filesystem::directory_iterator(std::filesystem::path(undoDir.toStdString()), ec))
                        if (ent.path().filename().extension() == ".pmap") { ++nBak; if (latest < QString::fromUtf8(ent.path().filename().string())) latest = QString::fromUtf8(ent.path().filename().string()); }
                    if (nBak < 2) { std::printf("clicktest FAIL: undo backups %d < 2\n", nBak); ++gfail; }
                    const QByteArray want = [&]{ QFile f(undoDir + QStringLiteral("/") + latest);
                        if (f.open(QIODevice::ReadOnly)) { QByteArray b = f.readAll(); f.close(); return b; } return QByteArray(); }();
                    f_undo();
                    QFile vf2(path);
                    if (!vf2.open(QIODevice::ReadOnly)) { std::printf("clicktest FAIL: undo file unreadable\n"); ++gfail; }
                    else { const QByteArray now = vf2.readAll(); vf2.close();
                           if (now != want) { std::printf("clicktest FAIL: undo content mismatch\n"); ++gfail; } }
                }
                }
            }
                f_openrestore();
                if (rdlg->isHidden()) { std::printf("clicktest FAIL: restore dlg not shown\n"); ++gfail; }
                if (rList->count() < 2) { std::printf("clicktest FAIL: restore list < 2\n"); ++gfail; }
                rList->setCurrentRow(0);
                f_restore();
                { const QString newest = g_backups.isEmpty() ? QString() : g_backups[0];
                  QFile bf(undoDir + QLatin1Char('/') + newest);
                  QByteArray want; if (bf.open(QIODevice::ReadOnly)) { want = bf.readAll(); bf.close(); }
                  QFile tf(path); QByteArray now; if (tf.open(QIODevice::ReadOnly)) { now = tf.readAll(); tf.close(); }
                  if (want != now) { std::printf("clicktest FAIL: restore content mismatch\n"); ++gfail; } }
                f_close_restore();
                { int qacts = 0;
                  for (QObject *ch : fmenus[0]->children()) if (qobject_cast<QAction *>(ch)) ++qacts;
                  if (qacts < 3) { std::printf("clicktest FAIL: File menu items %d < 3\n", qacts); ++gfail; }
                  QFile rf(recentFile); bool hasP = false;
                  if (rf.open(QIODevice::ReadOnly)) { hasP = rf.readAll().contains(path.toUtf8()); rf.close(); }
                  if (!hasP) { std::printf("clicktest FAIL: recent list lacks current file\n"); ++gfail; }
                }
                { f_export();
                  for (const QString &ext : {QStringLiteral(".labels.pdf"), QStringLiteral(".geom.pdf"), QStringLiteral(".cut.pdf")}) {
                      QFile pf(path + ext);
                      if (!pf.open(QIODevice::ReadOnly)) { std::printf("clicktest FAIL: export %s missing\n", ext.toUtf8().constData()); ++gfail; }
                      else { const QByteArray h = pf.read(4); pf.close();
                             if (h != "%PDF") { std::printf("clicktest FAIL: export %s not pdf\n", ext.toUtf8().constData()); ++gfail; } }
                  }
                }
            f_winprobe();
            if (winW->count() < 1) { std::printf("clicktest FAIL: window list empty\n"); ++gfail; }
            {   // process selection recolors tiles
                if (procSel->count() < 2) { std::printf("clicktest FAIL: no processes to select\n"); ++gfail; }
                else {
                    procSel->setCurrentIndex(1);
                    f_proc();
                    const QImage shot = win.grab().toImage();
                    int green = 0, blue = 0;
                    for (int y = 0; y < shot.height(); y += 3)
                        for (int x = 0; x < shot.width(); x += 3) {
                            const unsigned rgb = shot.pixel(x, y);
                            if (qAbs(((rgb >> 16) & 0xff) - 0xa6) < 8 && qAbs(((rgb >> 8) & 0xff) - 0xce) < 8 && qAbs((rgb & 0xff) - 0xa6) < 8) ++green;
                            if (qAbs(((rgb >> 16) & 0xff) - 0x79) < 8 && qAbs(((rgb >> 8) & 0xff) - 0x7d) < 8 && qAbs((rgb & 0xff) - 0x96) < 8) ++blue;
                        }
                    std::printf("proc recolor: green=%d blue=%d\n", green, blue);
                    if (green + blue < 4) { std::printf("clicktest FAIL: no colored tiles for '%s'\n", procSel->currentText().toUtf8().constData()); ++gfail; }
                    procSel->setCurrentIndex(0);
                    f_proc();
                }
            }
            st.datefmt = QStringLiteral("YYYY-MM-DD");
            dateCombo->setCurrentIndex(2);
            f_settings();
            if (setdlg->isHidden()) { std::printf("clicktest FAIL: settings dialog not shown\n"); ++gfail; }
            if (dateLbl->text() != QStringLiteral("Date format: YYYY-MM-DD")) { std::printf("clicktest FAIL: date format not applied\n"); ++gfail; }
            setdlg->hide();
            cbDetect->setChecked(false); cbStart->setChecked(false);
            { QFile sf(setIni);
              bool ok1 = false, ok2 = false;
              if (sf.open(QIODevice::ReadOnly)) { const QString all = sf.readAll(); sf.close();
                ok1 = all.contains("autodetect=0"); ok2 = all.contains("startauto=0"); }
              if (!ok1 || !ok2) { std::printf("clicktest FAIL: settings not persisted\n"); ++gfail; } }
            cbDetect->setChecked(true); cbStart->setChecked(true);
            std::printf("dlg gate done\n");
        }
    }
    return gfail ? 1 : 0;
}
