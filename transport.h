#pragma once
#include <cstdint>
#include <vector>

// split an .mwx payload into report-4 OUT frames: [0x04, code] padded to 16
std::vector<std::vector<uint8_t>> compile_frames(const std::vector<uint8_t> &payload);
// expected IN-ack for frame i: 0x20 | ((i+1)&3)
uint8_t expected_ack(size_t i);
// one control transfer on iface 0x01 of 053a:0b01; data[0]=report
bool device_transfer_out(std::vector<uint8_t> data);
bool device_send(uint8_t code);
