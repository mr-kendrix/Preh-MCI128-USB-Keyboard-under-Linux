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
#include <sys/time.h>   // struct timeval (POSIX poll timeout)
#include <cerrno>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>   // LOCK_SH/LOCK_EX for advisory flock (reader parity)
#include <unistd.h>
#include "transport.h"
#include <dirent.h>
#include <algorithm>
#include <QElapsedTimer>
#include <QHash>
#include <QVector>
#include <limits>
#include <chrono>

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

// sweep-handler marker fd (test harness): opened from PKT_SWEEP_LOG at live start;
// handler writes "SWEEP_UP <hex>\n" lines via raw ::write (signal-safe, fixed buf).
static int g_mark_fd = -1;

int main(int argc, char **argv) {
    ::setlinebuf(stdout);   // live/daemon mode: every line visible in logs immediately
    QString path;
    QString mode;
    int poskey = -1;
    bool allow_actions = false;   // actions OFF by default (shell-free argv exec,
                                  // still: only run what the user explicitly allows)
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
        else if (a == "--calibrate") mode = QStringLiteral("calibrate");
        else if (a == "--processes") mode = QStringLiteral("processes");
        else if (a == "--feed-hex") { if (i + 1 < argc) feed = argv[++i]; }
        else if (a == "--calib") { if (i + 1 < argc) calib = argv[++i]; }
        else if (a == "--device") { if (i + 1 < argc) device = argv[++i]; }
        else if (a == "--seconds") { if (i + 1 < argc) secs = QString(argv[++i]).toInt(); }
        else if (a == "--allow-actions") allow_actions = true;
        else if (a == "--cooldown-test") mode = "cooldown-test";
        else if (path.isEmpty()) path = a;
    }
    // unit gates: cooldown table + action parsing, incl. deliberate NEGATIVE cases
    if (mode == "cooldown-test") {
        struct C { int pk; qint64 t; bool want; };
        const C cs[] = { {2, 0, true},    {2, 100, false},  {2, 600, true},
                         {3, 650, true}, {3, 700, false},  {2, 1100, true} };
        QHash<int, qint64> tbl;
        int fails = 0;
        for (const C &c : cs) {
            const bool got = action_cooldown_check(c.pk, tbl, c.t, 500);
            if (got != c.want) { ++fails; std::printf("FAIL pk=%d t=%lld got=%d want=%d\n", c.pk, (long long)c.t, (int)got, (int)c.want); }
        }
        // action parsing must match the spec incl. refusals (negative cases!).
        struct A { const char *type; const char *value; int min_size; bool refuse; };  // min_size<0 = only 'refused' matters
        const A as[] = {
            { "APPLICATION", "touch /tmp/pwn1", 1, false },               // ONE program name, execvp-safe
            { "APPLICATION", "/usr/bin/touch::/tmp/argvA::/tmp/argvB", 3, false },
            { "APPLICATION", "/bin/sh::-c::id", 3, false },               // parser allows; --allow-actions IS the boundary
            { "WEBSITE", "http://a\nb", -1, true },                        // control char -> refused
            { "WEBSITE", "http://a'b", 2, false },                        // apostrophe = DATA (xdg-open gets it as argv, no shell -> allowed)
            { "WEBSITE", "ftp://x", -1, true },                            // non-http scheme
            { "APPLICATION", "C:\\WINDOWS\\explorer.exe", -1, true },      // windows path
        };
        for (const A &a : as) {
            const pktm::Layer l{QString::fromLatin1(a.type), QString::fromUtf8(a.value)};
            QString why;
            const QVector<QString> av = action_argv(l, why);
            const bool refused = av.isEmpty();
            bool bad = false;
            if (refused != a.refuse) bad = true;
            else if (!a.refuse && av.size() < a.min_size) bad = true;
            if (bad) { ++fails; std::printf("FAIL %s '%s' av=%d refused=%d why='%s'\n", a.type, a.value, (int)av.size(), (int)refused, why.toUtf8().constData()); }
        }
        std::printf("COOLDOWN+ACTION SELFTEST %zu+%zu cases fails=%d %s\n", sizeof(cs) / sizeof(cs[0]), sizeof(as) / sizeof(as[0]), fails, fails ? "BAD" : "OK");
        return fails ? 1 : 0;
    }
    // process picker source: kdotool ps windows + /proc comm names — the combo
    // in the GUI edits with exactly this list (same code the dep-match uses).
    if (mode == "processes") {
        const QStringList procs = list_processes(200);
        std::printf("PROCESSES (%d):\n", (int)procs.size());
        for (const QString &p : procs)
            std::printf("  %s\n", p.toUtf8().constData());
        return 0;
    }
    if (mode.isEmpty() || path.isEmpty() || (mode == "inject" && poskey < 0)) {
        std::fprintf(stderr,
            "usage: pktm <pmap> [--rules-report | --dry-inject POSKEY | "
            "--live [--feed-hex '04 ..'] [--calib JSON] [--allow-actions]] | --processes | --calibrate\n"
            "  APPLICATION value syntax: prog::arg1::arg2 (each arg own '::'). Actions run ONLY with --allow-actions:\n"
            "  the pmap is the user's own file — a pmap may still ask for /bin/sh::-c::anything, so the flag IS the trust boundary.\n");
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
            pktm::Layer l;  // empty when the MAP carries no layer (poskey still a rule)
            if (sel >= 0) l = r.layers[sel];
            const QString dep = l.dep.isEmpty() ? QStringLiteral("*") : l.dep;
            QString flags;
            if (l.btf == QStringLiteral("True")) flags += QStringLiteral("+BTF");
            if (l.rep == QStringLiteral("True")) flags += QStringLiteral("+AUTO");
            if (l.slow == QStringLiteral("True")) flags += QStringLiteral("+SLOW");
            if (flags.isEmpty()) flags = QStringLiteral("-");
            const QByteArray v = l.value.toUtf8(), d = dep.toUtf8(), fg = flags.toUtf8();
            std::printf("poskey=%d type='%s' value='%s' dep='%s' flags=%s layers=%d\n",
                        r.poskey, l.type.toUtf8().constData(), v.constData(),
                        d.constData(), fg.constData(), int(r.layers.size()));
        }
        return 0;
    }

    // --calibrate: rebuild the scan->POS table prompt-guided: press the key the
    // user calls POSKey 1, then 2, ... 128. Nothing is injected; on exit (Ctrl-C,
    // --seconds, or 128 presses) writes --calib as JSON + "_board_chk" stamp so
    // a later live run can verify the table belongs to this board.
    if (mode == "calibrate") {
        if (calib.isEmpty()) {
            std::fprintf(stderr, "error: --calibrate needs --calib <out.json>\n");
            return 2;
        }
        QHash<int, int> learned;   // scan -> POS
        int expect = 1;
        auto learn = [&](int sc, bool down) {
            if (!down || learned.contains(sc) || expect > 128) return;
            learned.insert(sc, expect);
            std::printf("learn scan 0x%04x -> POSKey %d (next: POSKey %d)\n",
                        sc, expect, expect + 1);
            ++expect;
        };
        if (!feed.isEmpty()) {   // synthetic feed for tests
            for (const KeyEvent &ev : decode_stream(hex_to_bytes(feed)))
                learn(ev.scancode, ev.down);
        } else {
            QString node = device.isEmpty() ? find_hidraw_node() : device;
            int fd = node.isEmpty() ? -1
                                    : ::open(node.toUtf8().constData(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                std::fprintf(stderr, "error: no hidraw node (%s)\n", node.toUtf8().constData());
                return 2;
            }
            const time_t t0 = std::time(nullptr);
            unsigned char rep[64];
            while (expect <= 128) {
                if (secs > 0 && std::time(nullptr) - t0 >= secs) break;
                const ssize_t n = ::read(fd, rep, 64);
                if (n <= 0) {
                    if (errno == EAGAIN || errno == EINTR) { ::usleep(20000); continue; }
                    break;   // node died
                }
                QByteArray buf(reinterpret_cast<const char *>(rep), int(n));
                for (const KeyEvent &ev : decode_stream(buf))
                    if (ev.scancode >= 0) learn(ev.scancode, ev.down);
            }
            if (fd >= 0) ::close(fd);
        }
        QString js = QStringLiteral("{\n");
        int lw = 0;
        const int chk = device_present() ? board_checksum16(&lw) : -1;
        if (chk >= 0)
            js += QStringLiteral("  \"_board_chk\": \"%1\",\n").arg(chk, 4, 16, QLatin1Char('0'));
        QList<int> keys = learned.keys();
        std::sort(keys.begin(), keys.end());
        for (int i = 0; i < keys.size(); ++i) {
            const int sc = keys[i];
            const QString k = sc > 0xFFFF ? QStringLiteral("0x%1").arg(sc, 6, 16, QLatin1Char('0'))
                                          : QStringLiteral("0x%1").arg(sc, 4, 16, QLatin1Char('0'));
            js += QStringLiteral("  \"%1\": %2%3\n").arg(k).arg(learned[sc])
                       .arg(i + 1 < keys.size() ? QStringLiteral(",") : QString());
        }
        js += QStringLiteral("}\n");
        QFile out(calib);
        if (!out.open(QIODevice::WriteOnly)) {
            std::fprintf(stderr, "error: cannot write %s\n", calib.toUtf8().constData());
            return 2;
        }
        out.write(js.toUtf8());
        std::printf("-- calibrate: %d scans -> %s\n", int(learned.size()), calib.toUtf8().constData());
        return 0;
    }

    // --live: vendor IN stream -> calib -> rules -> ydotool socket. Never
    // starts/stops daemons or units; a human decides which reader runs.
    if (mode == "live") {
        // ACTIONS: list every action the loaded pmap contains; execution is OFF
        // unless --allow-actions was given explicitly (per-key 500 ms cooldown).
        {
            int nact = 0;
            for (const Rule &r : rs.rules)
                for (const pktm::Layer &l : r.layers)
                    if (pktm::layer_is_action(l)) {
                        QString why;
                        const QVector<QString> av = pktm::action_argv(l, why);
                        std::printf("ACTION found: poskey=%d %s -> %s%s\n", r.poskey,
                                    l.type.toUtf8().constData(),
                                    av.isEmpty() ? "REFUSED " : "",
                                    why.isEmpty() ? av.join(QLatin1Char(' ')).toUtf8().constData()
                                                  : why.toUtf8().constData());
                        ++nact;
                    }
            if (nact > 0)
                std::printf("actions=%d allow=%s (per-key 500ms cooldown)\n", nact,
                            allow_actions ? "YES" : "NO --allow-actions missing");
        }
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
            static char buf[16];   // test marker side-channel (signal-safe, fixed buf)
            for (int i = 0; i < sweep_count.load() && i < 64; ++i) {
                const unsigned code = held_evdev[i] & 0xFFFFu;
                unsigned char rec[24] = {0};
                rec[16] = 1; rec[18] = (unsigned char)code; rec[19] = (unsigned char)(code >> 8);
                (void)::send(fd, rec, sizeof rec, 0);
                if (g_mark_fd >= 0) {
                    static const char h[] = "0123456789abcdef";
                    static const char pre[] = "SWEEP_UP ";
                    char *p = buf;
                    for (const char *q = pre; *q; ++q) *p++ = *q;
                    *p++ = h[(code >> 12) & 15u]; *p++ = h[(code >> 8) & 15u];
                    *p++ = h[(code >> 4) & 15u];  *p++ = h[code & 15u];
                    *p++ = '\n';
                    (void)::write(g_mark_fd, buf, (ssize_t)(p - buf));
                }
            }
            std::_Exit(0);
        };
        if (const char *mk = getenv("PKT_SWEEP_LOG"))   // test harness marker file
            g_mark_fd = ::open(mk, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        std::signal(SIGTERM, sweep_handler);
        std::signal(SIGINT, sweep_handler);

        const QString calib_path = calib.isEmpty() ? (getenv("PKT_CALIB") ? QString(getenv("PKT_CALIB")) : QString()) : calib;
        if (calib_path.isEmpty()) {
            std::fprintf(stderr, "error: --calib <json> or PKT_CALIB env required for --live\n");
            return 2;
        }
        const QHash<int, int> table = load_calib_json(calib_path);
        // calib-drift guard: "_board_chk" stamp vs live board checksum (EF 0E)
        {
            const QString stamp = pktm::calib_board_chk(calib_path);
            if (!stamp.isEmpty() && device_present()) {
                int lw = 0;
                const int chk = board_checksum16(&lw);
                if (chk >= 0) {
                    const QString now = QStringLiteral("%1").arg(chk, 4, 16, QChar('0')).toUpper();
                    if (now != stamp.toUpper())
                        std::printf(">> WARN: live board chk %s != calib stamp %s — table was changed, "
                                    "calib may be stale (re-record or restamp)\n",
                                    now.toUtf8().constData(), stamp.toUtf8().constData());
                }
            } else if (stamp.isEmpty()) {
                std::printf(">> note: calib has no '_board_chk' stamp — restamp after table uploads\n");
            }
        }
        const QString sock = [=]() {                                       // resolve+sanity: typo'd
            QString s = getenv("YDOTOOL_SOCKET") ? QString(getenv("YDOTOOL_SOCKET"))          // YDOTOOL_SOCKET
                    : QStringLiteral("/tmp/.ydotool_socket");                                 // (the one ydotoold
            if (getenv("YDOTOOL_SOCKET") && !QFile::exists(s)) {                            // really serves). An
                std::fprintf(stderr,                                                       // EXPLICIT setting is
                    "error: YDOTOOL_SOCKET '%s' does not exist — unset it for the default "  // honoured hard, no
                    "/tmp/.ydotool_socket or fix the path\n", s.toUtf8().constData());      // silent override.
                return QString();                                                          // caller exits
            }
            return s;
        }();
        if (sock.isEmpty())
            return 2;   // explicit bad env = hard failure (a service nobody reads must not drift)
        if (getenv("PKT_HOLD_MS") || getenv("PKT_SWEEP_LOG"))
            std::printf(">> TEST HOOKS ACTIVE (PKT_HOLD_MS/PKT_SWEEP_LOG) — key presses stay "
                        "held N ms; remove from any service environment\n");

        // one inject path used by BOTH batch-feed and event-loop
        QSet<int> held;
        // AutoRepeat (measured 2026-11-10 via hold_record: board sends ONE down +
        // one 0xFE release per hold, no repeats): rules with rep=true inject their
        // down now and defer the matching up until the release report — the OS
        // typematic then repeats (vendor 250/15 ms are XKB settings, not wire).
        QHash<int, QPair<int, int>> ar_pending;   // scancode -> deferred up event
        QHash<int, time_t> ar_since;              // scancode -> press time (watchdog below)
        // AutoRepeat safety net (review 2026-11-10): if the release report never arrives
        // (lost 0xFE, board unplugged mid-hold) or the daemon lives long, an open AR
        // down would keep the app-side key held forever. Cap the open hold; PKT_MAX_HOLD_S
        // overrides for tests (the service keeps the default).
        int max_hold_s = 30;
        if (const char *h = getenv("PKT_MAX_HOLD_S")) { const int v = atoi(h); if (v > 0) max_hold_s = v; }
        int presses = 0, injected = 0, skipped = 0;
        // remove one key from the exit-sweep array (normal AR release, flush,
        // watchdog): swap-remove; a signal inside the swap window may re-up the
        // swapped key once — up events are idempotent, accepted by ydotool
        auto sweep_remove = [](int evdev) {
            const int n = sweep_count.load();
            for (int i = 0; i < n; ++i)
                if (int(held_evdev[i]) == (evdev & 0xFFFF)) {
                    held_evdev[i] = held_evdev[n - 1];
                    held_evdev[n - 1] = 0;
                    sweep_count.store(n - 1);
                    return;
                }
        };
        auto on_event = [&](int scancode, bool down) {
            if (!down) {
                held.remove(scancode);
                if (ar_pending.contains(scancode)) {
                    const QPair<int, int> up = ar_pending.take(scancode);
                    ar_since.remove(scancode);
                    ydot_send(sock, QVector<QPair<int, int>>{up});
                    sweep_remove(up.first);   // sweep must not re-up it at exit
                    std::printf("-- AutoRepeat release (scancode 0x%04x, up sent)\n", scancode);
                }
                return;
            }
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
            for (const pktm::Layer &l : hit->layers) if (!l.dep.isEmpty()) { need_dep = true; break; }
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
            const pktm::Layer &L = hit->layers[sel];
            if (layer_is_action(L)) {           // vendor ACTION layer: exec, not typing
                const QVector<QString> av = action_argv(L, why);
                if (av.isEmpty())
                    std::printf("POSKey %d -> ACTION %s '%s' -> REFUSED (%s), nothing run\n",
                                pk, L.type.toUtf8().constData(),
                                L.value.toUtf8().constData(), why.toUtf8().constData());
                else if (!allow_actions)
                    std::printf("POSKey %d -> ACTION %s '%s' -> BLOCKED (add --allow-actions)\n",
                                pk, L.type.toUtf8().constData(),
                                L.value.toUtf8().constData());
                else {
                    // time base = first action; cooldown via unit-testable fn
                    static QHash<int, qint64> act_lastms;
                    static std::chrono::steady_clock::time_point act_t0{};
                    if (act_t0.time_since_epoch().count() == 0)
                        act_t0 = std::chrono::steady_clock::now();
                    const qint64 t = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - act_t0).count();
                    if (!action_cooldown_check(pk, act_lastms, t)) {
                        std::printf("POSKey %d -> ACTION %s -> COOLDOWN (<500ms), skipped\n",
                                    pk, L.type.toUtf8().constData());
                    } else {
                        std::printf("POSKey %d -> ACTION %s -> exec '%s'\n",
                                    pk, L.type.toUtf8().constData(),
                                    av.join(QLatin1Char(' ')).toUtf8().constData());
                        spawn_argv(av);
                    }
                }
                return;
            }
            if (!value_script(*hit, L, seq, why)) {
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
            // exit-sweep bookkeeping tracks ONLY keys the injection leaves OPEN (AR
            // press below). Non-AR seqs send their own ups — no tracking (tracking
            // them caused the exit sweep to re-up already-released keys: user trial
            // 2026-11-11 marker showed SWEEP_UP 002d x2 after clean AR taps).
            const bool rep = hit->layers[sel].rep.trimmed().toLower() == QStringLiteral("true");
            if (rep && seq.size() == 2 && seq[0].second == 1 && seq[1].second == 0
                && seq[0].first == seq[1].first) {
                // up deferred -> register FIRST (signal between store and send must
                // never miss the open key), then send the down
                if (sweep_count.load() < 64) {
                    const int si = sweep_count.load();
                    held_evdev[si] = seq[0].first;
                    sweep_count.store(si + 1);
                }
                const int rc = ydot_send(sock, QVector<QPair<int, int>>{seq[0]});
                if (rc == 0) {
                    ar_pending.insert(scancode, seq[1]);
                    ar_since.insert(scancode, std::time(nullptr));   // watchdog start
                    ++injected;
                    std::printf("POSKey %d '%s' -> AutoRepeat press (ydotool key %d down only)\n",
                                pk, typed.toUtf8().constData(), seq[0].first);
                } else {
                    std::printf("POSKey %d '%s' -> NOT injected: socket rc=%d (%s)\n",
                                pk, typed.toUtf8().constData(), rc, sock.toUtf8().constData());
                }
                return;
            }
            const int gap = hit->layers[sel].slow.trimmed().toLower() == QStringLiteral("true")
                                ? 20000 : 0;   // vendor SlowOutput: 20 ms/char
            const int rc = ydot_send(sock, seq, gap);
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
        auto flush_one = [&](int sc, const char *why) {
            // one pending AutoRepeat down -> release it now (lost report / replug / watchdog)
            if (!ar_pending.contains(sc)) return;
            const QPair<int, int> up = ar_pending.take(sc);
            ar_since.remove(sc);
            held.remove(sc);       // the board no longer holds it: a re-press re-injects
            ydot_send(sock, QVector<QPair<int, int>>{ up });
            sweep_remove(up.first);
            std::printf("-- AutoRepeat release (%s): scancode 0x%04x up sent\n", why, sc);
        };

        if (!feed.isEmpty()) {           // synthetic recorded feed stays batch (gate)
            const QVector<KeyEvent> events = decode_stream(hex_to_bytes(feed));
            for (const KeyEvent &ev : events) on_event(ev.scancode, ev.down);
            std::printf("-- %d presses, %d injected, %d unknown scancodes skipped\n",
                        presses, injected, skipped);
            return 0;
        }

        // REAL event loop: blocking per-report read, replug reopen, --seconds budget
        // Part 1 scan (skippable by TEST HOOK PKT_SKIP_PKTD_SCAN for headless gates
        // only — same "remove from service env" warning as the other hooks):
        if (getenv("PKT_SKIP_PKTD_SCAN"))
            std::printf(">> TEST HOOK ACTIVE: pktd scan skipped — never set in a service\n");
        else {
            bool busy = false;
            if (struct dirent *de; DIR *pr = ::opendir("/proc")) {
                while ((de = ::readdir(pr))) {
                    char *e = de->d_name; char *q = e; int pid = 0;
                    while (*q && *q >= '0' && *q <= '9') { pid = pid * 10 + (*q - '0'); ++q; }
                    if (!pid || pid == ::getpid() || *q) continue;
                    char cp[64]; std::snprintf(cp, sizeof cp, "/proc/%d/cmdline", pid);
                    if (FILE *cf = ::fopen(cp, "r")) {
                        char cl[4096]; const size_t got = ::fread(cl, 1, sizeof cl - 1, cf);
                        ::fclose(cf);
                        cl[got] = '\0';
                        bool is_py = false, is_inj = false;
                        for (size_t i = 0; i + 7 <= got; ++i)
                            if (!std::memcmp(cl + i, "pktd.py", 7)) is_py = true;
                        for (size_t i = 0; i + 6 <= got; ++i)
                            if (!std::memcmp(cl + i, "inject", 6)) is_inj = true;
                        if (is_py && is_inj) { busy = true;
                            std::printf(">> pktd.py inject is live (pid %d) — stop pktd.service first\n", pid);
                            break; }
                    }
                }
                ::closedir(pr);
            }
            if (busy) return 2;
        }
        // hidraw reader parity part 2: advisory flock on the SAME lockfile pktd uses
        // (/tmp/pktd-hidraw.lock). pktd's calibrate/watch/raw commands hold it LOCK_EX
        // while running (pktd.py:249) -> our conditional LOCK_SH fails meanwhile, and
        // our lock also makes pktd's next mandatory attempt fail (pktd refuses then).
        // (pktd's cmd=inject daemon never flocks — part 1 above covers that case.)
        // Own LOCK_EX file additionally excludes pktm vs pktm. Kernel drops locks at
        // process exit, so no stale locks after crashes.
        {
            const char *peer_path = "/tmp/pktd-hidraw.lock";
            // READ-ONLY open: flock works on an r-only fd and never fails on a root-
            // owned file (pktd runs as root -> root:0644 would EACCES an O_WRONLY open
            // = false refusal even with pktd stopped — reviewer round-10 finding).
            // Create only when missing (pktd itself creates it at its startup).
            int peer = ::open(peer_path, O_RDONLY | O_CLOEXEC);
            if (peer < 0 && errno == ENOENT)
                peer = ::open(peer_path, O_CREAT | O_WRONLY | O_NOFOLLOW, 0666);
            if (peer < 0) {
                std::printf(">> lockfile %s unusable: %s\n", peer_path, std::strerror(errno));
                return 2;
            }
            if (::flock(peer, LOCK_SH | LOCK_NB) != 0) {
                ::close(peer);
                std::printf(">> refusing: pktd's lockfile is held (pktd calibrate/watch/"
                            "raw running) — wait for it or stop pktd.service\n");
                return 2;
            }
            // own lock in the PRIVATE per-user runtime dir, 0600 + O_NOFOLLOW: a
            // world-writable /tmp lockfile could be weaponised/pre-held by others
            QString ldir = getenv("XDG_RUNTIME_DIR");
            if (ldir.isEmpty()) ldir = QString("/run/user/%1").arg(int(::getuid()));
            const QString self_path = ldir + "/pktm-hidraw.lock";
            const int self = ::open(self_path.toUtf8().constData(),
                                    O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
            if (self < 0 || ::flock(self, LOCK_EX | LOCK_NB) != 0) {
                if (self >= 0) ::close(self);
                ::close(peer);   // closing the fd drops its advisory lock
                std::printf(">> refusing: another pktm holds the exclusive pktm lock\n");
                return 2;
            }
            // fds stay open for the process lifetime; both locks drop on exit;
            // O_CLOEXEC prevents fd leaks into helpers started via fork/exec
        }
        std::printf(">> backend=ydotool calib=%d rules=%d (event loop, window per press; Ctrl-C = sweep; max-hold=%ds)\n",
                    int(table.size()), int(rs.rules.size()), max_hold_s);
        QString node = device.isEmpty() ? find_hidraw_node() : device;
        int fd = node.isEmpty() ? -1 : ::open(node.toUtf8().constData(), O_RDONLY | O_CLOEXEC);
        const time_t t0 = std::time(nullptr);
        unsigned char rep[64];
        QByteArray buf;
        int dead_opens = 0;   // open succeeded but read fails immediately, in a row
        while (true) {
            if (secs > 0 && std::time(nullptr) - t0 >= secs) break;
            // AutoRepeat watchdog: runs EVERY iteration, so a held key without new
            // IN reports can still be released (see poll below — read would block)
            if (!ar_since.isEmpty()) {
                const time_t now = std::time(nullptr);
                QList<int> overdue;
                for (auto it = ar_since.cbegin(); it != ar_since.cend(); ++it)
                    if (now - it.value() >= max_hold_s) overdue << it.key();
                if (!overdue.isEmpty()) {
                    for (const int sc : overdue) flush_one(sc, "max-hold");
                    sweep_count.store(0);   // the released AR downs left the list
                }
            }
            if (fd < 0) {
                ::usleep(200000);
                node = find_hidraw_node();
                fd = node.isEmpty() ? -1 : ::open(node.toUtf8().constData(), O_RDONLY | O_CLOEXEC);
                if (fd >= 0) std::printf(">> reconnected hidraw %s\n", node.toUtf8().constData());
                continue;
            }
            // poll before read: while a key is HELD the board sends no reports, a
            // blocking read would wait forever and the watchdog above could not run.
            // events MUST request POLLIN — a pollfd with events=0 never reports the
            // ready byte (regression found by review: ppoll always returned 0, live
            // board read dead). ppoll keeps timespec precision; plain poll(fds,nfds,
            // timeout_MS) would work too but its timeout is in MILLISECONDS.
            {
                struct pollfd pfd{ fd, POLLIN, 0 };
                struct timespec tv{ 0, 250000000L };       // 250 ms in NANOseconds (ppoll)
                const int pr = ::ppoll(&pfd, 1, &tv, nullptr);
                if (pr == 0) continue;               // empty wait window: watchdog next round
                if (pr < 0 && errno == EINTR) continue;
            }
            const ssize_t n = ::read(fd, rep, 64);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EINTR)) { ::usleep(20000); continue; }
                ::close(fd); fd = -1;       // replug/EOF: node died -> reopen next round
                // a board unplugged mid-hold: those pending AR downs would never get
                // their release report — flush them NOW instead of leaving them open
                { const QList<int> arms = ar_pending.keys();
                  if (!arms.isEmpty()) { for (const int sc : arms) flush_one(sc, "hidraw gone");
                                         sweep_count.store(0); } }
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
    if (hit && idx >= 0 && layer_is_action(hit->layers[idx])) {
        const pktm::Layer &L = hit->layers[idx];
        QString why;
        const QVector<QString> av = action_argv(L, why);
        typed = av.isEmpty() ? why + QStringLiteral(" -> REFUSED")
                             : L.type + QStringLiteral(" ") + L.value
                               + QStringLiteral(" (would exec: %1)").arg(av.join(QLatin1Char(' ')));
        std::printf("POSKey %d window='%s' action='%s' rule-match=YES\n",
                    poskey, active_window().toUtf8().constData(), typed.toUtf8().constData());
        return 0;
    }
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
