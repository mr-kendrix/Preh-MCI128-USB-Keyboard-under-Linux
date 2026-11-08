// pktup — LB5 gate: the report-4 upload loop + progress + error mapping,
// reachable headless so the wiring is testable without a hand on the board.
//   pktup <pmap> [mwx] [--upload] [--arm-upload] [--cancel-at N] [--map-table] [--shot f.png]
// The board is only ever written when --arm-upload (or PKT_UPLOAD_ARM=1) is set.
#include "upload.h"
#include <libusb-1.0/libusb.h>
#include <QApplication>
#include <QWidget>
#include <QPixmap>
#include <QImage>
#include <QFile>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
    QString path, container, shot;
    bool armed = false, upload = false, table = false, fail_text = false, verify = false;
    int cancel_at = -1, dump_frames = 0;
    for (int i = 1; i < argc; ++i) {
        const QString a = argv[i];
        if (a == "--verify") verify = true;
        else if (a == "--arm-upload") armed = true;
        else if (a == "--map-table") table = true;
        else if (a == "--fail-text") fail_text = true;
        else if (a == "--shot") { if (i + 1 < argc) shot = argv[++i]; }
        else if (a == "--cancel-at") { if (i + 1 < argc) cancel_at = QString(argv[++i]).toInt(); }
        else if (a == "--dump-frames") { if (i + 1 < argc) dump_frames = QString(argv[++i]).toInt(); }
        else if (path.isEmpty()) path = a;
        else container = a;
    }
    if (dump_frames > 0) {
        // byte-identical check against the python vendor_frame oracle (no board)
        std::vector<uint8_t> body(8 * dump_frames, 0x5A);
        for (int i = 0; i < dump_frames; ++i) {
            const auto f = vendor_frame(4 + 8 * i, body.data() + 8 * i, (i + 1) & 3);
            for (size_t k = 0; k < f.size(); ++k)
                std::printf("%02x%s", f[k], k + 1 == f.size() ? "\n" : " ");
        }
        std::printf("frames=%d\n", dump_frames);
        return 0;
    }
    if (getenv("PKT_UPLOAD_ARM")) armed = true;

    Pmap pm;
    if (path.size() > 0) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "open fail %s\n", path.toUtf8().constData()); return 1; }
        pm = parse_pmap(f.readAll());
    }

    if (table) { // every mapped status, so the mapping is verifiable on screen
        const int codes[] = {0, LIBUSB_ERROR_IO, LIBUSB_ERROR_INVALID_PARAM,
                             LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_NO_DEVICE,
                             LIBUSB_ERROR_NOT_FOUND, LIBUSB_ERROR_BUSY,
                             LIBUSB_ERROR_TIMEOUT, LIBUSB_ERROR_OVERFLOW,
                             LIBUSB_ERROR_PIPE, LIBUSB_ERROR_INTERRUPTED,
                             LIBUSB_ERROR_NO_MEM, LIBUSB_ERROR_NOT_SUPPORTED,
                             LIBUSB_ERROR_OTHER};
        for (const int c : codes)
            std::printf("%d|%s\n", c, transfer_error_message(c));
        return 0;
    }

    if (fail_text) { // formatting of the bounce/cancel/success reports (no wire)
        UploadResult r;
        r.frames = 182;
        r.sent = 41;
        r.stopped_at = 41;
        r.failed_code = 0x12;
        r.armed = true;
        for (const int s : {LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_TIMEOUT,
                            LIBUSB_ERROR_ACCESS, LIBUSB_ERROR_NOT_SUPPORTED}) {
            r.status = s;
            std::printf("%s\n", upload_text(r).toUtf8().constData());
        }
        r.status = 0;
        r.canceled = true;
        std::printf("%s\n", upload_text(r).toUtf8().constData());
        r.canceled = false;
        std::printf("%s\n", upload_text(r).toUtf8().constData());
        return 0;
    }

    if (verify) { // board checksum readback vs container (no write)
        const auto payload = payload_of(pm, container);
        std::vector<uint8_t> body(payload.begin(), payload.end());
        if (body.size() >= 4 && body[0] == 'P')
            body = std::vector<uint8_t>(body.begin() + 4, body.end());
        int lw = -1;
        const int bc = board_checksum16(&lw);
        const int ln = (body.size() >= 4) ? ((uint16_t(body[2]) << 8) | body[3]) : 0;
        uint32_t sum = 0;
        for (int i = 0; i + 1 < ln && i < int(body.size()); ++i) sum += body[i];
        const uint16_t cs = uint16_t(sum & 0xFFFF);
        std::printf("board: len=%d chk=%04X  container chk=%04X  %s  raw=%s\n",
                    lw, bc, cs, (bc >= 0 && bc == cs) ? "MATCH" : "DIFF/unread",
                    first8_debug().toUtf8().constData());
        return 0;
    }

    if (shot.size()) { // pixel proof: the button + status line inside a window
        static const char *qargv[] = {"pktup", "-platform", "offscreen"};
        int qargc = 3;
        QApplication app(qargc, const_cast<char **>(qargv));
        QWidget win;
        win.resize(360, 140);
        win.show();
        UploadPanel panel(&win, &pm, container, armed);
        if (upload) panel.run_now();
        const QPixmap pm_img = win.grab();
        const QImage img = pm_img.toImage();
        if (img.isNull()) { std::fprintf(stderr, "grab null\n"); return 1; }
        if (!img.save(shot)) { std::fprintf(stderr, "shot save failed\n"); return 1; }
        int inked = 0;
        for (int y = 0; y < img.height(); y += 3)
            for (int x = 0; x < img.width(); x += 3)
                if (img.pixel(x, y) != Qt::transparent) ++inked;
        std::printf("shot=%s %dx%d inked=%d status=%s\n", shot.toUtf8().constData(),
                    img.width(), img.height(), inked, panel.status_text().toUtf8().constData());
        return 0;
    }

    if (!upload) { // plan only: what a click would send
        const auto payload = payload_of(pm, container);
        const auto frames = compile_frames(payload);
        std::printf("payload=%lld frames=%zu first=%02x armed=%d board=%d\n",
                    (long long)payload.size(), frames.size(),
                    frames.empty() ? 0 : frames[0][0], int(armed),
                    int(device_present()));
        if (!device_present())
            std::printf("probe=%s\n", transfer_error_message(device_last_status()));
        return 0;
    }

    // the actual loop, progress reported per frame
    const UploadResult r = run_upload(payload_of(pm, container), armed,
                                      [&](int done, int total) {
        if (cancel_at >= 0 && done > cancel_at) return false; // prove the cancel path
        std::fprintf(stderr, "\r%d/%d ", done, total);
        return true;
    });
    std::fprintf(stderr, "\n");
    std::printf("frames=%d sent=%d armed=%d canceled=%d stopped=%d status=%d\n",
                r.frames, r.sent, int(r.armed), int(r.canceled), r.stopped_at, r.status);
    std::printf("%s\n", upload_text(r).toUtf8().constData());
    if (r.status)
        std::printf("mapped=%s\n", transfer_error_message(r.status));
    return r.status ? 2 : 0;
}
