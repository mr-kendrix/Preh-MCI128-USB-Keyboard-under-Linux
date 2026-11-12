// pmaprt — Phase-0 golden test: lossless round trip on real pmaps (2026-11-12).
// PASS criteria per file: parse OK; serialize(parse(x)) == x byte-exact;
// element counts seen by the old regex parser == counts seen by the DOM.
// Edit test: change one Value text -> exactly that payload differs.
// Negative tests: attributes / mismatched close -> dom_parse must FAIL.
#include "pmap_dom.h"
#include "pmap.h"
#include <QStringList>
#include <QFile>
#include <cstdio>
#include <cstring>

static int count_tag_dom(const DomDoc &d, const QString &tag)
{
    int c = 0;
    for (const DomSlot &s : d.pool)
        if (!s.is_text && s.name == tag)
            ++c;
    return c;
}

static int run_file(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { std::printf("MISS %s\n", path.toUtf8().constData()); return 2; }
    const QByteArray raw = f.readAll();
    DomDoc d;
    if (!dom_parse(raw, d)) { std::printf("PARSE-FAIL %s (%lld B)\n", path.toUtf8().constData(), (long long)raw.size()); return 1; }
    const QByteArray back = dom_serialize(d);
    const bool same = (back == raw);
    // Phase-0 idempotency: applying the FILE's own view must change NOTHING
    const Pmap own = dom_view(d);
    QString aerr;
    const QByteArray again = dom_apply_model(d, own, aerr);
    const bool idem = (again == raw && aerr.isEmpty());
    // old model view vs DOM counts
    const Pmap old = parse_pmap(QString::fromLatin1(raw));     // latin1 = byte-preserving
    struct Row { const char *tag; int oldc; int domc; } rows[] = {
        {"MAP", 0, count_tag_dom(d, "MAP")},
        {"POSKey", 0, count_tag_dom(d, "POSKey")},
        {"LAYER", 0, count_tag_dom(d, "LAYER")},
        {"Type", 0, count_tag_dom(d, "Type")},
        {"Value", 0, count_tag_dom(d, "Value")},
    };
    // old parser: count via regex-free model
    int maps = old.maps.size(), posk = maps, layers = 0, types = 0, vals = 0;
    for (const Map &m : old.maps) { posk += 0; for (const Layer &l : m.layers) { ++layers; ++types; ++vals; } }
    rows[0].oldc = maps; rows[1].oldc = posk; rows[2].oldc = layers; rows[3].oldc = types; rows[4].oldc = vals;
    int mism = 0;
    for (auto &r : rows) if (r.oldc != r.domc) { mism++; }
    // edit spot-test: first <Value> element child whose text is 2 hex chars
    bool edited = false;
    QByteArray raw2;
    for (int i = 0; i < d.pool.size() && !edited; ++i) {
        if (!d.pool[i].is_text)
            for (int c : d.pool[i].children)
                if (d.pool[c].is_text && d.pool[c].text.size() == 2
                    && d.pool[c].text[0] != '0') {
                    DomDoc d2 = d;   // value copy = deep (QVector value semantics)
                    d2.pool[c].text[0] = d.pool[c].text[0] + 1;
                    raw2 = dom_serialize(d2);
                    edited = true;
                    break;
                }
    }
    const bool edit_ok = edited && dom_parse(raw2, d);
    // NOTE: reused d on parse — only for "still parses"; the diff check below:
    int diffcount = -1;
    if (edited) {
        std::printf("edit sizes raw=%lld raw2=%lld\n", (long long)raw.size(), (long long)raw2.size());
        if (raw2.size() == raw.size()) {
            int dd = 0;
            for (int k = 0; k < raw.size(); ++k)
                if (raw2[k] != raw[k]) ++dd;
            diffcount = dd;
        }
    }
    // Phase 1 structural test: add layer to map0, drop LAST map, add NEW map 999
    int struct_ok = -1;
    {
        DomDoc dd;
        if (dom_parse(raw, dd)) {
            Pmap mod = dom_view(dd);
            if (!mod.maps.isEmpty()) {
                Layer la; la.type = QStringLiteral("TEXT"); la.value = QStringLiteral("P1ADD");
                mod.maps[0].layers.append(la);
            }
            if (mod.maps.size() >= 2)
                mod.maps.removeLast();
            {
                Layer nl; nl.type = QStringLiteral("TEXT"); nl.value = QStringLiteral("P1NEW");
                Map nm; nm.poskey = 999; nm.layers.append(nl);
                mod.maps.append(nm);
            }
            QString serr;
            const QByteArray s3 = dom_apply_model(dd, mod, serr);
            DomDoc d3;
            if (!serr.isEmpty() || !dom_parse(s3, d3) || dom_serialize(d3) != s3)
                struct_ok = serr.isEmpty() ? 0 : -1;
            else {
                const Pmap back = dom_view(d3);
                bool eq = back.maps.size() == mod.maps.size();
                for (int i = 0; i < back.maps.size() && eq; ++i) {
                    eq &= back.maps[i].poskey == mod.maps[i].poskey;
                    eq &= back.maps[i].layers.size() == mod.maps[i].layers.size();
                    for (int j = 0; eq && j < back.maps[i].layers.size(); ++j) {
                        eq &= back.maps[i].layers[j].value == mod.maps[i].layers[j].value;
                        eq &= back.maps[i].layers[j].type == mod.maps[i].layers[j].type;
                    }
                }
                struct_ok = eq ? 1 : 0;
            }
        }
    }
    std::printf("%-64s %7lld B  RT=%s IDOM=%s %s=%d/%d %s=%d/%d %s=%d/%d  counts_ok=%d struct=%d editdiff=%d\n",
        path.toUtf8().constData(), (long long)raw.size(), same ? "EXACT" : "LOSSY",
        idem ? "OK" : "NO",
        "MAP", rows[0].oldc, rows[0].domc, "LAYER", rows[2].oldc, rows[2].domc,
        "Value", rows[4].oldc, rows[4].domc, mism, struct_ok, diffcount);
    return (same && idem && !mism && struct_ok == 1) ? 0 : 3;
}

int main(int argc, char **argv)
{
    // file list comes from shell glob (Qt6 QFile::glob is instance-style; keep it simple)
    QStringList files;
    for (int i = 1; i < argc; ++i) files << argv[i];
    if (files.isEmpty()) { std::printf("usage: pmaprt <pmap files...> (or via runs/pmaprt.sh)\n"); return 2; }
    int fails = 0, skipped = 0;
    for (const auto &p : files) {
        if (!QFile::exists(p)) { ++skipped; std::printf("SKIP (absent): %s\n", p.toUtf8().constData()); continue; }
        const int rc = run_file(p);
        if (rc != 0) ++fails;
    }
    // negative tests
    const QByteArray neg1 = "<A B=\"1\">x</A>";
    const QByteArray neg2 = "<A><B>1</C></A>";
    DomDoc nd;
    const bool n1 = dom_parse(neg1, nd), n2 = dom_parse(neg2, nd);
    std::printf("negative: attrs=%s mismatch=%s (both must be 0)\n", n1 ? "PARSED" : "REJECTED", n2 ? "PARSED" : "REJECTED");
    if (n1 || n2) ++fails;
    std::printf("RESULT: %d files, fails=%d\n", (int)files.size(), fails);
    return fails ? 1 : 0;
}
