// pktm rules layer — parse + match + window probe; own TU, mounts on pmap.cpp's
// cp1252 hex decoder without touching that shared file (kanban collision rule).
#include "mapper.h"
#include "pmap.h"
#include <vector>
#include <algorithm>
#include <QProcess>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QByteArray>
#include <QFile>
#include <QPair>
#include <cstring>
#include <cerrno>
#include <ctime>   // struct timespec + nanosleep (PKT_HOLD_MS hook)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace pktm {

static const QString k_unknown = QStringLiteral("<unknown-window>");

static QString unhex_body(const QString &body) {
    // vendor bodies are hex on one line; whitespace-only bodies (e.g.
    // <Dependency>\n        </Dependency>) mean "empty" — strip ws first.
    QString hex;
    for (const QChar &c : body)
        if (!c.isSpace()) hex += c;
    return unhex_cp1252(hex);
}

static QString grab(const QString &scope, const QString &tag) {
    QRegularExpression re("<" + tag + ">(.*?)</" + tag + ">",
                          QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(scope);
    return m.isValid() ? unhex_body(m.captured(1)).trimmed() : QString();
}

// ---- live IN feed (pktm --live) ----
// pktwire port: 0x04 IN reports; 0xFE key-up marker, 0xE0 extended prefix.
static const struct { const char *word; int evdev; } WORDS[] = {
    {"tab", 15}, {"enter", 28}, {"space", 57}, {"leer", 57}, {"esc", 1},
    {"löschen", 111}, {"del", 111}, {"delete", 111},
    {"up", 103}, {"down", 108}, {"left", 105}, {"right", 106},
    {"shift", 42}, {"schift", 42},
};
static const struct { char ch; int evdev; } CHARMAP[] = {
    {'0', 11}, {'1', 2}, {'2', 3}, {'3', 4}, {'4', 5}, {'5', 6}, {'6', 7},
    {'7', 8}, {'8', 9}, {'9', 10},
    {'a', 30}, {'b', 48}, {'c', 46}, {'d', 32}, {'e', 18}, {'f', 33},
    {'g', 34}, {'h', 35}, {'i', 23}, {'j', 36}, {'k', 37}, {'l', 38},
    {'m', 50}, {'n', 49}, {'o', 24}, {'p', 25}, {'q', 16}, {'r', 19},
    {'s', 31}, {'t', 20}, {'u', 22}, {'v', 47}, {'w', 17}, {'x', 45},
    {'y', 21}, {'z', 44},
    {' ', 57}, {'\t', 15}, {'\n', 28}, {'-', 12}, {'=', 13}, {'.', 52},
    {',', 51}, {'/', 53}, {';', 39}, {'\'', 40}, {'[', 26}, {']', 27},
    {'\\', 43}, {'`', 41},
};

static int word_evdev(const QString &w) {
    for (const auto &e : WORDS)
        if (w == QString::fromUtf8(e.word)) return e.evdev;
    return -1;
}
static int char_evdev(QChar c) {
    for (const auto &e : CHARMAP)
        if (c.unicode() == static_cast<int>(static_cast<unsigned char>(e.ch))) return e.evdev;
    return -1;
}

QVector<QPair<int, int>> evdev_sequence(const QString &text) {
    QVector<QPair<int, int>> out;
    const QString t = text.trimmed();
    if (t.isEmpty()) return out;
    const int one = word_evdev(t.toLower());
    if (one >= 0) { out << QPair<int, int>{one, 1} << QPair<int, int>{one, 0}; return out; }
    for (const QChar &c : t) {
        const bool upper = c.isUpper();
        const int code = char_evdev(upper ? c.toLower() : c);
        if (code < 0) return {};   // one unmappable char -> refuse, half-typed text is worse
        if (upper) {               // vendor shift chain: shift down, key, key up, shift up
            out << QPair<int, int>{42, 1} << QPair<int, int>{code, 1}
                << QPair<int, int>{code, 0} << QPair<int, int>{42, 0};
        } else {
            out << QPair<int, int>{code, 1} << QPair<int, int>{code, 0};
        }
    }
    return out;
}

// ---- macro executor ---------------------------------------------------
// Vendor {TOKEN} Values are the real output; refuse only what we cannot do
// faithfully. ev_key numbers verified against the XKB evdev keycodes table
// (value = ev_key + 8: TAB 23->15, ESC 9->1, BKSP 22->14, DELE 119->111) and
// the live journal ('5' -> key 6). Never guess an unknown token's meaning.
static int macro_evdev(const QString &tok) {
    static const struct { const char *tok; int ev; } MACROS[] = {
        {"N0",11},{"N1",2},{"N2",3},{"N3",4},{"N4",5},{"N5",6},{"N6",7},{"N7",8},
        {"N8",9},{"N9",10},
        {"BS",14},{"DEL",111},{"ESC",1},{"TAB",15},{"SPACE",57},{"ENTER",28},
        {"UP",103},{"DOWN",108},{"LEFT",105},{"RIGHT",106},{"PGUP",104},{"PGDN",109},
        {"SHIFT",42},{"CTRL",29},{"ALT",56},{"CAPS",58},{"NUMLOCK",69},{"SCROLL-LOCK",70},
    };
    for (const auto &e : MACROS)
        if (tok.compare(QString::fromUtf8(e.tok), Qt::CaseInsensitive) == 0) return e.ev;
    return -1;
}
static int modifier_evdev(const QString &tok) {
    if (tok.compare("CTRL", Qt::CaseInsensitive) == 0) return 29;   // LCTL
    if (tok.compare("ALT", Qt::CaseInsensitive) == 0) return 56;    // LALT
    if (tok.compare("SHIFT", Qt::CaseInsensitive) == 0) return 42;  // LFSH
    return -1;
}

// false -> refused; why says which token/char made it impossible
bool value_script(const Rule &r, const Layer &l, QVector<QPair<int, int>> &seq, QString &why) {
    seq.clear(); why.clear();
    if (l.value.isEmpty()) { seq = evdev_sequence(r.text); return true; } // <Text> fallback
    const QString v = l.value;
    // mixed literal+macro: literal = non-space char OUTSIDE braces (token text
    // never counts as literal); mixed stays refused (oracle behaviour untested).
    bool has_macro = v.contains(QLatin1Char('{'));
    bool has_lit = false;
    bool inm = false;
    for (int i = 0; i < v.size(); ++i) {
        const QChar c = v[i];
        if (c == QLatin1Char('{')) inm = true;
        else if (c == QLatin1Char('}')) inm = false;
        else if (!inm && !c.isSpace()) { has_lit = true; break; }
    }
    if (has_macro && has_lit) { why = QStringLiteral("mixed literal+macro"); return false; }
    int i = 0;
    while (i < v.size()) {
        const QChar c = v[i];
        if (c == QLatin1Char('{')) {
            const int end = v.indexOf(QLatin1Char('}'), i + 1);
            if (end < 0) { seq.clear(); why = QStringLiteral("unterminated {"); return false; }
            const QString tok = v.mid(i + 1, end - i - 1);
            i = end + 1;
            if (tok.contains(QLatin1Char('{'))) { seq.clear(); why = QStringLiteral("nested brace"); return false; }
            // {DELAY}: NEVER silently drop. Vendor-tool wait semantics (allegedly
            // ~0.5 s) untested here and ydot_send has no timed-send model -> refuse.
            if (tok == QLatin1String("DELAY")) {
                seq.clear(); why = QStringLiteral("DELAY not modelled, refusing"); return false;
            }
            if (tok.contains(QLatin1Char('+'))) {
                const QStringList parts = tok.split(QLatin1Char('+'));
                QVector<int> mods;
                bool ok = parts.size() >= 2;
                for (int p = 0; ok && p + 1 < parts.size(); ++p) {
                    const int m = modifier_evdev(parts[p]);
                    if (m < 0) ok = false; else mods << m;
                }
                int key = ok ? macro_evdev(parts.last()) : -1;
                if (ok && key < 0 && parts.last().size() == 1)
                    key = char_evdev(parts.last().toLower()[0]);
                if (!ok || key < 0) { seq.clear(); why = QStringLiteral("unsupported combo '") + tok + "'"; return false; }
                for (const int m : mods) seq << QPair<int, int>{m, 1};
                seq << QPair<int, int>{key, 1} << QPair<int, int>{key, 0};
                for (int k = mods.size() - 1; k >= 0; --k) seq << QPair<int, int>{mods[k], 0};
                continue;
            }
            const int ev = macro_evdev(tok);
            if (ev < 0) { seq.clear(); why = QStringLiteral("unsupported macro '") + tok + "'"; return false; }
            seq << QPair<int, int>{ev, 1} << QPair<int, int>{ev, 0};
        } else {
            const bool upper = c.isUpper();
            const int code = char_evdev(upper ? c.toLower() : c);
            if (code < 0) { seq.clear(); why = QStringLiteral("unmappable char"); return false; }
            if (upper) seq << QPair<int, int>{42, 1} << QPair<int, int>{code, 1}
                           << QPair<int, int>{code, 0} << QPair<int, int>{42, 0};
            else seq << QPair<int, int>{code, 1} << QPair<int, int>{code, 0};
            ++i;
        }
    }
    return true;
}

QVector<KeyEvent> decode_report(const unsigned char *p) {
    QVector<KeyEvent> out;
    QVector<int> keys;
    for (int i = 0; i < 4; ++i)
        if (p[i]) keys << p[i];
    int i = 0;
    while (i < keys.size()) {
        bool down = true, ext = false;
        while (i < keys.size() && (keys[i] == 0xFE || keys[i] == 0xE0)) {
            if (keys[i] == 0xFE) down = false; else ext = true;
            ++i;
        }
        if (i >= keys.size()) break;   // marker without a code
        const int code = keys[i++];
        out.append(KeyEvent{(ext ? 0xE000 : 0) | code, down, ext});
    }
    return out;
}

QVector<KeyEvent> decode_stream(const QByteArray &raw) {
    QVector<KeyEvent> out;
    const unsigned char *b = reinterpret_cast<const unsigned char *>(raw.constData());
    for (int i = 0; i < raw.size(); ) {
        if (b[i] == 0x04 && i + 5 <= raw.size()) {
            out += decode_report(b + i + 1);
            i += 5;
        } else ++i;                     // resync on stray bytes
    }
    return out;
}

QHash<int, int> load_calib_json(const QString &path) {
    QHash<int, int> m;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return m;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return m;
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        QString k = it.key();
        if (k.startsWith("_")) continue;   // metadata keys ("_board_chk") not scancodes
        if (k.startsWith("0x") || k.startsWith("0X")) k = k.mid(2);
        m.insert(k.toInt(nullptr, 16), it.value().toInteger());
    }
    return m;
}

// board-checksum stamp written by a human/pktup when calib was recorded:
// "_board_chk": "D2B6". Empty when absent.
QString calib_board_chk(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return QString();
    const QString v = doc.object().value("_board_chk").toString();
    return v.trimmed();
}

// ydotool 1.x wire: one 24-byte input_event datagram per event + EV_SYN tail
// (record = {int64 sec, int64 usec, u16 type, u16 code, s32 value}, zeroed tv —
// exactly what Client/ydotool.c writes; the daemon's recv discards any datagram
// whose size != sizeof(struct input_event) = 24 on x86_64).
// keys currently held by the LAST ydot_send (exit sweep: a signal mid-sequence
// must not leave them stuck — handler reuses the connected fd stored here)
static std::vector<int> held_keys;
static int g_ydot_connected = -1;   // fd connected by ydot_send_now (for the handler)
static int ydot_send_now(const QString &socket_path, const QVector<QPair<int, int>> &events, int gap_us);
int ydot_send(const QString &socket_path, const QVector<QPair<int, int>> &events, int gap_us) {
    for (const auto &e : events)
        if (e.second == 1) held_keys.push_back(e.first);
        else held_keys.erase(std::remove(held_keys.begin(), held_keys.end(), e.first), held_keys.end());
    return ydot_send_now(socket_path, events, gap_us);
}
int ydot_sweep_fd() { return g_ydot_connected; }
static int ydot_send_now(const QString &socket_path, const QVector<QPair<int, int>> &events, int gap_us) {
    // ONE connected socket for the whole daemon lifetime (review 2026-11-10: per-call
    // socket() leaked one fd per injection — thousands of keypresses = thousands of
    // leaked descriptors dying silently at the fd limit). Reconnect only after a
    // failure (or a path change), closing the old fd and invalidating BEFORE close.
    static QString connected_path;
    int fd = g_ydot_connected;
    if (fd >= 0 && connected_path != socket_path) {   // path moved: close old, reconnect below
        ::close(fd);
        g_ydot_connected = -1;
        fd = -1;
    }
    if (fd < 0) {
        QByteArray p = socket_path.toUtf8();
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        std::memcpy(sa.sun_path, p.constData(),
                    qMin(p.size(), int(sizeof(sa.sun_path) - 1)));
        fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);   // cloexec: QProcess
        // helpers (kdotool/xdotool) must never inherit the ydotool fd (review r11)
        if (fd < 0) return -errno;
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa))) {
            const int e = errno; ::close(fd); return -e;
        }
        g_ydot_connected = fd; connected_path = socket_path;
    }
    auto send_rec = [&fd](uint16_t type, uint16_t code, int32_t val) -> bool {
        unsigned char rec[24] = {};
        std::memcpy(rec + 16, &type, 2);
        std::memcpy(rec + 18, &code, 2);
        std::memcpy(rec + 20, &val, 4);
        return ::write(fd, rec, sizeof rec) == static_cast<ssize_t>(sizeof rec);
    };
    auto fail_close = [&](int e) -> int {
        // invalidate BEFORE close: a signal between the two statements must never
        // see a stale fd pointing at a closed (or kernel-reused) descriptor
        if (g_ydot_connected == fd) g_ydot_connected = -1;
        ::close(fd);
        return -e;
    };
    for (const auto &ev : events) {
        if (!send_rec(1, static_cast<uint16_t>(ev.first), static_cast<int32_t>(ev.second)))
            return fail_close(errno);
        // PKT_HOLD_MS: TEST-ONLY hook — hold the key for N ms before its release
        // (lets a SIGTERM land in a real hold window so the sweep can be observed).
        // nanosleep: correct for N >= 1000 (usleep caps at one second).
        if (ev.second == 1) {   // hold only between a down and its matching up
            if (const char *hs = getenv("PKT_HOLD_MS")) {
                const long ms = atol(hs);
                if (ms > 0) {
                    struct timespec ts{ static_cast<time_t>(ms / 1000),
                                        static_cast<time_t>((ms % 1000) * 1000000L) };
                    ::nanosleep(&ts, nullptr);
                }
            }
        }
        if (!send_rec(0, 0, 0))
            return fail_close(errno);
        if (ev.second == 0 && gap_us > 0)   // vendor SlowOutput: 20 ms/char
            ::usleep(gap_us);
    }
    // success: the fd stays connected — reused by the NEXT call and by the signal
    // sweep (never closed here, that would leak one fd per injection)
    return 0;
}

RuleSet parse_rules(const QString &xml) {
    RuleSet rs;
    rs.name = grab(xml, "Name");
    QRegularExpression mapRe("<MAP>(.*?)</MAP>",
                             QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression layerRe("<LAYER>(.*?)</LAYER>",
                               QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator mit = mapRe.globalMatch(xml);
    while (mit.hasNext()) {
        const QString mb = mit.next().captured(1);
        Rule r;
        r.poskey = grab(mb, "POSKey").toInt();
        r.text = grab(mb, "Text");
        QRegularExpressionMatchIterator lit = layerRe.globalMatch(mb);
        while (lit.hasNext()) {
            const QString lb = lit.next().captured(1);
            Layer l;
            l.type = grab(lb, "Type");
            l.value = grab(lb, "Value");
            l.dep = grab(lb, "Dependency");
            l.btf = grab(lb, "BringToForeground");
            l.rep = grab(lb, "AutoRepeat");
            l.slow = grab(lb, "SlowOutput");
            r.layers.append(l);
        }
        rs.rules.append(r);
    }
    return rs;
}

static bool dep_matches(const QString &dep_in, const QString &process, const QString &window) {
    if (dep_in.isEmpty() || window == k_unknown) return false;
    // vendor "<Dependency>" may be "process" or "process:title" — split on ':'
    QString dep_proc = dep_in, dep_title;
    const int colon = dep_in.indexOf(QLatin1Char(':'));
    if (colon > 0) { dep_proc = dep_in.left(colon); dep_title = dep_in.mid(colon + 1); }
    // comm truncates at 15 chars -> truncate BOTH, then case-insensitive EQUAL
    auto proc_eq = [](const QString &a, const QString &b) {
        if (a.isEmpty() || b.isEmpty()) return false;
        return a.left(15).compare(b.left(15), Qt::CaseInsensitive) == 0;
    };
    if (!process.isEmpty())
        return proc_eq(dep_proc, process) && (dep_title.isEmpty() || window.contains(dep_title, Qt::CaseInsensitive));
    // process undeterminable (no xdotool/pure Wayland) -> degraded title-only match
    return window.contains(dep_proc, Qt::CaseInsensitive);
}

int match_layer(const Rule &r, const QString &process, const QString &window) {
    // window-specific layer wins, else the unconditional one
    for (int i = 0; i < r.layers.size(); ++i)
        if (!r.layers[i].dep.isEmpty() && dep_matches(r.layers[i].dep, process, window))
            return i;
    for (int i = 0; i < r.layers.size(); ++i)
        if (r.layers[i].dep.isEmpty())
            return i;
    return -1;
}

QString literal_to_type(const Rule &r, const Layer &l) {
    // macros: known tokens map via WORDS (caller-side); an UNKNOWN {...} token must
    // not be silently dropped -> refuse (sentinel '#', caller injects nothing + logs).
    // known macro-only Value falls back to cap <Text> (label is the honest stand-in).
    static const auto known = [](const QString &t) {
        static const QStringList k = {"N1","N2","N3","N3a","N3b","N4","N5","N6","N7","N8","N9",
            "N10","N11","N12","N13","N14","ESC","TAB","DELAY","POS","POSKEY","MSR"};
        for (const QString &s : k) if (t.compare(s, Qt::CaseInsensitive) == 0) return true;
        return false;
    };
    // mixed literal+macro ("abc{TAB}def"): the Tab press would be LOST by stripping
    // -> REFUSE unless the whole Value is one known macro (fallback path).
    bool any_macro = false;
    for (const QChar &c : l.value) if (c == QLatin1Char('{')) { any_macro = true; break; }
    if (any_macro) {
        QString tok; bool inm = false; bool has_lit = false;
        for (const QChar &c : l.value) {
            if (c == QLatin1Char('{')) { if (inm) return QStringLiteral("\x01nested-brace"); inm = true; tok.clear(); }
            else if (c == QLatin1Char('}')) { inm = false; if (!known(tok)) return QStringLiteral("\x01unsupported-macro"); }
            else if (inm) tok += c;
            else if (!c.isSpace()) has_lit = true;
        }
        if (inm) return QStringLiteral("\x01unsupported-macro"); // unterminated
        if (has_lit) return QStringLiteral("\x01mixed-macro-refused");
    }
    QString v; bool in_macro = false; QString tok;
    for (const QChar &c : l.value) {
        if (c == QLatin1Char('{')) { in_macro = true; tok.clear(); }
        else if (c == QLatin1Char('}')) {
            in_macro = false;
            if (!known(tok)) return QStringLiteral("\x01unsupported-macro");
        }
        else { if (in_macro) tok += c; if (!in_macro) v += c; }
    }
    if (in_macro) return QStringLiteral("\x01unsupported-macro"); // unterminated {...}
    return v.isEmpty() ? r.text : v;
}

// window probe helper chain: kdotool (KDE Wayland/Plasma 6, KWin scripting over
// D-Bus) preferred; xdotool for X11 sessions; else fail-closed unknown window.
// A probe counts as VALID only if the helper started AND exited 0 (review 2026-11-10:
// kdotool 0.3.0's own compat check exits nonzero on unsupported Plasma builds — a
// nonzero exit must NOT be read as a window). Title = first line of stdout only.
// Test environment for this chain: kdotool v0.3.0 @ Plasma 6.5.x (Arch, 2026-11-10).
static bool run_probe(const QString &helper, const QStringList &args, QString &out) {
    QProcess p;
    p.start(helper, args);
    if (p.error() == QProcess::FailedToStart) return false;
    p.waitForFinished(3000);   // kdotool loads+runs+deletes a KWin script per call
    if (p.state() == QProcess::Running) { p.kill(); return false; }   // hung helper: no window, no freeze
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    if (!err.isEmpty()) {
        // one log line per distinct stderr message — the probe runs per press,
        // a repeating warning would flood the live log (review 2026-11-10).
        // Informational only: the exit code + output below decide.
        static QStringList logged;
        const QString line = err.split('\n').first().simplified();
        if (!logged.contains(line)) {
            logged << line;
            std::printf(">> note: %s stderr '%s' (exit %d)\n",
                        helper.toUtf8().constData(), line.toUtf8().constData(), p.exitCode());
        }
    }
    if (p.exitCode() != 0) return false;   // helper said no: unknown/process-undeterminable
    out = QString::fromUtf8(p.readAllStandardOutput()).split('\n').first().trimmed();
    return true;
}

QString active_window() {
    for (const QString &h : { QStringLiteral("kdotool"), QStringLiteral("xdotool") }) {
        QString name;
        if (run_probe(h, {QStringLiteral("getactivewindow"), QStringLiteral("getwindowname")}, name))
            return name.isEmpty() ? k_unknown : name;
    }
    return k_unknown;
}

QString active_process() {
    // getactivewindow getwindowpid -> /proc/PID/comm (vendor <Dependency> = process name).
    // Rules should name the PROCESS, not only a title: the process comes from the
    // PID (titles are user-settable by any window — review 2026-11-10).
    for (const QString &h : { QStringLiteral("kdotool"), QStringLiteral("xdotool") }) {
        QString pid;
        if (run_probe(h, {QStringLiteral("getactivewindow"), QStringLiteral("getwindowpid")}, pid)) {
            if (pid.isEmpty()) return QString();
            // PID must be plain digits before it touches /proc (garbage from an
            // unvalidated helper must never build a path — review 2026-11-10)
            bool numeric = true;
            for (const QChar &c : pid)
                if (!c.isDigit()) { numeric = false; break; }
            if (!numeric) return QString();
            QFile c(QStringLiteral("/proc/%1/comm").arg(pid));
            if (!c.open(QIODevice::ReadOnly)) return QString();   // PID not alive: undeterminable
            return c.readAll().trimmed();
        }
    }
    return QString();
}

QString unknown_window() { return k_unknown; }

}
