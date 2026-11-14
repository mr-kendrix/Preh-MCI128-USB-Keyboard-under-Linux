// grab: screen capture to PNG via QScreen::grab() (Qt 6.10+)
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QImage>
#include <QPixmap>
#include <QDebug>
#include <cstdio>
int main(int argc, char **av) {
    QGuiApplication app(argc, av);
    if (argc < 2) { qWarning("usage: grab <out.png>"); return 1; }
    const auto scr = app.screens();
    if (scr.isEmpty()) { qWarning("no screens"); return 2; }
    const QPixmap pm = scr.first()->grabWindow(0, 0, 0, -1, -1);
    QImage img = pm.toImage();
    if (img.isNull()) { qWarning("grab empty"); return 3; }
    if (!img.save(av[1])) { qWarning("write failed"); return 4; }
    qInfo() << "wrote" << av[1] << img.size();
    return 0;
}
