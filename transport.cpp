#include "transport.h"
#include <libusb-1.0/libusb.h>
#include <hidapi/hidapi.h>
#include <vector>
#include <cstring>
#include <QFile>
#include <QRegularExpression>

std::vector<std::vector<uint8_t>> compile_frames(const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> codes;
    for (size_t i = 0; i < payload.size(); ++i) {
        if (payload[i] == 0x04 && i + 1 < payload.size()) { codes.push_back(payload[++i]); continue; }
        codes.push_back(payload[i]);
    }
    std::vector<std::vector<uint8_t>> out;
    for (size_t i = 0; i < codes.size(); i += 8) {
        std::vector<uint8_t> f(16, 0);
        for (size_t j = 0; j < 8 && i + j < codes.size(); ++j) {
            f[2 * j] = 0x04;
            f[2 * j + 1] = codes[i + j];
        }
        out.push_back(f);
    }
    return out;
}

uint8_t expected_ack(size_t i) { return 0x20 | static_cast<uint8_t>((i + 1) & 3); }

// ------------------------------------------------------------- board wire ----
// The vendor tool sends every code as one 2-byte OUT report [04 <code>] — hidapi
// issues the HID SET_REPORT control request for us (interface 1), so no hand-rolled
// control_transfer with invented request bytes is needed anywhere.
static hid_device *board = nullptr;
static int last_status = 0;

static bool board_open_locked() {
    if (!board) {
        board = hid_open(0x053a, 0x0b01, nullptr);
        if (!board) { last_status = LIBUSB_ERROR_NO_DEVICE; return false; }
    }
    return true;
}

static bool send_one(uint8_t code) {
    if (!board_open_locked()) return false;
    const unsigned char r[2] = {0x04, code};
    const int rc = hid_write(board, r, 2);
    if (rc != 0) { last_status = LIBUSB_ERROR_IO; return false; }
    return true;
}

static int drain_one(int ms, std::vector<int> &out) {
    out.clear();
    if (!board_open_locked()) return -1;
    unsigned char buf[64];
    const int n = hid_read_timeout(board, buf, sizeof buf, (unsigned)ms);
    for (int i = 0; i < n; ++i)
        if (buf[i]) out.push_back(buf[i]);
    return n;
}

// vendor frame dialect — WinProgrammer 2.5 / Mwxusb.dll 4.6, byte-exact against
// the USBPcap capture (18 codes per block, checksum over c[2..17] only)
std::vector<uint8_t> vendor_frame(int addr, const uint8_t *p, int seq) {
    int x = 0;
    for (int i = 0; i < 8; ++i) x ^= p[i];
    std::vector<int> c;
    c.push_back(0xEF); c.push_back(0x0F);
    c.push_back(((addr >> 10) & 0x3F) | 0x80);
    c.push_back(((addr >> 4) & 0x3F) | 0x40);
    c.push_back(((seq & 3) << 4) | (addr & 0x0F) | 0x04);
    c.push_back((p[0] >> 2) | 0x80);
    c.push_back((((p[0] & 0xF3) << 4) | 0x40 | (p[1] >> 4)) & 0xFF);
    c.push_back((((p[1] & 0xCF) << 2) | (p[2] >> 6)) & 0xFF);
    c.push_back((p[2] & 0x3F) | 0x80);
    c.push_back(((p[3] >> 2) | 0x40) & 0xFF);
    c.push_back((((p[3] & 0xF3) << 4) | (p[4] >> 4)) & 0xFF);
    c.push_back((((p[4] & 0xCF) << 2) | 0x80 | (p[5] >> 6)) & 0xFF);
    c.push_back((p[5] & 0x3F) | 0x40);
    c.push_back((p[6] >> 2) & 0xFF);
    c.push_back((((p[6] & 0xF3) << 4) | 0x80 | (p[7] >> 4)) & 0xFF);
    c.push_back((((p[7] & 0xCF) << 2) | 0x40 | (x >> 6)) & 0xFF);
    c.push_back(x & 0x3F);
    int chk = 0;
    for (size_t i = 2; i < c.size(); ++i) chk ^= c[i];
    c.push_back(chk & 0x3F);
    std::vector<uint8_t> out;
    for (int v : c) out.push_back(uint8_t(v));
    return out;
}

bool device_send(uint8_t code) { return send_one(code); }

bool send_code(uint8_t code) { return send_one(code); }

int drain_ms(int ms, std::vector<int> &out) { return drain_one(ms, out); }

int device_last_status() { return last_status; }

bool device_present() {
    hid_device *h = hid_open(0x053a, 0x0b01, nullptr);
    if (!h) { last_status = LIBUSB_ERROR_NO_DEVICE; return false; }
    hid_close(h);
    last_status = 0;
    return true;
}

// every LIBUSB_ERROR_* plus the two board-specific findings, phrased for the UI
const char *transfer_error_message(int status) {
    switch (status) {
    case 0: return "";
    case LIBUSB_ERROR_IO: return "I/O error on the wire";
    case LIBUSB_ERROR_INVALID_PARAM: return "container too small or bad magic";
    case LIBUSB_ERROR_ACCESS: return "access denied — add 99-prehkeytec.rules uacces rule";
    case LIBUSB_ERROR_NO_DEVICE: return "no Preh board (053a:0b01) on any interface";
    case LIBUSB_ERROR_NOT_FOUND: return "no HID interface for report-4";
    case LIBUSB_ERROR_BUSY: return "board busy — another upload running";
    case LIBUSB_ERROR_TIMEOUT: return "timeout — board never answered";
    case LIBUSB_ERROR_OVERFLOW: return "packet overflow — frame too big";
    case LIBUSB_ERROR_PIPE: return "pipe closed — replug the board";
    case LIBUSB_ERROR_NO_MEM: return "out of memory";
    default: return "wire failure";
    }
}

// IN stream: reuse the same board handle the upload opened
int poll_in(unsigned char *buf, int len, int ms) {
    if (!board_open_locked()) return -1;
    return hid_read_timeout(board, buf, (size_t)len, (unsigned)ms);
}

#include <QFile>
#include <QRegularExpression>

QHash<int,int> load_calib(const QString &path) {
    QHash<int,int> map;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return map;
    const QString j = f.readAll();
    static const QRegularExpression re(QStringLiteral("\"0x([0-9a-fA-F]+)\"\\s*:\\s*([0-9]+)"));
    QRegularExpressionMatchIterator it = re.globalMatch(j);
    while (it.hasNext()) {
        const QRegularExpressionMatch mm = it.next();
        map.insert(mm.captured(1).toInt(nullptr, 16), mm.captured(2).toInt());
    }
    return map;
}
