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
#include <QLocalSocket>
#include <cstdio>
#include <QByteArray>
#include <QSet>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <atomic>
#include <ctime>
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
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
    ::setlinebuf(stdout);   // live/daemon mode: every line visible in logs immediately
    QString path;
    QString mode;
    int poskey = -1;
    QString feed, calib, device;
    int secs = 0;   // --live default: run until signalled; tests pass explicit time
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
            int sel = match_layer(r, unknown_window(), unknown_window());
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
        // exit sweep, SIGNAL-SAFE: handler sends up-events over the persistent
        // socket fd via raw ::send and _Exits; fixed array + atomic counter,
        // no heap / no stdio inside the handler. Normal exit sweeps via do_sweep().
        static std::atomic<int> sweep_count{0};
        static int held_evdev[64];
        // signal-safe sweep: reuse the fd ydot_send connected (set right after its
        // connect) — no connect/heap/stdio in the handler. PKT_HOLD_MS widens the
        // down->up gap so a signal can fall into a real hold window (tests).
        auto sweep_handler = [](int) {
            const int fd = pktm::ydot_sweep_fd();
            if (fd >= 0)
                for (int i = 0; i < sweep_count.load() && i < 64; ++i) {
                    unsigned char rec[24] = {0};
                    const unsigned code = held_evdev[i] & 0xFFFFu;
                    rec[16] = 1; rec[18] = (unsigned char)code; rec[19] = (unsigned char)(code >> 8);
                    (void)::send(fd, rec, sizeof rec, 0);
                }
            std::_Exit(0);
        };
        std::signal(SIGTERM, sweep_handler);
        std::signal(SIGINT, sweep_handler);

        const QString calib_path = calib.isEmpty() ? (getenv("PKT_CALIB") ? QString(getenv("PKT_CALIB")) : QString()) : calib;
        if (calib_path.isEmpty()) {
            std::fprintf(stderr, "error: --calib <json> or PKT_CALIB env required for --live\n");
            return 2;
        }
        const QHash<int, int> table = load_calib_json(calib_path);
        const QString sock = [=]() {                                       // resolve+sanity: typo'd
            QString s = getenv("YDOTOOL_SOCKET") ? QString(getenv("YDOTOOL_SOCKET"))          // YDOTOOL_SOCKET
                    : QStringLiteral("/tmp/.ydotool_socket");                                 // (the one ydotoold
            if (getenv("YDOTOOL_SOCKET") && !QFile::exists(s)                                 // really serves) ->
                && QFile::exists(QStringLiteral("/tmp/.ydotool_socket"))) {                  // loud fallback
                std::fprintf(stderr,
                    ">> YDOTOOL_SOCKET '%s' does not exist; falling back to '/tmp/.ydotool_socket' "
                    "which ydotoold serves — fix the env spelling, this line says which path got used\n",
                    s.toUtf8().constData());
                s = QStringLiteral("/tmp/.ydotool_socket");
            } else if (!QFile::exists(s)) {
                std::fprintf(stderr,
                    ">> ydotool socket '%s' not found — check YDOTOOL_SOCKET spelling and that "
                    "ydotoold runs; injections WILL FAIL\n", s.toUtf8().constData());
            }
            return s;
        }();

        // one inject path used by BOTH batch-feed and event-loop
        QSet<int> held;
        int presses = 0, injected = 0, skipped = 0;
        auto on_event = [&](int scancode, bool down) {
            if (!down) { held.remove(scancode); return; }
            if (held.contains(scancode)) return;      // typematic repeat while held
            held.insert(scancode);
            ++presses;
            if (!table.contains(scancode)) {
                std::printf("UNKNOWN scancode 0x%04x -> skipped (not in calib)\n", scancode);
                ++skipped; return;
            }
            const int pk = table.value(scancode);
            const Rule *hit = nullptr;
            for (const Rule &r : rs.rules)
                if (r.poskey == pk) { hit = &r; break; }
            if (!hit) { std::printf("POSKey %d -> no rule -> nothing injected\n", pk); return; }
            // probes only when actually needed (rule has a dependency)
            bool need_dep = false;
            for (const Layer &l : hit->layers) if (!l.dep.isEmpty()) { need_dep = true; break; }
            QString win = unknown_window(), proc;
            if (need_dep) { win = active_window(); proc = active_process(); }
            const int sel = match_layer(*hit, proc, win);
            if (sel < 0) {
                std::printf("POSKey %d -> no rule match (window '%s' proc '%s') -> nothing injected\n",
                            pk, win.toUtf8().constData(), proc.toUtf8().constData());
                return;
            }
            QVector<QPair<int, int>> seq;
            QString why;
            if (!value_script(*hit, hit->layers[sel], seq, why)) {
                std::printf("POSKey %d -> %s -> REFUSED, nothing injected\n",
                            pk, why.toUtf8().constData());
                return;
            }
            QString typed = literal_to_type(*hit, hit->layers[sel]); // label only
            if (typed.startsWith(QStringLiteral("\x01"))) typed = hit->text;
            if (seq.isEmpty()) {
                std::printf("POSKey %d '%s' -> rule match -> no injectable key -> nothing injected\n",
                            pk, typed.toUtf8().constData());
                return;
            }
            // record key-codes for the exit sweep: store BEFORE increment (a signal
            // between the two must never see an empty slot counted)
            for (const auto &p : seq)
                if (p.second == 1 && sweep_count.load() < 64) {
                    const int i = sweep_count.load();
                    held_evdev[i] = p.first;
                    sweep_count.store(i + 1);
                }
            const int rc = ydot_send(sock, seq);
            if (rc == 0) {
                // sequence fully sent (down+up pairs) -> nothing left to release
                sweep_count.store(0);
                ++injected;
                QString pairs;
                for (const auto &p : seq) pairs += QString("%1:%2 ").arg(p.first).arg(p.second);
                std::printf("POSKey %d '%s' -> rule match -> injected (ydotool key %s)\n",
                            pk, typed.toUtf8().constData(), pairs.toUtf8().constData());
            } else {
                std::printf("POSKey %d '%s' -> NOT injected: socket rc=%d (%s)\n",
                            pk, typed.toUtf8().constData(), rc, sock.toUtf8().constData());
            }
        };
        auto do_sweep = [&]() {
            // called ONLY from the loop (not from the signal handler): release pending keys
            QString pairs;
            QVector<QPair<int, int>> ups;
            for (int i = 0; i < sweep_count.load(); ++i) ups.append({held_evdev[i], 0});
            for (const auto &p : ups) pairs += QString("%1:0 ").arg(p.first);
            if (!ups.isEmpty()) ydot_send(sock, ups);
            sweep_count.store(0);
            std::printf("-- exit sweep: released %s\n", pairs.toUtf8().constData());
        };

        if (!feed.isEmpty()) {           // synthetic recorded feed stays batch (gate)
            const QVector<KeyEvent> events = decode_stream(hex_to_bytes(feed));
            for (const KeyEvent &ev : events) on_event(ev.scancode, ev.down);
            std::printf("-- %d presses, %d injected, %d unknown scancodes skipped\n",
                        presses, injected, skipped);
            return 0;
        }

        // REAL event loop: blocking per-report read, replug reopen, --seconds budget
        std::printf(">> backend=ydotool calib=%d rules=%d (event loop, window per press; Ctrl-C = sweep)\n",
                    int(table.size()), int(rs.rules.size()));
        QString node = device.isEmpty() ? find_hidraw_node() : device;
        int fd = node.isEmpty() ? -1 : ::open(node.toUtf8().constData(), O_RDONLY);
        const time_t t0 = std::time(nullptr);
        unsigned char rep[64];
        QByteArray buf;
        int dead_opens = 0;   // open succeeded but read fails immediately, in a row
        while (true) {
            if (secs > 0 && std::time(nullptr) - t0 >= secs) break;
            if (fd < 0) {
                ::usleep(200000);
                node = find_hidraw_node();
                fd = node.isEmpty() ? -1 : ::open(node.toUtf8().constData(), O_RDONLY);
                if (fd >= 0) std::printf(">> reconnected hidraw %s\n", node.toUtf8().constData());
                continue;
            }
            const ssize_t n = ::read(fd, rep, 64);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) { ::usleep(20000); continue; }
                ::close(fd); fd = -1;       // replug/EOF: node died -> reopen next round
                if (++dead_opens >= 5) {    // persistent other-reader (pktd.service?) -> stop spam
                    std::printf(">> hidraw node dead: %d open+read failures (errno %d) — "
                                "a second reader (pktd.service) owns it? stop it first\n",
                                dead_opens, errno);
                    break;
                }
                continue;
            }
            dead_opens = 0;
            buf.append(reinterpret_cast<const char*>(rep), int(n));
            for (const KeyEvent &ev : decode_stream(buf)) on_event(ev.scancode, ev.down);
            buf.clear();
        }
        if (sweep_count.load() > 0) do_sweep();
        std::printf("-- %d presses, %d injected, %d unknown scancodes skipped\n",
                    presses, injected, skipped);
        return 0;
    }

    // --dry-inject: resolve window, match, print what WOULD be typed (no send)
    const QString win = active_window();
    const QString proc = active_process();
    const Rule *hit = nullptr;
    for (const Rule &r : rs.rules)
        if (r.poskey == poskey) { hit = &r; break; }
    int idx = hit ? match_layer(*hit, proc, win) : -1;
    QString typed;
    if (idx >= 0) {
        QVector<QPair<int, int>> seq;
        QString why;
        if (value_script(*hit, hit->layers[idx], seq, why)) {
            typed.clear();
            for (const auto &p : seq) typed += QStringLiteral("%1:%2 ").arg(p.first).arg(p.second);
        } else typed = why + QStringLiteral(" -> REFUSED");
    }
    const QByteArray w = win.toUtf8(), t = typed.toUtf8();
    std::printf("POSKey %d window='%s' would-type '%s' rule-match=%s\n",
                poskey, w.constData(), t.constData(), idx >= 0 ? "YES" : "NO");
    return 0;
}
