#pragma once
// LB5: the upload engine — report-4 frames pushed through device_send() with a
// progress callback and a mapped error reason.  Kept out of gui_main.cpp so the
// GUI (LB4) and the compiler (LB6) can land independently.
#include <QString>
#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <cstdint>
#include <functional>
#include <vector>
#include "pmap.h"
#include "transport.h"

struct UploadResult {
    int frames = 0;        // compile_frames() output size
    int sent = 0;          // frames handed over to device_send()
    int stopped_at = -1;   // frame index that failed / cancelled (-1 = ran out)
    int failed_code = -1;  // the code that bounced, -1 = none
    int status = 0;        // LIBUSB_ERROR_* of the bounce (0 = ok)
    bool canceled = false; // progress callback said stop (cancel button)
    bool armed = false;    // did we actually write the board?
};

// payload -> report-4 frames -> one device_send() per code; progress(done,total)
// returning false aborts (the QProgressDialog cancel button)
UploadResult run_upload(const std::vector<uint8_t> &payload, bool armed,
                        const std::function<bool(int, int)> &progress);

// payload for the upload: an .mwx container when given, else the pmap's own bytes
std::vector<uint8_t> payload_of(const Pmap &pm, const QString &container);

// one-line UI text for an attempt (arm gate, cancel, bounce, success)
QString upload_text(const UploadResult &r);

// the GUI affordance: button + QProgressDialog + status line, mountable in one
// line from any window:  UploadPanel panel(&win, &pm, container, armed);
class UploadPanel : public QWidget {
public:
    explicit UploadPanel(QWidget *parent, const Pmap *pm = nullptr,
                         const QString &container = QString(), bool armed = false);
    void run_now();                       // the click handler, also callable headless
    QString status_text() const;          // last attempt's one-line report
private:
    const Pmap *pm_ = nullptr;
    QString container_;
    bool armed_ = false;
    QString last_;
    QPushButton *btn_ = nullptr;
    QLabel *status_ = nullptr;
};
