#include "upload.h"
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
    const auto frames = compile_frames(payload);
    UploadResult r;
    r.frames = int(frames.size());
    r.armed = armed;
    // a board-less run refuses instead of failing on frame one
    if (armed && !device_present()) {
        r.status = device_last_status();
        r.stopped_at = 0;
        return r;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        if (armed) {
            // frame layout: [0x04 code] x8 — the code is the odd byte
            for (size_t j = 1; j < frames[i].size(); j += 2) {
                if (!device_send(frames[i][j])) {
                    r.failed_code = frames[i][j];
                    r.status = device_last_status();
                    r.stopped_at = int(i);
                    return r;
                }
            }
        }
        r.sent = int(i) + 1;
        if (progress && !progress(r.sent, r.frames)) {
            r.canceled = true;
            r.stopped_at = int(i);
            return r;
        }
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
    return QStringLiteral("upload: %1 frames, %2 sent · EF 12 not sent — verify with "
                          "tests/keytable_match.py before trusting the table")
               .arg(r.frames).arg(r.sent);
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
