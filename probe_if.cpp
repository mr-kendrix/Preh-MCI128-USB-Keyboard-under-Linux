// probe_if — read-only: what libusb sees on the Preh board (no transfers)
#include <libusb-1.0/libusb.h>
#include <cstdio>

int main() {
    libusb_context *c = nullptr;
    if (libusb_init(&c) < 0) { std::printf("init fail\n"); return 1; }
    libusb_device **devs = nullptr;
    const int n = libusb_get_device_list(c, &devs);
    for (int i = 0; devs && i < n; ++i) {
        libusb_device_descriptor d{};
        libusb_get_device_descriptor(devs[i], &d);
        if (d.idVendor != 0x053a || d.idProduct != 0x0b01) continue;
        libusb_device_handle *h = nullptr;
        if (libusb_open(devs[i], &h) < 0) { std::printf("open fail\n"); break; }
        for (int cc = 0; cc < d.bNumConfigurations; ++cc) {
            libusb_config_descriptor *cfg = nullptr;
            if (libusb_get_config_descriptor(devs[i], cc, &cfg) < 0) continue;
            for (int a = 0; a < cfg->bNumInterfaces; ++a) {
                const libusb_interface &itf = cfg->interface[a];
                std::printf("iface=%d nalt=%d\n", a, itf.num_altsetting);
                for (int s = 0; s < itf.num_altsetting; ++s) {
                const libusb_interface_descriptor &alt = itf.altsetting[s];
                std::printf("  alt iface=%d nEP=%d\n", alt.bInterfaceNumber, alt.bNumEndpoints);
                for (int e = 0; e < alt.bNumEndpoints; ++e) {
                    const libusb_endpoint_descriptor &ed = alt.endpoint[e];
                    std::printf("    ep=0x%02x attr=0x%02x max=%d interval=%d\n",
                                ed.bEndpointAddress, ed.bmAttributes,
                                ed.wMaxPacketSize, ed.bInterval);
                }
                }
            }
            libusb_free_config_descriptor(cfg);
        }
        libusb_close(h);
    }
    libusb_free_device_list(devs, 1);
    return 0;
}
