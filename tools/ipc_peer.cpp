// ipc peer gate: connects the daemon's bound socket, sends canned control
// lines, reads the emitted status lines and asserts the design contract.
// run: (pktm --live --ipc --feed-hex ...) & sleep 0.4; ipc_peer
#include "ipc.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char **av) {
    QString err;
    const int p = pktipc::connect_peer(err);
    if (p < 0) { std::printf("ipc peer FAIL: connect %s\n", err.toUtf8().constData()); return 2; }

    // control toward the daemon (design: pmap file = only source of truth;
    // the channel carries reload and quit only — no set-value / set-dep)
    QString es;
    if (!pktipc::send_line(p, pktipc::mk_reload_req(), es)) std::printf("peer send1 fail: %s\n", es.toUtf8().constData());
    if (!pktipc::send_line(p, pktipc::mk_quit(), es)) std::printf("peer send2 fail: %s\n", es.toUtf8().constData());

    // dead-daemon survival test (reviewer round-2): PKT_IPCG_DEAD=<ms> waits that
    // long (the harness SIGKILLs the daemon meanwhile), then writes AGAIN. With
    // MSG_NOSIGNAL this returns EPIPE — the PROCESS must live on, never exit 141.
    if (const char *dl = getenv("PKT_IPCG_DEAD")) {
        const int dl_ms = atoi(dl);
        if (dl_ms > 0) ::usleep(dl_ms * 1000);
        if (!pktipc::send_line(p, pktipc::mk_reload_req(), es))
            std::printf("dead-peer write: %s — survived (no SIGPIPE death)\n", es.toUtf8().constData());
        else
            std::printf("dead-peer write: still accepted (daemon lived)\n");
    }

    int saw_press = 0, saw_inject = 0, saw_reload = 0, saw_quit = 0, bad = 0;
    pktipc::LineBuf lb;
    for (int k = 0; k < 64; ++k) {
        QString line, e2;
        if (!pktipc::recv_line(p, lb, line, e2)) break;   // peer closed / err
        if (line.isEmpty()) continue;
        pktipc::Msg m;
        QString why;
        if (!pktipc::parse_line(line, m, why)) { ++bad; continue; }
        if (m.t == QLatin1String("press")) ++saw_press;
        else if (m.t == QLatin1String("inject")) ++saw_inject;
        else if (m.t == QLatin1String("reload")) ++saw_reload;
        else if (m.t == QLatin1String("error")) {
            const QString w = m.o.value(QStringLiteral("where")).toString();
            if (w == QLatin1String("ipc")) ++bad;   // unknown control -> error path OK once
        }
    }
    (void)saw_quit;
    std::printf("peer saw: press=%d inject=%d reload=%d bad=%d\n", saw_press, saw_inject, saw_reload, bad);
    if (saw_press >= 1 && saw_inject >= 1 && saw_reload >= 1 && bad == 0)
        return 0;
    std::printf("ipc peer FAIL: contract not met\n");
    return 1;
}
