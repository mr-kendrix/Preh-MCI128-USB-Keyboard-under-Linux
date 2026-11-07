#include "transport.h"
#include <libusb-1.0/libusb.h>
#include <vector>
#include <cstring>

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

static libusb_context *ctx = nullptr;
static int last_status = 0;

static void note(int code) { last_status = code; }

int device_last_status() { return last_status; }

static libusb_device_handle *find_handle() {
    if (!ctx && libusb_init(&ctx) < 0) return nullptr;
    libusb_device **devs = nullptr;
    const int n = libusb_get_device_list(ctx, &devs);
    libusb_device_handle *found = nullptr;
    for (int i = 0; devs && i < n; ++i) {
        libusb_device_descriptor d;
        libusb_get_device_descriptor(devs[i], &d);
        if (d.idVendor == 0x053a && d.idProduct == 0x0b01) {
            if (libusb_open(devs[i], &found) < 0) found = nullptr;
            break;
        }
    }
    libusb_free_device_list(devs, 1);
    return found;
}

static int find_control_ep(libusb_device_handle *h) {
    libusb_device *dev = libusb_get_device(h);
    libusb_device_descriptor dd;
    if (libusb_get_device_descriptor(dev, &dd) < 0) return -1;
    for (int c = 0; c < dd.bNumConfigurations; ++c) {
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_config_descriptor(dev, c, &cfg) < 0) continue;
        int ep = -1, hid_ep = -1;
        // cfg->interface is an ARRAY of bNumInterfaces entries — walking only
        // interface 0 (the old bug) never saw iface 1, where report-4 lives
        for (int i = 0; i < cfg->bNumInterfaces; ++i) {
            const libusb_interface &itf = cfg->interface[i];
            for (int a = 0; a < itf.num_altsetting; ++a) {
                const libusb_interface_descriptor *alt = &itf.altsetting[a];
                if (alt->bInterfaceNumber != 1) continue;
                for (int e = 0; e < alt->bNumEndpoints; ++e) {
                    const libusb_endpoint_descriptor *ed = &alt->endpoint[e];
                    // strict form: a real control endpoint, 16-byte packets
                    if ((ed->bmAttributes & 0x04) && ed->wMaxPacketSize == 16)
                        ep = ed->bEndpointAddress;
                    // HID-class fallback: the kernel hands the HID class
                    // descriptors back (0x82 = report type 2 = OUT) as "endpoints"
                    if (hid_ep < 0 && (ed->bEndpointAddress & 0x80))
                        hid_ep = ed->bEndpointAddress;
                }
            }
        }
        libusb_free_config_descriptor(cfg);
        if (ep >= 0) return ep;
        if (hid_ep >= 0) return hid_ep;
    }
    return -1;
}

bool device_send(uint8_t code) {
    libusb_device_handle *h = find_handle();
    if (!h) { note(LIBUSB_ERROR_NO_DEVICE); return false; }
    const int ep = find_control_ep(h);
    if (ep < 0) { note(LIBUSB_ERROR_NOT_FOUND); libusb_close(h); return false; }
    unsigned char data[16] = {0};
    data[0] = 0x04;
    data[1] = code;
    // bmRequestType 0x02 = send request, bmIndex 0x01 = interface
    const int r = libusb_control_transfer(h, 0x02, 0x01, 0, ep, data, 16, 1000);
    libusb_close(h);
    note(r < 0 ? r : 0);
    return r >= 0;
}

bool device_present() {
    libusb_device_handle *h = find_handle();
    if (!h) { note(LIBUSB_ERROR_NO_DEVICE); return false; }
    const int ep = find_control_ep(h);
    libusb_close(h);
    if (ep < 0) { note(LIBUSB_ERROR_NOT_FOUND); return false; }
    note(0);
    return true;
}

// every LIBUSB_ERROR_* plus the two board-specific findings, phrased for the UI
const char *transfer_error_message(int status) {
    switch (status) {
    case 0: return "";
    case LIBUSB_ERROR_IO: return "I/O error on the wire";
    case LIBUSB_ERROR_INVALID_PARAM: return "endpoint/iface params wrong";
    case LIBUSB_ERROR_ACCESS: return "access denied — install 99-prehkeytec.rules";
    case LIBUSB_ERROR_NO_DEVICE: return "no Preh board on a bus (053a:0b01)";
    case LIBUSB_ERROR_NOT_FOUND: return "no report-4 control endpoint on interface 1";
    case LIBUSB_ERROR_BUSY: return "board busy — another upload running";
    case LIBUSB_ERROR_TIMEOUT: return "timeout — board never answered";
    case LIBUSB_ERROR_OVERFLOW: return "packet overflow — frame too big";
    case LIBUSB_ERROR_PIPE: return "pipe closed — replug the board";
    case LIBUSB_ERROR_INTERRUPTED: return "transfer interrupted (signal)";
    case LIBUSB_ERROR_NO_MEM: return "out of memory";
    case LIBUSB_ERROR_NOT_SUPPORTED: return "board rejects report-4 uploads";
    case LIBUSB_ERROR_OTHER: return "other libusb failure";
    default: return "libusb failure";
    }
}

#include <hidapi/hidapi.h>
#include <QFile>
#include <QRegularExpression>

int poll_in(unsigned char *buf, int len, int ms) {
    static hid_device *dev = nullptr;
    if (!dev) dev = hid_open(0x053a, 0x0b01, nullptr);
    if (!dev) return -1;
    return hid_read_timeout(dev, buf, (size_t)len, ms);
}

QHash<int,int> load_calib(const QString &path) {
    QHash<int,int> map;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return map;
    const QString j = f.readAll();
    static const QRegularExpression re(QStringLiteral("\"0x([0-9a-fA-F]+)\"\\s*:\\s*([0-9]+)"));

    for (auto it = re.globalMatch(j); it.hasNext(); ) {
        const QRegularExpressionMatch mm = it.next();
        map.insert(mm.captured(1).toInt(nullptr, 16), mm.captured(2).toInt());
    }
    return map;
}
