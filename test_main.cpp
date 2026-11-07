#include "pmap.h"
#include "transport.h"
#include <QFile>
#include <QTextStream>
#include <cstdio>
#include <vector>
#include <cstdlib>

int main(int argc, char **argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: pktest <pmap> <mwx>\n"); return 1; }
    // gate 1: pmap parse
    QFile f(argv[1]);
    if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "open fail\n"); return 1; }
    const QString xml = f.readAll();
    const Pmap p = parse_pmap(xml);
    // gate 2: report-4 compile of .mwx payload
    QFile m(argv[2]);
    std::vector<uint8_t> payload;
    if (m.open(QIODevice::ReadOnly)) {
        const QByteArray raw = m.readAll();
        payload.assign(raw.constData(), raw.constData() + raw.size());
    }
    auto frames = compile_frames(payload);
    std::printf("maps=%zu name=%s frames=%zu first=%02x\n",
                p.maps.size(), p.name.toUtf8().constData(), frames.size(),
                frames.empty() ? 0 : frames[0][0]);
    bool ok = p.name.size() > 0 && !frames.empty() && frames[0][0] == 0x04;
    return ok ? 0 : 2;
}
