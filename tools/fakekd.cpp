// fakekd: stand-in daemon for the GUI reload gate. Binds+listens the sock path
// via the tested ipc library, answers reload acks and quit. Never for a board.
#include "ipc.h"
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <unistd.h>

int main() {
    setvbuf(stdout, nullptr, 'X', 0);   // line-unbuffered: harness reads state live
    QString err;
    const int fd = pktipc::bind_socket(err);
    if (fd < 0) { std::printf("fakekd bind FAIL: %s\n", err.toUtf8().constData()); return 1; }
    std::printf("fakekd bound\n");
    pktipc::LineBuf in;
    for (int round = 0; round < 200; ++round) {   // ~10 s at 50 ms
        if (getenv("KD_NEVER_ACCEPT")) continue;  // rogue-daemon test: never accept
        QString e2;
        const int p = pktipc::poll_accept(fd, e2);
        if (p >= 0) {
            std::printf("fakekd peer accepted\n");
            pktipc::send_line(p, pktipc::mk_hello(), e2);   // handshake
            int idle = 0;
            for (int k = 0; k < 64 && idle < 3; ++k) {
                if (!in.acc.contains('\n')) {
                    struct pollfd pf{ p, POLLIN, 0 };
                    if (::poll(&pf, 1, 50) <= 0) { ++idle; continue; }   // wait for the next line
                }
                QString line;
                if (!pktipc::recv_line(p, in, line, e2)) break;
                if (line.isEmpty()) continue;
                idle = 0;
                pktipc::Msg m;
                QString why;
                std::printf("fakekd recv: %s\n", line.toUtf8().constData());
                std::fflush(stdout);
                if (!pktipc::parse_line(line, m, why)) { pktipc::send_line(p, pktipc::mk_error("ipc", why), e2); continue; }
                if (m.t == QLatin1String("reload")) {
                    pktipc::send_line(p, pktipc::mk_reload(QStringLiteral("/fakekd/p.pmap"), 3), e2);
                    // burst after ack: the GUI must drain these via refresh() heartbeats
                    pktipc::send_line(p, pktipc::mk_press(2, true, pktipc::now_ms()), e2);
                    pktipc::send_line(p, pktipc::mk_inject(2, QStringLiteral("TEXT"), QStringLiteral("hi"), pktipc::now_ms()), e2);
                    std::printf("fakekd reloaded -> ack + status lines\n");
                } else if (m.t == QLatin1String("quit")) {
                    pktipc::drop_bound(fd);
                    std::printf("fakekd quit\n");
                    return 0;
                }
            }
            ::close(p);
        }
        ::usleep(50000);
    }
    pktipc::drop_bound(fd);
    return 0;
}
