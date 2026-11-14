// Phase 2 IPC — see ipc.h; design: runs/phase2_ipc_design.md (NDJSON lines
// over one bound unix STREAM socket; daemon binds, GUI connects, both ways).
#include "ipc.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctime>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>

namespace pktipc {

static QString g_bound;   // path this process bound (peer side connects it)

QString sock_path() {
    const char *x = ::getenv("XDG_RUNTIME_DIR");
    const QString base = (x && *x) ? QString::fromUtf8(x) : QStringLiteral("/tmp");
    return base + QStringLiteral("/derpktxc/daemon.sock");
}

static bool fill_un(sockaddr_un &un, const QString &p) {
    const QByteArray pb = p.toUtf8();
    const int ln = qMin(pb.size(), int(sizeof un.sun_path) - 1);
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    memcpy(un.sun_path, pb.constData(), size_t(ln));
    un.sun_path[ln] = 0;
    return ln == pb.size();   // path must fit the sun_path field
}

bool ensure_dir() {
    const QString p = sock_path();
    const QString dir = p.left(p.size() - QStringLiteral("/daemon.sock").size());
    QDir d(dir);
    if (d.exists()) return true;
    return d.mkdir(QString(), {QFile::Permissions(0x1ff)});   // 0700 like the design
}

int bind_socket(QString &err) {
    if (!ensure_dir()) { err = QStringLiteral("no ipc dir"); return -1; }
    const QString p = sock_path();
    // stale node from a crashed daemon: unlink first, then bind fresh
    if (::access(p.toUtf8().constData(), 0) == 0)
        (void)::unlink(p.toUtf8().constData());
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { err = QStringLiteral("sock: %1").arg(errno); return -1; }
    sockaddr_un un {};
    if (!fill_un(un, p)) { err = QStringLiteral("path too long"); ::close(fd); return -1; }
    const socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + qstrlen(p.toUtf8().constData()) + 1);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&un), len) != 0) {
        err = QStringLiteral("bind: %1").arg(errno);
        ::close(fd);
        return -1;
    }
    // kernel 6.12+ AF_UNIX reconnect rework: a plain bound socket is NOT a receiver —
    // accept gives EINVAL and peer connect ECONNREFUSED until listen()/SO_LISTEN
    // marks sk_state TCP_LISTEN. Backlog 1 = the single GUI peer.
    if (::listen(fd, 1) != 0) {
        err = QStringLiteral("listen: %1").arg(errno);
        ::close(fd);
        return -1;
    }
    g_bound = p;
    return fd;
}

int connect_peer(QString &err) {
    const QString p = sock_path();
    sockaddr_un un {};
    if (!fill_un(un, p)) { err = QStringLiteral("path too long"); return -1; }
    // retry on ECONNREFUSED: a receiver that has not marked itself listening yet
    // refuses (kernel 6.12+ reconnect semantics) — wait up to ~1 s.
    for (int attempt = 0; attempt < 40; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) { err = QStringLiteral("sock: %1").arg(errno); return -1; }
        const socklen_t len = socklen_t(offsetof(sockaddr_un, sun_path) + qstrlen(p.toUtf8().constData()) + 1);
        if (::connect(fd, reinterpret_cast<const sockaddr *>(&un), len) == 0)
            return fd;
        const int e = errno;
        ::close(fd);
        // retry also ENOENT/ENOTDIR: the daemon may not have (re)bound the path yet
        // (board-check probe runs before the ipc block) — transient until bound.
        if (e != ECONNREFUSED && e != ENOENT && e != ENOTDIR) { err = QStringLiteral("connect: %1").arg(e); return -1; }
        ::usleep(25000);
    }
    err = QStringLiteral("connect: refused (no listening receiver)");
    return -1;
}

static std::atomic<int> g_peer{-1};   // accepted peer fd (-1 pending, -2 failed)

void start_accept(int bound_fd) {
    // detached thread parked on a blocking accept4: kernel 6.18 gives no usable
    // nonblock accept4 (probe: SO_* flags EINVAL, SOCK_NONBLOCK still blocks), so
    // the accept lives on its own thread; process exit ends the blocked syscall.
    std::thread t([bound_fd]{
        struct sockaddr_un ap {};
        socklen_t al = sizeof ap;
        const int p = ::accept4(bound_fd, reinterpret_cast<sockaddr *>(&ap), &al, 0);
        g_peer.store(p < 0 ? -2 : p);
    });
    t.detach();
}

int accepted_fd() { return g_peer.load(); }

void drop_bound(int fd) {
    if (fd >= 0) ::close(fd);
    (void)::unlink(sock_path().toUtf8().constData());   // stale nodes refuse connects
}

bool send_line(int fd, const QString &line, QString &err) {
    const QByteArray d = (line + QLatin1Char('\n')).toUtf8();
    int off = 0;
    while (off < d.size()) {
        // glibc <sys/socket.h> carries a 4.4-BSD 'send' relic with 4 args — ::send
        // on a socket fd resolves to the wrong symbol family (ENOTSOCK). ::write is
        // the safe call for connected sockets.
        const ssize_t n = ::write(fd, d.constData() + off, size_t(d.size() - off));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { err = QStringLiteral("send: %1").arg(errno); return false; }
        off += int(n);
    }
    return true;
}

bool recv_line(int fd, LineBuf &buf, QString &line, QString &err) {
    line.clear();
    int k = buf.acc.indexOf('\n');
    if (k < 0) {
        char tmp[4096];
        const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
        if (n == 0) { err = QStringLiteral("peer-closed"); return false; }
        if (n < 0) { err = QStringLiteral("recv: %1").arg(errno); return false; }
        buf.acc.append(tmp, int(n));
        k = buf.acc.indexOf('\n');
        if (k < 0) return false;   // partial line: keep buffering
    }
    line = QString::fromUtf8(buf.acc.constData(), k);
    buf.acc.remove(0, k + 1);
    return true;
}

qint64 now_ms() {
    timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

static QString pack_line(const QJsonObject &o) {
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

QString mk_press(int poskey, bool down, qint64 ms) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("press")},
                      {QStringLiteral("poskey"), poskey},
                      {QStringLiteral("down"), down},
                      {QStringLiteral("ms"), ms}});
}
QString mk_inject(int poskey, const QString &kind, const QString &seq, qint64 ms) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("inject")},
                      {QStringLiteral("poskey"), poskey},
                      {QStringLiteral("kind"), kind},
                      {QStringLiteral("seq"), seq},
                      {QStringLiteral("ms"), ms}});
}
QString mk_window(const QString &title, const QString &proc, qint64 ms) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("window")},
                      {QStringLiteral("title"), title},
                      {QStringLiteral("proc"), proc},
                      {QStringLiteral("ms"), ms}});
}
QString mk_health(int polls, int heartbeat, const QString &board_chk, const QString &warns) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("health")},
                      {QStringLiteral("polls"), polls},
                      {QStringLiteral("heartbeat"), heartbeat},
                      {QStringLiteral("board_chk"), board_chk},
                      {QStringLiteral("warns"), warns}});
}
QString mk_reload(const QString &path, int rules) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("reload")},
                      {QStringLiteral("path"), path},
                      {QStringLiteral("rules"), rules}});
}
QString mk_error(const QString &where, const QString &why) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("error")},
                      {QStringLiteral("where"), where},
                      {QStringLiteral("why"), why}});
}

QString mk_set_value(int layer, const QString &text) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("set-value")},
                      {QStringLiteral("layer"), layer},
                      {QStringLiteral("text"), text}});
}
QString mk_set_dep(int layer, const QString &proc) {
    return pack_line({{QStringLiteral("t"), QStringLiteral("set-dep")},
                      {QStringLiteral("layer"), layer},
                      {QStringLiteral("proc"), proc}});
}
QString mk_probe_window() { return pack_line({{QStringLiteral("t"), QStringLiteral("probe-window")}}); }
QString mk_reload_req()   { return pack_line({{QStringLiteral("t"), QStringLiteral("reload")}}); }
QString mk_quit()         { return pack_line({{QStringLiteral("t"), QStringLiteral("quit")}}); }

bool parse_line(const QString &line, Msg &m, QString &why) {
    QJsonParseError pe {};
    const QJsonDocument d = QJsonDocument::fromJson(line.toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError) { why = QStringLiteral("json: %1").arg(pe.errorString()); return false; }
    if (!d.isObject()) { why = QStringLiteral("not an object"); return false; }
    m.o = d.object();
    const auto it = m.o.find(QStringLiteral("t"));
    if (it == m.o.end() || !it.value().isString()) { why = QStringLiteral("missing t"); return false; }
    m.t = it.value().toString();
    return true;
}
}
