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
        int ep = -1;
        for (int a = 0; a < cfg->interface->num_altsetting && ep < 0; ++a) {
            const libusb_interface_descriptor *alt = &cfg->interface->altsetting[a];
            if (alt->bInterfaceNumber != 1) continue;
            for (int e = 0; e < alt->bNumEndpoints; ++e) {
                const libusb_endpoint_descriptor *ed = &alt->endpoint[e];
                // control endpoint: attr bit 0x04, packet size 16
                if ((ed->bmAttributes & 0x04) && ed->wMaxPacketSize == 16) {
                    ep = ed->bEndpointAddress;
                    break;
                }
            }
        }
        libusb_free_config_descriptor(cfg);
        if (ep >= 0) return ep;
    }
    return -1;
}

bool device_send(uint8_t code) {
    libusb_device_handle *h = find_handle();
    if (!h) return false;
    const int ep = find_control_ep(h);
    if (ep < 0) { libusb_close(h); return false; }
    unsigned char data[16] = {0};
    data[0] = 0x04;
    data[1] = code;
    // bmRequestType 0x02 = send request, bmIndex 0x01 = interface
    const int r = libusb_control_transfer(h, 0x02, 0x01, 0, ep, data, 16, 1000);
    libusb_close(h);
    return r >= 0;
}
