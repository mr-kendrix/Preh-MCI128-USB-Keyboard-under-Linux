/* variant probe: does xkb apply layout:variant for '^' ?
 * gcc tools/xkb_variant_probe.c -o /tmp/xkbvp -lxkbcommon
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

int main(int argc, char **av) {
    if (argc < 2) { printf("usage: xkbvp de[:variant]\n"); return 2; }
    static char lo[64], va[64];
    const char *s = av[1];
    const char *c1 = strchr(s, ':');
    if (c1) { strncpy(lo, s, (size_t)(c1 - s)); lo[c1 - s] = 0; }
    else { strncpy(lo, s, sizeof lo - 1); lo[sizeof lo - 1] = 0; }
    if (c1) strncpy(va, c1 + 1, sizeof va - 1);
    va[sizeof va - 1] = 0;
    struct xkb_rule_names rn;
    memset(&rn, 0, sizeof rn);
    rn.layout = lo;
    if (c1) rn.variant = va;
    struct xkb_context *cx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_names(cx, &rn, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) { printf("KEYMAP LOAD FAILED\n"); return 1; }
    printf("layout=%s variant=%s\n", lo, rn.variant ? va : "(none)");
    int found = 0;
    for (xkb_keycode_t kc = 8; kc < 512; ++kc) {
        int nlay = xkb_keymap_num_layouts_for_key(km, kc);
        for (int lay_i = 0; lay_i < nlay && lay_i < 6; ++lay_i) {
            int nl = xkb_keymap_num_levels_for_key(km, kc, lay_i);
            for (int lev = 0; lev < nl && lev < 4; ++lev) {
                const xkb_keysym_t *one = 0;
                int r = xkb_keymap_key_get_syms_by_level(km, kc, lay_i, lev, &one);
                if (!one) continue;
                (void)r;
                if (*one == 0x5E) {
                    printf("  kc=%d ev=%d layout%d lev%d DIRECT U+005E\n", (int)kc, (int)kc - 8, lay_i, lev);
                    ++found;
                }
                char nm[32];
                if (xkb_keysym_get_name(*one, nm, sizeof nm) == 1 && strcmp(nm, "dead_circumflex") == 0)
                    printf("  kc=%d ev=%d layout%d lev%d NAME dead_circumflex\n", (int)kc, (int)kc - 8, lay_i, lev);
            }
        }
    }
    if (!found) printf("  no direct '^' codepoint found\n");
    return 0;
}
