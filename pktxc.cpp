#include "pktxc.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <regex>
#include <set>
#include <stdexcept>
#include <vector>

// learned training pair — same relative layout as the Python workspace
static const std::string MWX_REF = "fixtures/uploads/00_pos_mci128.mwx";

static std::string read_all(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::vector<std::string> split_lines(const std::string &t) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : t) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    if (!cur.empty()) {
        if (cur.back() == '\r') cur.pop_back();
        out.push_back(cur);
    }
    return out;
}

static bool is_ws(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static std::string strip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && is_ws(s[a])) ++a;
    while (b > a && is_ws(s[b - 1])) --b;
    return s.substr(a, b - a);
}
static std::string first_token(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && is_ws(s[a])) ++a;
    size_t b = a;
    while (b < s.size() && !is_ws(s[b])) ++b;
    return s.substr(a, b - a);
}
// str.split(' ', 1)[1] — remainder after the FIRST literal space
static std::string after_first_space(const std::string &s) {
    const auto p = s.find(' ');
    if (p == std::string::npos) throw std::runtime_error("no space");
    return s.substr(p + 1);
}
// str.split(None, 1)[1] — remainder after the first whitespace run
static std::string after_ws_run(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && is_ws(s[a])) ++a;  // split(None) ignores leading ws
    size_t t = a;
    while (t < s.size() && !is_ws(s[t])) ++t;
    if (t == a) throw std::runtime_error("no token");
    size_t sp = t;
    while (sp < s.size() && is_ws(s[sp])) ++sp;
    if (sp == t) throw std::runtime_error("no value");
    return s.substr(sp);
}
static std::vector<std::string> split_c(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

HeaderMap header_lines(const std::string &mwf_text) {
    HeaderMap out;
    for (const auto &ln : split_lines(mwf_text)) {
        if (ln.rfind("!@", 0) == 0) break;
        const std::string key = first_token(ln);
        if (key == "LANGUAGE" || key == "MSR" || key == "MODE" || key == "SOUND" || key == "RATE")
            out[key] = strip(ln);
    }
    return out;
}

std::array<uint8_t, 3> predicted(const HeaderMap &h) {
    static const std::map<std::string, uint8_t> NAMES{
        {"US", 0x00}, {"GR", 0x18}, {"FR", 0x28}, {"UK", 0x30},
        {"SG", 0x40}, {"SF", 0x50}, {"IT", 0x60}, {"SP", 0x70}};
    static const std::map<std::string, uint8_t> A_BITS{
        {"T1", 0x01}, {"T2", 0x02}, {"T3", 0x04}, {"MSR", 0x08},
        {"FCR", 0x10}, {"KL", 0x20}, {"KVK", 0x40}, {"BCR", 0x80}};

    if (!h.count("LANGUAGE")) throw std::runtime_error("LANGUAGE missing");
    const std::string lang = after_first_space(h.at("LANGUAGE"));
    std::vector<std::string> parts = split_c(lang, ',');
    if (!parts.size()) throw std::runtime_error("LANGUAGE malformed");
    auto it = NAMES.find(parts[0]);
    if (it == NAMES.end()) throw std::runtime_error("LANGUAGE unknown");
    uint8_t lb = it->second;
    const std::string shift = parts.size() > 1 ? strip(parts[1]) : "";
    lb = static_cast<uint8_t>((lb & ~8) | (shift == "S" ? 8 : 0));

    int a = 0xBF, b = 0x00;
    std::string msr = h.count("MSR") ? after_first_space(h.at("MSR")) : "";
    for (const auto &tok0 : split_c(msr, ',')) {
        const std::string tok = strip(tok0);
        b |= tok == "NSE" ? 0x08 : tok == "LRC" ? 0x10 : tok == "M3" ? 0x20 : 0;
        if (tok.rfind("NA", 0) == 0) a &= ~8;
        if (tok.rfind("N1", 0) == 0) a &= ~1;
        if (tok.rfind("N2", 0) == 0) a &= ~2;
        if (tok.rfind("N3", 0) == 0) a &= ~4;
    }
    {
        std::smatch m;
        const std::string mode = h.count("MODE") ? h.at("MODE") : "";
        static const std::regex sign_re("([+-])([A-Z0-9]+)");
        for (auto re = std::sregex_iterator(mode.begin(), mode.end(), sign_re);
             re != std::sregex_iterator(); ++re) {
            const bool neg = (*re)[1].str() == "-";
            const std::string name = (*re)[2].str();
            if (auto bit = A_BITS.find(name); bit != A_BITS.end()) {
                if (!neg) a |= bit->second; else a &= ~bit->second;
            } else if (name == "BA0") { a |= 0x40; b &= ~1; }
            else if (name == "BA1") { a &= ~0x40; b &= ~1; }
            else if (name == "BA2") { a |= 0x40; b |= 1; }
            else if (name == "EF" || name == "EC" || name == "SE") {
                const int bit = name == "EF" ? 0x02 : name == "EC" ? 0x04 : 0x08;
                if (!neg) b |= bit; else b &= ~bit;
            } else if (name == "IEK" || name == "FFA") {
                if (neg) b |= name == "IEK" ? 0x40 : 0x80;
            } else if (name == "OPOS" && !neg) {
                lb &= 0x0F;
            }
        }
    }
    return {static_cast<uint8_t>(lb), static_cast<uint8_t>(a & 0xFF), static_cast<uint8_t>(b & 0xFF)};
}

bool sound_quad(const HeaderMap &h, std::array<uint8_t, 4> &out) {
    if (!h.count("SOUND")) return false;
    std::vector<int> v;
    for (const auto &t : split_c(after_ws_run(h.at("SOUND")), ','))
        v.push_back(std::stoi(t));  // throws on garbage like Python's int()
    if (v.size() != 4) return false;
    out = {static_cast<uint8_t>(v[0] * 255 / 100), static_cast<uint8_t>(v[1] / 20),
           static_cast<uint8_t>(v[2] * 255 / 100), static_cast<uint8_t>(v[3] / 20)};
    return true;
}

std::vector<uint8_t> load_container(const std::string &path) {
    const std::string data = read_all(path);
    if (data.empty()) throw std::runtime_error("cannot read " + path);
    if (data.rfind("Preh", 0) == 0) return std::vector<uint8_t>(data.begin() + 4, data.end());
    return std::vector<uint8_t>(data.begin(), data.end());
}

static std::vector<uint32_t> pos_ids(const std::string &mwf_text) {
    static const std::regex key_re(R"re(!@KEYPRINT:([A-H]\d\d)[^;]*"POS\\line(\d+)")re");
    std::set<uint32_t> want;
    for (auto re = std::sregex_iterator(mwf_text.begin(), mwf_text.end(), key_re);
         re != std::sregex_iterator(); ++re)
        want.insert(static_cast<uint32_t>(std::stoul((*re)[2].str())));
    return std::vector<uint32_t>(want.begin(), want.end());
}

PkStats build(const std::string &mwf_path, const std::string &out_path,
              const std::string &ref_path) {
    const std::string mwf_text = read_all(mwf_path);
    if (mwf_text.empty()) throw std::runtime_error("cannot read " + mwf_path);
    const std::vector<uint8_t> ref = load_container(ref_path.empty() ? MWX_REF : ref_path);

    // rebuild: reference keeps its own bytes; POS ids with no matching block are gaps
    const std::vector<uint32_t> want = pos_ids(mwf_text);
    std::set<uint32_t> have;
    for (size_t off = 0; off < ref.size(); off += 8) {
        // mirror Python: struct.unpack_from('<H') on the 8-byte block raises on <2-byte tails
        if (off + 2 > ref.size()) throw std::runtime_error("struct.error: unpack_from requires a buffer of at least 2 bytes");
        const uint32_t pid = ref[off] | (static_cast<uint32_t>(ref[off + 1]) << 8);
        if (std::binary_search(want.begin(), want.end(), pid)) have.insert(pid);
    }
    std::vector<uint32_t> gaps;
    for (uint32_t w : want)
        if (!have.count(w)) gaps.push_back(w);

    std::vector<uint8_t> buf = ref;
    PkStats st;
    st.out = out_path;
    st.gaps = gaps;

    static const std::map<std::string, uint8_t> RATE_CLASS{{"FAST", 0xC9}, {"VERY SLOW", 0x75}};
    const HeaderMap h = header_lines(mwf_text);
    std::map<uint16_t, uint8_t> want_bytes;
    if (h.count("LANGUAGE") && h.count("MSR") && h.count("MODE")) {
        const auto p = predicted(h);
        want_bytes[0x0004] = p[0]; want_bytes[0x0135] = p[1]; want_bytes[0x0136] = p[2];
    }
    std::array<uint8_t, 4> sq{};
    if (sound_quad(h, sq))
        for (int i = 0; i < 4; ++i) want_bytes[0x0124 + i] = sq[i];
    if (h.count("RATE")) {
        const std::vector<std::string> toks = split_c(after_ws_run(h.at("RATE")), ',');
        const std::string tok = strip(toks.back());
        if (auto r = RATE_CLASS.find(tok); r != RATE_CLASS.end()) want_bytes[0x0007] = r->second;
    }
    for (auto [store, val] : want_bytes) {
        const size_t i = store - 4;
        if (i >= buf.size()) continue;
        if (buf[i] != val) {
            const uint8_t old = buf[i];
            buf[i] = val;
            st.patches.push_back({static_cast<int>(store), old, val});
        }
    }

    std::ofstream o(out_path, std::ios::binary);
    if (!o) throw std::runtime_error("cannot write " + out_path);
    o << "Preh" << std::string(buf.begin(), buf.end());

    const std::vector<uint8_t> ref_full = load_container(MWX_REF);
    st.bytes = 4 + buf.size();
    st.blocks = buf.size() / 8;
    st.byte_equal_ref = (buf == ref_full);
    return st;
}

VStats verify(const std::string &mwf_path, const std::string &container_path) {
    const std::vector<uint8_t> payload = load_container(container_path);
    const HeaderMap h = header_lines(read_all(mwf_path));
    VStats v;
    v.want = predicted(h);
    const auto at = [&](size_t i) { return i < payload.size() ? payload[i] : uint8_t(0); };
    v.got = {at(0x0000), at(0x0131), at(0x0132)};
    v.match = v.want == v.got;
    v.have_sound = sound_quad(h, v.swant);
    v.sgot = {at(0x0120), at(0x0121), at(0x0122), at(0x0123)};
    v.sound_match = v.have_sound && v.swant == v.sgot;
    return v;
}
