// pktm — mapper rules-layer CLI + headless gate:
//   pktm <pmap> --rules-report   one line per rule (poskey, value, dep|'*', flags)
//   pktm <pmap> --dry-inject N   preview what POSKey N would type for the
//                                active window (xdotool via QProcess; the
//                                <unknown-window> fallback admits only
//                                unconditional rules) — nothing is injected.
//   pktm --live <pmap> [--feed-hex '04 ..'] [--calib JSON] [--device NODE]
//                                [--seconds N]  IN reports (report 0x04) ->
//                                scan->POSKey (calib) -> rules -> ydotool inject.
// Every vendor body is cp1252-hexed; mapper.cpp does the rule decode.
#include "mapper.h"
#include <QFile>
#include <cstdio>
#include <QByteArray>
#include <QSet>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include "transport.h"
#include <dirent.h>

using namespace pktm;

static QString find_hidraw_node() {
    // /dev/input/by-id: prefer the vendor interface (*Combi-if01-hidraw), fall
    // back to any Preh/MCI if01 hidraw node like pktd autodetect does.
    const QString dir = QStringLiteral("/dev/input/by-id");
    QString best, alt;
    if (DIR *h = ::opendir(dir.toUtf8().constData())) {
        while (struct dirent *e = ::readdir(h)) {
            const QString n = QString::fromUtf8(e->d_name);
            if (!n.endsWith("hidraw")) continue;
            if (n.contains("Combi-if01")) best = n;
            else if (n.contains("if01") &&
                     (n.contains("preh", Qt::CaseInsensitive) || n.contains("mci", Qt::CaseInsensitive))) {
                if (alt.isEmpty()) alt = n;
            }
        }
        ::closedir(h);
    }
    const QString pick = best.isEmpty() ? alt : best;
    return pick.isEmpty() ? QString() : dir + QLatin1Char('/') + pick;
}

static QByteArray collect_hidraw(const QString &node, int secs, bool &opened) {
    // hidraw reports arrive whole; O_NONBLOCK + short sleep keeps --seconds honest
    // without pulling in glibc's poll() timeout overloads.
    QByteArray raw;
    opened = false;
    const int fd = ::open(node.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return raw;
    opened = true;
    const time_t t0 = std::time(nullptr);
    char buf[512];
    while (std::time(nullptr) - t0 < secs) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) { raw.append(buf, n); continue; }
        if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        ::usleep(20000);
    }
    ::close(fd);
    return raw;
}

static QByteArray collect_bus(int secs) {
    // fallback via transport.cpp: hid_libusb_wrap_sys_device on the bus node
    // (same wrapped-hidraw route the upload engine uses; two readers on one
    // endpoint race — pktd.service is a human decision, not ours)
    QByteArray raw;
    unsigned char buf[64];
    const long long t0 = std::time(nullptr);
    while (std::time(nullptr) - t0 < secs) {
        const int n = poll_in(buf, int(sizeof buf), 50);
        if (n < 0) break;
        if (n > 0) raw.append(reinterpret_cast<const char *>(buf), n);
    }
    return raw;
}

static QByteArray hex_to_bytes(const QString &hex) {
    QByteArray out;
    int hi = -1;
    for (const QChar &c : hex) {
        if (c.isSpace()) continue;
        int d = -1;
        if (c >= QLatin1Char('0') && c <= QLatin1Char('9')) d = c.unicode() - '0';
        else if (c >= QLatin1Char('a') && c <= QLatin1Char('f')) d = c.unicode() - 'a' + 10;
        else if (c >= QLatin1Char('A') && c <= QLatin1Char('F')) d = c.unicode() - 'A' + 10;
        else return out;
        if (hi < 0) hi = d;
        else { out.append(static_cast<char>((hi << 4) | d)); hi = -1; }
    }
    return out;
}

int main(int argc, char **argv) {
    QString path;
    QString mode;
    int poskey = -1;
    QString feed, calib, device;
    int secs = 3;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (a == "--rules-report") mode = QStringLiteral("report");
        else if (a == "--dry-inject") {
            mode = QStringLiteral("inject");
            if (i + 1 < argc) poskey = QString(argv[++i]).toInt();
        }
        else if (a == "--live") mode = QStringLiteral("live");
        else if (a == "--feed-hex") { if (i + 1 < argc) feed = argv[++i]; }
        else if (a == "--calib") { if (i + 1 < argc) calib = argv[++i]; }
        else if (a == "--device") { if (i + 1 < argc) device = argv[++i]; }
        else if (a == "--seconds") { if (i + 1 < argc) secs = QString(argv[++i]).toInt(); }
        else if (path.isEmpty()) path = a;
    }
    if (path.isEmpty() || mode.isEmpty() || (mode == "inject" && poskey < 0)) {
        std::fprintf(stderr,
            "usage: pktm <pmap> [--rules-report | --dry-inject POSKEY | "
            "--live [--feed-hex '04 ..' | --device NODE] [--calib JSON] [--seconds N]]\n");
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

    // --live: vendor IN stream -> calib -> rules -> ydotool socket. Never
    // starts/stops daemons or units; a human decides which reader runs.
    if (mode == "live") {
        const QString calib_path = calib.isEmpty() ? QStringLiteral("/home/nonrmy/MK.calib.json") : calib;
        const QHash<int, int> table = load_calib_json(calib_path);
        const QString win = active_window();
        const QString sock = getenv("YDOTOOL_SOCKET") ? QString(getenv("YDOTOOL_SOCKET"))
                                                      : QStringLiteral("/tmp/.ydotool_socket");
        std::printf(">> backend=ydotool calib=%d rules=%d window='%s' socket=%s\n",
                    int(table.size()), int(rs.rules.size()), win.toUtf8().constData(),
                    sock.toUtf8().constData());

        QByteArray raw;
        if (!feed.isEmpty()) {
            raw = hex_to_bytes(feed);
            std::printf(">> synthetic feed (--feed-hex): %d bytes\n", int(raw.size()));
        } else {
            const QString node = device.isEmpty() ? find_hidraw_node() : device;
            bool opened = false;
            if (!node.isEmpty()) {
                raw = collect_hidraw(node, secs, opened);
                std::printf(">> reading %s (%ds)\n", node.toUtf8().constData(), secs);
            }
            if (raw.isEmpty() && !opened) {   // no hidraw node left after replug
                raw = collect_bus(secs);
                std::printf(">> reading bus node via wrapped hidapi (%ds)\n", secs);
            }
        }

        const QVector<KeyEvent> events = decode_stream(raw);
        QSet<int> held, announced;
        int presses = 0, injected = 0, skipped = 0;
        for (const KeyEvent &ev : events) {
            if (!ev.down) { held.remove(ev.scancode); continue; }
            if (held.contains(ev.scancode)) continue;  // auto-repeat while held
            held.insert(ev.scancode);
            ++presses;
            if (!table.contains(ev.scancode)) {
                if (!announced.contains(ev.scancode)) {
                    announced.insert(ev.scancode);
                    std::printf("UNKNOWN scancode 0x%04x -> skipped (not in calib)\n", ev.scancode);
                }
                ++skipped;
                continue;
            }
            const int pk = table.value(ev.scancode);
            const Rule *hit = nullptr;
            for (const Rule &r : rs.rules)
                if (r.poskey == pk) { hit = &r; break; }
            if (!hit) {
                std::printf("POSKey %d -> no rule for this POSKey -> nothing injected\n", pk);
                continue;
            }
            const int sel = match_layer(*hit, win);
            if (sel < 0) {
                std::printf("POSKey %d -> no rule match (window '%s') -> nothing injected\n",
                            pk, win.toUtf8().constData());
                continue;
            }
            const QString typed = literal_to_type(*hit, hit->layers[sel]);
            const QVector<QPair<int, int>> seq = evdev_sequence(typed);
            if (seq.isEmpty()) {
                std::printf("POSKey %d '%s' -> rule match -> no evdev code -> nothing injected\n",
                            pk, typed.toUtf8().constData());
                continue;
            }
            QString pairs;
            for (const auto &p : seq) pairs += QString("%1:%2 ").arg(p.first).arg(p.second);
            const int rc = ydot_send(sock, seq);
            if (rc == 0) {
                ++injected;
                std::printf("POSKey %d '%s' -> rule match -> injected (ydotool key %s)\n",
                            pk, typed.toUtf8().constData(), pairs.toUtf8().constData());
            } else {
                std::printf("POSKey %d '%s' -> rule match -> NOT injected: socket rc=%d (%s)\n",
                            pk, typed.toUtf8().constData(), rc, sock.toUtf8().constData());
            }
        }
        std::printf("-- %d presses, %d injected, %d unknown scancodes skipped\n",
                    presses, injected, skipped);
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
