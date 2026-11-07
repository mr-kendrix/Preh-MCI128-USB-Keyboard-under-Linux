#pragma once
// .MWF -> .mwx container compiler — C++ port of src/pktxc.py v2 (LB6).
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using HeaderMap = std::map<std::string, std::string>;  // LANGUAGE/MSR/MODE/SOUND/RATE lines

// MWF text -> header directive lines (up to the first '!@' line)
HeaderMap header_lines(const std::string &mwf_text);

// store 0x0004 / 0x0135 / 0x0136 from the header (mwxc32.dll rules)
std::array<uint8_t, 3> predicted(const HeaderMap &h);

// SOUND quad -> [Ton Lautst.][Ton Dauer][Fehlalarm Lautst.][Fehlalarm Dauer]
bool sound_quad(const HeaderMap &h, std::array<uint8_t, 4> &out);

// container bytes -> payload without the 'Preh' magic (store addr = index + 4)
std::vector<uint8_t> load_container(const std::string &path);

struct PkStats {
    std::string out;
    size_t bytes = 0, blocks = 0;
    std::vector<uint32_t> gaps;                    // POS ids with no reference block
    std::vector<std::array<int, 3>> patches;       // store, old, new (header-derived)
    bool byte_equal_ref = false;                   // gate: payload == fixtures 00_pos_mci128
};

PkStats build(const std::string &mwf_path, const std::string &out_path,
              const std::string &ref_path);

struct VStats {
    std::array<uint8_t, 3> want{}, got{};
    bool match = false;
    bool have_sound = false;
    std::array<uint8_t, 4> swant{}, sgot{};
    bool sound_match = false;
};

VStats verify(const std::string &mwf_path, const std::string &container_path);
