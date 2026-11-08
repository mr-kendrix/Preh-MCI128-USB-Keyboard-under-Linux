#include "transport.h"
#include <libusb-1.0/libusb.h>
#include <hidapi/hidapi.h>
#include <hidapi/hidapi_libusb.h>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
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
static QString g_first8;
QString first8_debug() { return g_first8; }
static int last_status = 0;

// resolve bus node path of 053a:0b01 via libusb, then wrap per interface exactly
// like the proven python path (hid_libusb_wrap_sys_device(fd, iface)).
static QString find_bus_path() {
    if (const char *pp = getenv("PKT_HID_PATH"))
        return pp;
    static libusb_context *cx = nullptr;
    if (!cx && libusb_init(&cx) < 0)
        return QString();
    libusb_device **devs = nullptr;
    const int n = libusb_get_device_list(cx, &devs);
    QString path;
    for (int i = 0; i < n; ++i) {
        libusb_device_descriptor d;
        if (libusb_get_device_descriptor(devs[i], &d) < 0)
            continue;
        if (d.idVendor == 0x053a && d.idProduct == 0x0b01) {
            const int bus = libusb_get_bus_number(devs[i]);
            const int dev = libusb_get_device_address(devs[i]);
            path = QStringLiteral("/dev/bus/usb/%1/%2")
                       .arg(bus, 3, 10, QChar('0')).arg(dev, 3, 10, QChar('0'));
            break;
        }
    }
    libusb_free_device_list(devs, 1);
    return path;
}

static bool board_open_locked() {
    if (board)
        return true;
    const QString p = find_bus_path();
    if (p.isEmpty()) { last_status = LIBUSB_ERROR_NO_DEVICE; return false; }
    const int fd = ::open(p.toUtf8().constData(), O_RDWR);
    if (fd < 0) { last_status = LIBUSB_ERROR_ACCESS; return false; }
    int iface = getenv("PKT_HID_IFACE") ? atoi(getenv("PKT_HID_IFACE")) : -1;
    if (iface < 0) iface = 1;   // vendor interface of the combi keyboard
    board = hid_libusb_wrap_sys_device(fd, iface);
    if (!board) { last_status = LIBUSB_ERROR_NOT_FOUND; return false; }
    return true;
}

int drain_one(int ms, std::vector<int> &out);
static bool send_one(uint8_t code) {
    if (!board_open_locked()) return false;
    const unsigned char r[2] = {0x04, code};
    std::vector<int> junk;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int rc = hid_write(board, r, 2);
        if (rc == 0 || rc == 2) return true;
        drain_one(50, junk);   // board full: let its OUT queue drain
    }
    last_status = -1;
    return false;
}

int drain_one(int ms, std::vector<int> &out) {
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


static int nibble_sum(int addr, int ln) {
    return ((addr >> 12) & 15) + ((addr >> 8) & 15) + ((addr >> 4) & 15) +
           (addr & 15) + ((ln >> 4) & 15) + (ln & 15);
}

// board read verb: EF 0E + ASCII hex "%04X%02X%02X" (addr/len/nibble-sum)
// reply = escape-coded IN codes until 350 ms silence; FC->00 FE|0x80, else literal
bool board_read(int addr, int ln, std::vector<int> &vals) {
    if (!board_open_locked()) return false;
    { std::vector<int> pre; drain_one(150, pre); }   // silence first (leftover IN tail)
    const QString d = QStringLiteral("%1").arg(addr, 4, 16, QChar('0')).toUpper() +
                      QStringLiteral("%1").arg(ln, 2, 16, QChar('0')).toUpper() +
                      QStringLiteral("%1").arg(nibble_sum(addr, ln) & 0xFF, 2, 16, QChar('0')).toUpper();
    if (!send_code(0xEF) || !send_code(0x0E)) return false;
    for (int i = 0; i < d.size(); ++i)
        if (!send_code(static_cast<uint8_t>(d.at(i).unicode()))) return false;
    std::vector<int> codes;
    int quiet_slices = 0;
    for (int t = 0; t < 300; ++t) {   // up to 3 s, end at 350 ms silence
        unsigned char buf[64];
        const int n = hid_read_timeout(board, buf, sizeof buf, 10u);
        if (n > 0) {
            for (int i = 1; i < n; ++i)   // skip report id like python r[1:]
                if (buf[i]) codes.push_back(buf[i]);
            quiet_slices = 0;
        } else if (++quiet_slices >= 35) break;
    }
    for (size_t i = 0; i < codes.size(); ++i) {
        const int c = codes[i];
        if (c == 0xFC) vals.push_back(0x00);
        else if (c == 0xFD) vals.push_back(0x80);
        else if (c == 0xFE && i + 1 < codes.size()) { vals.push_back(codes[++i] | 0x80); }
        else vals.push_back(c);
    }
    return true;
}

int board_checksum16(int *len_word) {   // read first 8 bytes, return board's own checksum
    for (int t2 = 0; t2 < 3; ++t2) {
        std::vector<int> v;
        drain_ms(300, v);   // silence first (leftover IN tail)
        v.clear();
        if (board_read(0, 8, v) && v.size() >= 8) {
            QString hexs;
            for (int i = 0; i < 9 && i < int(v.size()); ++i) hexs += QStringLiteral(" %1").arg(v[i], 2, 16, QChar('0'));
            g_first8 = hexs;
            // framing seen in WinProgrammer captures AND on this board: 31 00 CHK(2) LEN(2)
            if (len_word) *len_word = (v[6] << 8) | v[7];
            return (v[2] << 8) | v[3];
        }
    }
    return -1;
}

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
