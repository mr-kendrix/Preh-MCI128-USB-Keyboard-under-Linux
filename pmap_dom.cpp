// Lossless pmap DOM — see pmap_dom.h. Phase 0 round-trip core.
// Design: single slot pool, children reference parents-by-index; children
// commit BEFORE their parent so indices stay stable (no reserve/index skew).
#include "pmap_dom.h"
#include <QString>

static bool parse_element(const QByteArray &raw, int &i, DomDoc &out,
                          int &slot_out, int depth);
static int append_text(DomDoc &out, const QByteArray &t);

static int append_text(DomDoc &out, const QByteArray &t)
{
    const int idx = out.pool.size();
    out.pool.append(DomSlot{});
    DomSlot &s = out.pool[idx];
    s.is_text = true;
    s.text = t;
    return idx;
}

static bool parse_element(const QByteArray &raw, int &i, DomDoc &out,
                          int &slot_out, int depth)
{
    if (depth > 64 || i + 1 >= raw.size() || raw[i] != '<')
        return false;
    const int n = raw.size();
    ++i;
    const int ns = i;
    while (i < n && raw[i] != '>') ++i;
    if (i >= n) return false;
    const QByteArray nameb = raw.mid(ns, i - ns);
    if (nameb.isEmpty() || nameb.contains('='))
        return false;                     // attributes unsupported
    bool selfclose = false;
    QByteArray nm = nameb;
    if (nm.endsWith(QLatin1String(" /"))) { selfclose = true; nm = nm.left(nm.size() - 2); }
    else if (nm.endsWith(QLatin1String("/"))) { selfclose = true; nm = nm.left(nm.size() - 1); }
    if (nm.isEmpty()) return false;
    ++i;
    const QString name = QString::fromLatin1(nm);
    QVector<int> kids;
    if (selfclose) {
        const int idx = out.pool.size();
        out.pool.append(DomSlot{});
        DomSlot &s = out.pool[idx];
        s.is_text = false;
        s.selfclose = true;
        s.name = name;
        slot_out = idx;
        return true;
    }
    while (true) {
        if (i < n && raw[i] != '<') {
            const int start = i;
            while (i < n && raw[i] != '<') ++i;
            kids.push_back(append_text(out, raw.mid(start, i - start)));
            continue;
        }
        if (i >= n) return false;         // unterminated
        if (raw.mid(i, 2) == QByteArray("</")) {
            const int ts = i + 2;
            int te = ts;
            while (te < n && raw[te] != '>') ++te;
            if (te >= n) return false;
            if (QString::fromLatin1(raw.mid(ts, te - ts)) != name)
                return false;             // mismatched close
            i = te + 1;
            break;
        }
        int child = -1;
        if (!parse_element(raw, i, out, child, depth + 1))
            return false;
        kids.push_back(child);
    }
    // commit AFTER children: parent index > all child indices
    const int idx = out.pool.size();
    out.pool.append(DomSlot{});
    DomSlot &s = out.pool[idx];
    s.is_text = false;
    s.name = name;
    s.children = kids;
    slot_out = idx;
    return true;
}

bool dom_parse(const QByteArray &raw, DomDoc &out)
{
    out = DomDoc{};
    int i = 0;
    const int n = raw.size();
    while (i < n) {
        if (raw[i] == '<') {
            int node = -1;
            if (!parse_element(raw, i, out, node, 0))
                return false;
            out.roots.append(node);
        } else {
            const int start = i;
            while (i < n && raw[i] != '<') ++i;
            out.roots.append(append_text(out, raw.mid(start, i - start)));
        }
    }
    return true;
}

int dom_find_child(const DomDoc &d, int slot, const QString &tag)
{
    if (slot < 0 || slot >= d.pool.size() || d.pool[slot].is_text)
        return -1;
    for (int c : d.pool[slot].children)
        if (!d.pool[c].is_text && d.pool[c].name == tag)
            return c;
    return -1;
}

QVector<int> dom_find_all(const DomDoc &d, int slot, const QString &tag)
{
    QVector<int> r;
    if (slot < 0 || slot >= d.pool.size() || d.pool[slot].is_text)
        return r;
    for (int c : d.pool[slot].children)
        if (!d.pool[c].is_text && d.pool[c].name == tag)
            r.push_back(c);
    return r;
}

QByteArray dom_text(const DomDoc &d, int slot)
{
    QByteArray r;
    if (slot < 0 || slot >= d.pool.size())
        return r;
    for (int c : d.pool[slot].children)
        if (d.pool[c].is_text)
            r += d.pool[c].text;
    return r;
}

static void ser_node(const DomDoc &d, int slot, QByteArray &out)
{
    const DomSlot &s = d.pool[slot];
    if (s.is_text) { out += s.text; return; }
    if (s.selfclose) { out += '<' + s.name.toUtf8() + " />"; return; }
    out += '<' + s.name.toUtf8() + '>';
    for (int c : s.children)
        ser_node(d, c, out);
    out += "</" + s.name.toUtf8() + '>';
}

QByteArray dom_serialize(const DomDoc &d)
{
    QByteArray out;
    for (int r : d.roots)
        ser_node(d, r, out);
    return out;
}

bool dom_set_text(DomDoc &d, int slot, const QByteArray &t)
{
    if (slot < 0 || slot >= d.pool.size() || d.pool[slot].is_text)
        return false;
    QVector<int> texts;
    for (int c : d.pool[slot].children)
        if (d.pool[c].is_text)
            texts.push_back(c);
    if (texts.size() == 1) { d.pool[texts[0]].text = t; return true; }
    if (texts.isEmpty()) {
        // append a fresh text child at the end
        d.pool[slot].children.push_back(append_text(d, t));
        return true;
    }
    return false;   // ambiguous multi-text: refuse, keep file untouched
}

static QByteArray dom_apply_model_impl(DomDoc &d, const Pmap &pm, QString &err)
{
    int root = -1;
    for (int r : d.roots)
        if (!d.pool[r].is_text) { root = r; break; }
    if (root < 0 || d.pool[root].name != QStringLiteral("POSKeyMapper")) {
        err = QStringLiteral("SAVE BLOCKED: root is not POSKeyMapper");
        return {};
    }
    // Name (LAYOUT block)
    if (const int layout = dom_find_child(d, root, QStringLiteral("LAYOUT")); layout >= 0)
        if (const int nameEl = dom_find_child(d, layout, QStringLiteral("Name")); nameEl >= 0)
            dom_set_text(d, nameEl, to_hex_cp1252(pm.name).toUtf8());
    const int mapping = dom_find_child(d, root, QStringLiteral("MAPPING"));
    const QVector<int> domMaps = mapping >= 0 ? dom_find_all(d, mapping, QStringLiteral("MAP")) : QVector<int>{};
    if (domMaps.size() != pm.maps.size()) {
        err = QStringLiteral("SAVE BLOCKED: structural edit (maps %1->%2) not yet supported — Phase 1"
        ).arg(domMaps.size()).arg(pm.maps.size());
        return {};
    }
    for (int i = 0; i < domMaps.size(); ++i) {
        const int me = domMaps[i];
        const int pk = dom_find_child(d, me, QStringLiteral("POSKey"));
        if (pk < 0) { err = QStringLiteral("SAVE BLOCKED: MAP without POSKey"); return {}; }
        const int dompk = unhex_cp1252(dom_text(d, pk)).toInt();
        if (dompk != pm.maps[i].poskey) {
            err = QStringLiteral("SAVE BLOCKED: DOM/model POSKey mismatch at map %1 (%2 vs %3)"
            ).arg(i).arg(dompk).arg(pm.maps[i].poskey);
            return {};
        }
        const QVector<int> domLayers = dom_find_all(d, me, QStringLiteral("LAYER"));
        if (domLayers.size() != pm.maps[i].layers.size()) {
            err = QStringLiteral("SAVE BLOCKED: structural edit on POSKey %1 (layers %2->%3) not yet supported"
            ).arg(pm.maps[i].poskey).arg(domLayers.size()).arg(pm.maps[i].layers.size());
            return {};
        }
        for (int j = 0; j < domLayers.size(); ++j) {
            const int te = dom_find_child(d, domLayers[j], QStringLiteral("Type"));
            const int ve = dom_find_child(d, domLayers[j], QStringLiteral("Value"));
            if (te >= 0 && !dom_set_text(d, te, to_hex_cp1252(pm.maps[i].layers[j].type).toUtf8()))
                { err = QStringLiteral("SAVE BLOCKED: ambiguous Type text"); return {}; }
            if (ve >= 0 && !dom_set_text(d, ve, to_hex_cp1252(pm.maps[i].layers[j].value).toUtf8()))
                { err = QStringLiteral("SAVE BLOCKED: ambiguous Value text"); return {}; }
        }
    }
    return dom_serialize(d);
}

QByteArray dom_apply_model(DomDoc &d, const Pmap &pm, QString &err)
{
    return dom_apply_model_impl(d, pm, err);
}
