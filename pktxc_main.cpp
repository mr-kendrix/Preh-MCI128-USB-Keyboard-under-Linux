// CLI mirror of src/pktxc.py: pktxc <MWF> <out-container> [--ref REF.mwx]
//                           pktxc --verify <MWF> <CONTAINER.mwx>
#include "pktxc.h"
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    std::vector<std::string> arg(argv + 1, argv + argc);
    const bool verify_only = !arg.empty() && arg[0] == "--verify";
    if (verify_only) arg.erase(arg.begin());
    if (arg.size() < 2 || (!verify_only && arg.size() > 3)) {
        std::fprintf(stderr, "usage: pktxc [--verify] <MWF> <OUT.mwx> [REF.mwx]\n"); return 1;
    }
    const std::string ref = (!verify_only && arg.size() == 3) ? arg[2] : "";

    try {
        if (verify_only) {
            const VStats v = verify(arg[0], arg[1]);
            const std::string sw = v.have_sound
                ? "(" + std::to_string(+v.swant[0]) + ", " + std::to_string(+v.swant[1]) + ", " +
                  std::to_string(+v.swant[2]) + ", " + std::to_string(+v.swant[3]) + ")"
                : "None";
            char sg[32];
            std::snprintf(sg, sizeof sg, "(%d, %d, %d, %d)", +v.sgot[0], +v.sgot[1], +v.sgot[2], +v.sgot[3]);
            std::printf("%s vs %s: store 0x0004/0x0135/0x0136 want %02x %02x %02x got %02x %02x %02x %s "
                        "\xc2\xb7 SOUND want %s got %s %s\n",
                        arg[0].c_str(), arg[1].c_str(), v.want[0], v.want[1], v.want[2],
                        v.got[0], v.got[1], v.got[2], v.match ? "MATCH" : "DIFF", sw.c_str(), sg,
                        v.sound_match ? "MATCH" : "DIFF");
            return v.match && v.sound_match ? 0 : 3;
        }
        const PkStats st = build(arg[0], arg[1], ref);
        std::string patches;
        for (size_t i = 0; i < st.patches.size(); ++i) {
            char b[32];
            std::snprintf(b, sizeof b, "%04x:%02x->%02x", st.patches[i][0], st.patches[i][1], st.patches[i][2]);
            if (i) patches += ", ";
            patches += b;
        }
        std::printf("out=%s bytes=%zu blocks=%zu gaps=%zu header_patches=%s byte-equal-fixture=%s\n",
                    st.out.c_str(), st.bytes, st.blocks, st.gaps.size(), patches.c_str(),
                    st.byte_equal_ref ? "True" : "False");
        return st.byte_equal_ref ? 0 : 3;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
