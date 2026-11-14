#pragma once
// Phase 2 IPC (2026-11-14, design runs/phase2_ipc_design.md): one unix STREAM
// socket at $XDG_RUNTIME_DIR/derpktxc/daemon.sock; daemon binds, GUI connects
// the same path; NDJSON both directions ({\"t\":type,...}, no framing bytes).
// Additive: everything works byte-identical without the socket.
#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <sys/un.h>

namespace pktipc {

// --- endpoints ------------------------------------------------------------
QString sock_path();                 // design path (XDG_RUNTIME_DIR or /tmp fallback)
bool  ensure_dir();                  // mkdir $XDG_RUNTIME_DIR/derpktxc mode 0700
int   bind_socket(QString &err);     // unlink stale, bind; fd bound to path
int   connect_peer(QString &err);    // GUI side: connect to the bound path
void  start_accept(int bound_fd);    // daemon side: detached thread parked on a BLOCKING
                                     // accept4 — kernel 6.18 has no working nonblock accept4
                                     // (SO_* flags EINVAL, SOCK_* ignored); pending peer
                                     // connects are queued, the thread picks them up
int   accepted_fd();                 // -1 pending / >=0 connected peer / -2 accept failed
void  drop_bound(int bound_fd);      // unlink the path + close fd (daemon exit cleanup)
bool  send_line(int fd, const QString &line, QString &err);   // NUL-terminated write

// --- framing: buffered line reader (stream socket, no message boundary) ----
struct LineBuf { QByteArray acc; };
bool recv_line(int fd, LineBuf &buf, QString &line, QString &err);
// one ::recv, appends, pops the first full line; false = nothing complete yet

// --- messages (daemon -> GUI) ----------------------------------------------
QString mk_press(int poskey, bool down, qint64 ms);
QString mk_inject(int poskey, const QString &kind, const QString &seq, qint64 ms);
QString mk_window(const QString &title, const QString &proc, qint64 ms);
QString mk_health(int polls, int heartbeat, const QString &board_chk, const QString &warns);
QString mk_reload(const QString &path, int rules);
QString mk_error(const QString &where, const QString &why);

// --- messages (GUI -> daemon) ----------------------------------------------
QString mk_set_value(int layer, const QString &text);
QString mk_set_dep(int layer, const QString &proc);
QString mk_probe_window();
QString mk_reload_req();
QString mk_quit();

// --- parse -----------------------------------------------------------------
struct Msg { QString t; QJsonObject o; };
bool parse_line(const QString &line, Msg &m, QString &why);

// ms since process start (CLOCK_MONOTONIC, shared by both sides)
qint64 now_ms();
}
