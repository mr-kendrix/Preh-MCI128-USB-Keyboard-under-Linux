// pkteditor — pmap editor frontend (Qt6 Widgets)
#include <QApplication>
#include <QWidget>
#include <QLabel>
#include <QFile>
#include <cstdio>
#include <cstdlib>
#include "pmap.h"

static const char *ROWS[] = {"H", "G", "F", "E", "D", "C", "B", "A"};
static const int ROWS_N[] = {15, 16, 13, 12, 12, 12, 16, 14};

int main(int argc, char **argv) {
    Pmap pm;
    QString path;
    if (argc > 1) path = argv[1];
    if (path.size() > 0) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly))
            pm = parse_pmap(f.readAll());
    }
    if (getenv("GUI_SMOKE")) {
        std::fprintf(stderr, "rows=%d maps=%zu\n", 8, pm.maps.size());
        return 0;
    }
    QApplication app(argc, argv);
    QWidget win;
    QLabel head(&win);
    head.setText(pm.name.size() == 0
                     ? QStringLiteral("pkteditor — no pmap loaded")
                     : pm.name + QStringLiteral("  maps=%1").arg(pm.maps.size()));
    for (int r = 0; r < 8; ++r) {
        QLabel *l = new QLabel(&win);
        QString line;
        for (int c = 0; c < ROWS_N[r]; ++c)
            line += QStringLiteral("%1%2 ").arg(ROWS[r]).arg(c + 1);
        l->setWordWrap(true);
        l->setText(line);
    }
    return app.exec();
}
