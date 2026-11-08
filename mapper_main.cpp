// pktm — mapper rules-layer CLI + headless gate:
//   pktm <pmap> --rules-report   one line per rule (poskey, value, dep|'*', flags)
//   pktm <pmap> --dry-inject N   preview what POSKey N would type for the
//                                active window (xdotool via QProcess; the
//                                <unknown-window> fallback admits only
//                                unconditional rules) — nothing is injected.
// Every vendor body is cp1252-hexed; mapper.cpp does the rule decode.
#include "mapper.h"
#include <QFile>
#include <cstdio>

using namespace pktm;

int main(int argc, char **argv) {
    QString path;
    QString mode;
    int poskey = -1;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (a == "--rules-report") mode = QStringLiteral("report");
        else if (a == "--dry-inject") {
            mode = QStringLiteral("inject");
            if (i + 1 < argc) poskey = QString(argv[++i]).toInt();
        } else if (path.isEmpty()) path = a;
    }
    if (path.isEmpty() || mode.isEmpty() || (mode == "inject" && poskey < 0)) {
        std::fprintf(stderr, "usage: pktm <pmap> [--rules-report | --dry-inject POSKEY]\n");
        return 1;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "open fail %s\n", path.toUtf8().constData());
        return 1;
    }
    const RuleSet rs = parse_rules(f.readAll());

    if (mode == "report") {
        const QByteArray nm = rs.name.toUtf8();
        std::printf("rules=%d name=%s\n", int(rs.rules.size()), nm.constData());
        for (const Rule &r : rs.rules) {
            int sel = match_layer(r, unknown_window());
            if (sel < 0 && !r.layers.isEmpty()) sel = 0;
            Layer l;  // empty when the MAP carries no layer (poskey still a rule)
            if (sel >= 0) l = r.layers[sel];
            const QString dep = l.dep.isEmpty() ? QStringLiteral("*") : l.dep;
            QString flags;
            if (l.btf == QStringLiteral("True")) flags += QStringLiteral("+BTF");
            if (l.rep == QStringLiteral("True")) flags += QStringLiteral("+AUTO");
            if (l.slow == QStringLiteral("True")) flags += QStringLiteral("+SLOW");
            if (flags.isEmpty()) flags = QStringLiteral("-");
            const QByteArray v = l.value.toUtf8(), d = dep.toUtf8(), fg = flags.toUtf8();
            std::printf("poskey=%d value='%s' dep='%s' flags=%s layers=%d\n",
                        r.poskey, v.constData(), d.constData(), fg.constData(),
                        int(r.layers.size()));
        }
        return 0;
    }

    // --dry-inject: resolve window, match, print what WOULD be typed (no send)
    const QString win = active_window();
    const Rule *hit = nullptr;
    for (const Rule &r : rs.rules)
        if (r.poskey == poskey) { hit = &r; break; }
    int idx = hit ? match_layer(*hit, win) : -1;
    QString typed;
    if (idx >= 0) typed = literal_to_type(*hit, hit->layers[idx]);
    const QByteArray w = win.toUtf8(), t = typed.toUtf8();
    std::printf("POSKey %d window='%s' would-type '%s' rule-match=%s\n",
                poskey, w.constData(), t.constData(), idx >= 0 ? "YES" : "NO");
    return 0;
}
