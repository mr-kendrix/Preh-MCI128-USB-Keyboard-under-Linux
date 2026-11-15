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
#include <poll.h>

namespace pktipc {

static QString g_bound;   // path this process bound (peer side connects it)

QString sock_path() {
    const char *x = ::getenv("XDG_RUNTIME_DIR");
    const QString base = (x && *x) ? QString::fromUtf8(x) : QStringLiteral("/tmp");
    return base + QStringLiteral("/pkt/daemon.sock");   // short namespace dir (review 2), not 'derpktxc'
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
    if (QDir(dir).exists()) return true;
    // Qt6 Permissions are an ENUM bitmask (ReadUser/WriteUser/ExeUser), not octal:
    // 0700 = the three User bits. mkpath creates parents as needed.
    return QDir().mkpath(dir, {QFile::Permissions(QFile::ReadUser | QFile::WriteUser | QFile::ExeUser)});
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
    // Standard stream-socket semantics: bind() alone is not a receiver. accept()
    // answers EINVAL and peer connect ECONNREFUSED until listen() marks the socket
    // TCP_LISTEN. This is not a kernel-version change — listen() is simply required.
    // Backlog 1 is enough for the single GUI peer.
    if (::listen(fd, 1) != 0) {
        err = QStringLiteral("listen: %1").arg(errno);
        ::close(fd);
        return -1;
    }
    g_bound = p;
    return fd;
}

static int try_connect_once(QString &err) {
    const QString p = sock_path();
    sockaddr_un un {};
    if (!fill_un(un, p)) { err = QStringLiteral("path too long"); return -1; }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { err = QStringLiteral("sock: %1").arg(errno); return -1; }
    const socklen_t len = offsetof(sockaddr_un, sun_path) + qstrlen(p.toUtf8().constData()) + 1;
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&un), len) == 0)
        return fd;
    const int e = errno;
    ::close(fd);
    err = QStringLiteral("connect: %1").arg(e);
    return -1;
}

int try_connect(QString &err) { return try_connect_once(err); }

int try_connect_nb(QString &err) {
    // SOCK_NONBLOCK belongs in the TYPE argument (2nd), not the 3rd (that is the
    // protocol field -> EPROTONOSUPPORT 93 when passed there, an old probe bug).
    const QString p = sock_path();
    sockaddr_un un {};
    if (!fill_un(un, p)) { err = QStringLiteral("path too long"); return -1; }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) { err = QStringLiteral("sock(nb): %1").arg(errno); return -1; }
    const socklen_t len = offsetof(sockaddr_un, sun_path) + qstrlen(p.toUtf8().constData()) + 1;
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&un), len) == 0)
        return fd;
    const int e = errno;
    if (e == EAGAIN || e == ENOENT || e == ENOTDIR) {
        // queue full / not ready: connected once poll sees data; <=200 ms only.
        struct pollfd pf{ fd, POLLOUT, 0 };
        if (::poll(&pf, 1, 200) > 0) return fd;
        ::close(fd);
        err = QStringLiteral("connect: timeout (accept queue)");
        return -1;
    }
    ::close(fd);
    err = QStringLiteral("connect: %1").arg(e);
    return -1;
}

int connect_peer(QString &err) {
    // retry on ECONNREFUSED/ENOENT/ENOTDIR: daemon may not have (re)bound yet
    for (int attempt = 0; attempt < 40; ++attempt) {
        const int fd = try_connect_once(err);
        if (fd >= 0) return fd;
        const int e = errno;
        if (e != ECONNREFUSED && e != ENOENT && e != ENOTDIR) return -1;
        ::usleep(25000);
    }
    err = QStringLiteral("connect: refused (no listening receiver)");
    return -1;
}

int poll_accept(int fd, QString &err) {
    // poll(POLLIN) on the LISTENing socket reports a pending peer connect (verified
    // on kernel 6.18: 0 without pending, 1 after a connect, 0 after its accept).
    // Guarding accept4 with it keeps the pump loop non-blocking; without a pending
    // connect accept4 on a LISTEN socket would WAIT for one. If the poll signal is
    // stale (peer died meanwhile) accept4 answers EAGAIN — callers treat p<0 as
    // 'no peer now' and retry on the next loop iteration.
    struct pollfd pf{ fd, POLLIN, 0 };
    if (::poll(&pf, 1, 0) <= 0) return -1;
    struct sockaddr_un ap {};
    socklen_t al = sizeof ap;
    const int p = ::accept4(fd, reinterpret_cast<sockaddr *>(&ap), &al, 0);
    if (p < 0) { err = QStringLiteral("accept: %1").arg(errno); return -1; }
    return p;
}

void drop_bound(int fd) {
    if (fd >= 0) ::close(fd);
    (void)::unlink(sock_path().toUtf8().constData());   // stale nodes refuse connects
}

bool send_line(int fd, const QString &line, QString &err) {
    const QByteArray d = (line + QLatin1Char('\n')).toUtf8();
    int off = 0;
    while (off < d.size()) {
        // 4-arg ::send with MSG_NOSIGNAL on the CONNECTED accepted peer fd: a dead
        // peer gives EPIPE WITHOUT raising SIGPIPE — protects daemon AND GUI
        // regardless of each program's signal setup (the historic ENOTSOCK bug was
        // sending on the BOUND fd, unrelated to send-vs-write).
        const ssize_t n = ::send(fd, d.constData() + off, size_t(d.size() - off), MSG_NOSIGNAL);
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

// GUI -> daemon messages: reload and quit ONLY. set-value / set-dep / probe-window
// were removed by design: the pmap file on disk is the single source of truth; the
// GUI saves it atomically and sends reload. Two live rule copies must not exist.
QString mk_reload_req() { return pack_line({{QStringLiteral("t"), QStringLiteral("reload")}}); }
QString mk_hello() { return pack_line({{QStringLiteral("t"), QStringLiteral("hello")},
                                        {QStringLiteral("v"), 1}}); }
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
