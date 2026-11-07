#pragma once
#include <cstdint>
#include <vector>
#include <QHash>
#include <QString>

// split an .mwx payload into report-4 OUT frames: [0x04, code] padded to 16
std::vector<std::vector<uint8_t>> compile_frames(const std::vector<uint8_t> &payload);
// expected IN-ack for frame i: 0x20 | ((i+1)&3)
uint8_t expected_ack(size_t i);
// one control transfer on iface 0x01 of 053a:0b01; data[0]=report
bool device_transfer_out(std::vector<uint8_t> data);
bool device_send(uint8_t code);

// raw LIBUSB_ERROR_* of the last device_send() (0 = ok)
int device_last_status();
// board on a bus + control endpoint there, without sending anything
bool device_present();
// human-readable reason for a status code ("" when ok)
const char *transfer_error_message(int status);

// IN stream: raw hidapi read of one 5-byte report [0x04 …]; ms=0 non-blocking
int poll_in(unsigned char *buf, int len, int ms);
// calib JSON "0xSCNC" -> POSKey (empty when file missing)
QHash<int, int> load_calib(const QString &path);
