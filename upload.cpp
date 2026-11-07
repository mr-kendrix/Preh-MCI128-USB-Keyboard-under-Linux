#include "upload.h"
#include <libusb-1.0/libusb.h>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QProgressDialog>
#include <cstdio>

std::vector<uint8_t> payload_of(const Pmap &pm, const QString &container) {
    QByteArray raw;
    if (!container.isEmpty()) {
        QFile f(container);
        if (f.open(QIODevice::ReadOnly)) raw = f.readAll();
    } else {
        raw = serialize_pmap(pm).toLatin1();
    }
    return std::vector<uint8_t>(raw.constData(), raw.constData() + raw.size());
}

UploadResult run_upload(const std::vector<uint8_t> &payload, bool armed,
                        const std::function<bool(int, int)> &progress) {
    UploadResult r;
    r.armed = armed;
    // container = "Preh" magic + body; blocks of 8 bytes over the body
    if (payload.size() < 4 || payload[0] != 'P' || payload[1] != 'r' ||
        payload[2] != 'e' || payload[3] != 'h') {
        r.status = LIBUSB_ERROR_INVALID_PARAM;
        r.stopped_at = 0;
        return r;
    }
    std::vector<uint8_t> body(payload.begin() + 4, payload.end());
    while (body.size() % 8) body.push_back(0);   // zero-pad the last block
    r.frames = int(body.size() / 8);
    if (!armed)
        return r;                                // plan only, nothing on the wire

    std::vector<int> junk;
    // identify: EF 59 31 00 (GetWriteStatus)
    for (uint8_t c : {0xEF, 0x59, 0x31, 0x00})
        if (!send_code(c)) { r.failed_code = c; r.status = device_last_status(); return r; }
    drain_ms(300, junk);
    // StartWriteDataMWX
    if (!send_code(0xEF) || !send_code(0x00)) {
        r.failed_code = 0x00; r.status = device_last_status(); return r;
    }
    drain_ms(200, junk);

    for (size_t bi = 0; bi < body.size(); bi += 8) {
        const int idx = int(bi / 8);
        const std::vector<uint8_t> frame =
            vendor_frame(4 + 8 * idx, body.data() + bi, (idx + 1) & 3);
        for (uint8_t code : frame) {
            if (!send_code(code)) {
                r.failed_code = code;
                r.status = device_last_status();
                r.stopped_at = idx;
                return r;
            }
        }
        drain_ms(150, junk);
        for (int a : junk) {
            if (a >= 0x20 && a <= 0x23) ++r.acks;
            else if (a) ++r.noise;
        }
        r.sent = idx + 1;
        if (progress && !progress(r.sent, r.frames)) {
            r.canceled = true;
            r.stopped_at = idx;
            return r;
        }
    }
    // FinishWriteDataMWX only if the board accepted nearly every block
    if (r.acks >= r.frames - 2) {
        send_code(0xEF);
        send_code(0x12);
        drain_ms(500, junk);
        r.finished = true;
    }
    return r;
}

QString upload_text(const UploadResult &r) {
    if (r.canceled) {
        const QString tail = r.armed ? QStringLiteral("board left half-fed, verify before resending")
                                     : QStringLiteral("nothing was sent");
        return QStringLiteral("upload cancelled at frame %1/%2 — %3")
                   .arg(r.stopped_at + 1).arg(r.frames).arg(tail);
    }
    if (!r.armed)
        return QStringLiteral("Upload NOT ARMED — %1 frames compiled, nothing sent "
                              "(rerun with --arm-upload)").arg(r.frames);
    if (r.status) {
        const QString code = QStringLiteral("%1").arg(r.failed_code, 2, 16, QChar('0')).toUpper();
        return QStringLiteral("upload FAILED at frame %1/%2 (code %3) — %4")
                   .arg(r.stopped_at + 1).arg(r.frames).arg(code)
                   .arg(QString::fromUtf8(transfer_error_message(r.status)));
    }
    return QStringLiteral("upload: %1 blocks, %2 ACKs (EF 12 %3)")
               .arg(r.frames).arg(r.acks)
               .arg(r.finished ? QStringLiteral("sent — table finalised, verify keytable")
                               : QStringLiteral("NOT sent — board accepted too few blocks"));
}

UploadPanel::UploadPanel(QWidget *parent, const Pmap *pm, const QString &container, bool armed)
    : QWidget(parent), pm_(pm), container_(container), armed_(armed) {
    btn_ = new QPushButton(this);
    btn_->setText(QStringLiteral("\u2191 Upload"));
    btn_->setToolTip(QStringLiteral("push the report-4 frames to the board"));
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setText(armed ? QStringLiteral("press Upload")
                           : QStringLiteral("Upload NOT ARMED — nothing sent"));
    // click -> the same run_now() the headless gates call
    QObject::connect(btn_, &QPushButton::clicked, this, [this] { run_now(); });
}

void UploadPanel::run_now() {
    const Pmap empty;
    const auto payload = payload_of(pm_ ? *pm_ : empty, container_);
    QWidget *host = parent() ? qobject_cast<QWidget *>(parent()) : this;
    QProgressDialog prog(QStringLiteral("pushing report-4 frames to the board…"),
                         QStringLiteral("Cancel"), 0, 1, host);
    prog.setMinimumDuration(0); // show it even for a fast run
    const UploadResult r = run_upload(payload, armed_, [&](int done, int total) {
        prog.setRange(0, total);
        prog.setValue(done);
        return !prog.wasCanceled();
    });
    last_ = upload_text(r);
    status_->setText(last_);
}

QString UploadPanel::status_text() const { return last_; }
