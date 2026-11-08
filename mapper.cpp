// pktm rules layer — parse + match + window probe; own TU, mounts on pmap.cpp's
// cp1252 hex decoder without touching that shared file (kanban collision rule).
#include "mapper.h"
#include "pmap.h"
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
        if (k.startsWith("0x") || k.startsWith("0X")) k = k.mid(2);
        m.insert(k.toInt(nullptr, 16), it.value().toInteger());
    }
    return m;
}

// ydotool 1.x wire: one 24-byte input_event datagram per event + EV_SYN tail
// (record = {int64 sec, int64 usec, u16 type, u16 code, s32 value}, zeroed tv —
// exactly what Client/ydotool.c writes; the daemon's recv discards any datagram
// whose size != sizeof(struct input_event) = 24 on x86_64).
int ydot_send(const QString &socket_path, const QVector<QPair<int, int>> &events) {
    QByteArray p = socket_path.toUtf8();
    sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::memcpy(sa.sun_path, p.constData(),
                qMin(p.size(), int(sizeof(sa.sun_path) - 1)));
    int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -errno;
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa))) {
        const int e = errno; ::close(fd); return -e;
    }
    auto send_rec = [&fd](uint16_t type, uint16_t code, int32_t val) -> bool {
        unsigned char rec[24] = {};
        std::memcpy(rec + 16, &type, 2);
        std::memcpy(rec + 18, &code, 2);
        std::memcpy(rec + 20, &val, 4);
        return ::write(fd, rec, sizeof rec) == static_cast<ssize_t>(sizeof rec);
    };
    for (const auto &ev : events) {
        if (!send_rec(1, static_cast<uint16_t>(ev.first), static_cast<int32_t>(ev.second))) {
            const int e = errno; ::close(fd); return -e;
        }
        if (!send_rec(0, 0, 0)) {          // EV_SYN, like uinput_emit(syn_report=true)
            const int e = errno; ::close(fd); return -e;
        }
    }
    ::close(fd);
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

static bool dep_matches(const QString &dep, const QString &window) {
    if (dep.isEmpty() || window == k_unknown) return false;
    return window.contains(dep, Qt::CaseInsensitive) ||
           dep.contains(window, Qt::CaseInsensitive);
}

int match_layer(const Rule &r, const QString &window) {
    // window-specific layer wins, else the unconditional one
    for (int i = 0; i < r.layers.size(); ++i)
        if (!r.layers[i].dep.isEmpty() && dep_matches(r.layers[i].dep, window))
            return i;
    for (int i = 0; i < r.layers.size(); ++i)
        if (r.layers[i].dep.isEmpty())
            return i;
    return -1;
}

QString literal_to_type(const Rule &r, const Layer &l) {
    // "{N5}", "{Esc}", "{TAB}" are key-macro placeholders, not literal text:
    // strip {..} runs; nothing literal left (empty Value too) -> cap <Text>.
    // no regex needed: drop {..} macro placeholders, keep literal chars
    QString v;
    bool in_macro = false;
    for (const QChar &c : l.value) {
        if (c == QLatin1Char('{')) in_macro = true;
        else if (c == QLatin1Char('}')) in_macro = false;
        else if (!in_macro) v += c;
    }
    return v.isEmpty() ? r.text : v;
}

QString active_window() {
    QProcess p;
    p.start(QStringLiteral("xdotool"),
            {QStringLiteral("getactivewindow"), QStringLiteral("getwindowname")});
    if (p.error() == QProcess::FailedToStart)
        return k_unknown;
    p.waitForFinished(1500);
    const QString name = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    return name.isEmpty() ? k_unknown : name;
}

QString unknown_window() { return k_unknown; }

}
