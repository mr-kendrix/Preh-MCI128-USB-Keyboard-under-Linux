#include "upload.h"
#include <libusb-1.0/libusb.h>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QProgressDialog>
#include <QDateTime>
#include <QDir>
#include <cstdio>
#include <functional>

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

// simplified rule confirmed via vendor oracle (mwxc32.dll under wine):
// checksum16 = sum of ALL body bytes mod 2^16 (body = file without "Preh" magic;
// the length word at body[2:3] counts the whole file incl. magic)
static uint16_t payload_checksum16(const std::vector<uint8_t> &body) {
    uint32_t sum = 0;
    for (uint8_t b : body) sum += b;
    return uint16_t(sum & 0xFFFF);
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

    // BACKUP before touching the board: read the live keytable back (EF 0E verb),
    // refuse to overwrite if the read fails. Dump -> /tmp/pktup_backup_*.mwx
    {
        QByteArray bak;
        bool ok = true;
        for (int addr = 0; addr + 8 <= 0x5D8; addr += 8) {   // 1496 B keytable region
            std::vector<int> v;
            if (!board_read(addr, 8, v) || v.size() < 8) { ok = false; break; }
            for (int i = 0; i < 8; ++i) bak.append(uint8_t(v[i]));
        }
        if (!ok) {
            r.status = LIBUSB_ERROR_TIMEOUT;
            r.err_msg = QStringLiteral("backup read failed — refusing blind overwrite");
            return r;
        }
        const QString dir = QStringLiteral("%1/.local/share/pkteditor").arg(
                                QDir::homePath());
        QDir().mkdir(dir);
        const QString path = dir + QStringLiteral("/pktup_backup_%1.mwx")
                                 .arg(QDateTime::currentDateTime().toString("yyyyMMdd_Hhmmss"));
        QFile bf(path);
        if (bf.open(QIODevice::WriteOnly)) bf.write("Preh" + bak);
        r.backup_path = path;
    }

    std::vector<int> junk;
    // StartWriteDataMWX (identify EF 59 dropped: not in any verified capture)
    if (!send_code(0xEF) || !send_code(0x00)) {
        r.failed_code = 0x00; r.status = device_last_status(); return r;
    }
    drain_ms(200, junk);

    for (size_t bi = 0; bi < body.size(); bi += 8) {
        const int idx = int(bi / 8);
        const int seq = (idx + 1) & 3;
        const int want = 0x20 | seq;             // per-block ack must be waited for
        bool ok = false;
        for (int attempt = 0; attempt < 50 && !ok; ++attempt) {
            const std::vector<uint8_t> frame =
                vendor_frame(4 + 8 * idx, body.data() + bi, seq);
            bool sent = true;
            for (uint8_t code : frame) {
                if (!send_code(code)) {
                    r.failed_code = code;
                    r.status = device_last_status();
                    r.stopped_at = idx;
                    return r;
                }
            }
            r.sent = idx + 1;
            // poll IN up to 150 ms for the exact ack; F4 = board abort
            for (int slice = 0; slice < 15 && !ok; ++slice) {
                std::vector<int> codes;
                drain_one(10, codes);
                for (int b : codes) {
                    if (b == want) { ++r.acks; ok = true; break; }
                    if (b == 0xF4) {
                        drain_ms(100, junk);
                        r.canceled = true;
                        r.stopped_at = idx;
                        return r;
                    }
                    if (b >= 0x20 && b <= 0x23) ++r.acks;
                    else if (b) ++r.noise;
                }
            }
            if (progress && !progress(r.sent, r.frames)) {
                r.canceled = true; r.stopped_at = idx; return r;
            }
        }
        if (!ok) {   // ack never arrived: cancel cleanly, no half-fed table
            send_code(0xEF); send_code(0xF1);
            drain_ms(100, junk);
            r.canceled = true; r.stopped_at = idx;
            return r;
        }
    }
    drain_ms(150, junk);
    r.finished = true;
    // FinishWriteDataMWX + expected ack 0x02
    send_code(0xEF); send_code(0x12);
    drain_ms(150, junk);
// verify: board's own checksum (Claude sum rule matches on this board: sum(body[0:len-2]))
    const uint16_t cs = payload_checksum16(body);
    int len_word = -1;
    const int board_cs = board_checksum16(&len_word);
    r.checksum16 = cs;
    r.board_checksum16 = board_cs;
    r.verified = (board_cs >= 0 && board_cs == cs);
    return r;
}

QString upload_text(const UploadResult &r) {
    if (r.canceled) {
        const QString tail = r.armed ? QStringLiteral("board left half-fed, verify before resending")
                                     : QStringLiteral("nothing was sent");
        return QStringLiteral("upload cancelled at block %1/%2 — %3")
                   .arg(r.stopped_at + 1).arg(r.frames).arg(tail);
    }
    if (!r.armed)
        return QStringLiteral("Upload NOT ARMED — %1 blocks compiled, nothing sent "
                              "(rerun with --arm-upload)").arg(r.frames);
    if (r.status) {
        const QString code = QStringLiteral("%1").arg(r.failed_code, 2, 16, QChar('0')).toUpper();
        return QStringLiteral("upload FAILED at block %1/%2 (code %3) — %4")
                   .arg(r.stopped_at + 1).arg(r.frames).arg(code)
                   .arg(QString::fromUtf8(transfer_error_message(r.status)));
    }
    QString t = QStringLiteral("upload: %1 blocks, %2 ACKs, EF 12 %3")
               .arg(r.frames).arg(r.acks)
               .arg(r.finished ? QStringLiteral("sent") : QStringLiteral("NOT sent"));
    if (r.board_checksum16 < 0)
        t += QStringLiteral(" — board checksum unread");
    else {
        t += QStringLiteral(", board checksum %1 vs payload %2%3")
                   .arg(r.board_checksum16, 0, 16).arg(r.checksum16, 0, 16)
                   .arg(r.verified ? QStringLiteral(" MATCH") : QStringLiteral(" DIFF — board content suspect"));
    }
    return t;
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
