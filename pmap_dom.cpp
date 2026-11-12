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

#include <algorithm>

// --- Phase 1: structural DOM edit helpers ------------------------------
static int dom_clone(DomDoc &d, int src)
{
    const DomSlot copy = d.pool[src];   // value copy: realloc-safe
    DomSlot ns;
    ns.name = copy.name;
    ns.selfclose = copy.selfclose;
    for (int c : copy.children) {
        const DomSlot cs = d.pool[c];
        if (cs.is_text) {
            const int ti = d.pool.size();
            d.pool.append(DomSlot{});
            d.pool[ti].is_text = true;
            d.pool[ti].text = cs.text;
            ns.children.append(ti);
        } else {
            ns.children.append(dom_clone(d, c));
        }
    }
    const int idx = d.pool.size();
    d.pool.append(ns);
    return idx;
}

static int dom_new_elem(DomDoc &d, const QString &name, const QString &hexText)
{
    QVector<int> kids;
    if (!hexText.isNull()) {
        const int ti = d.pool.size();
        d.pool.append(DomSlot{});
        d.pool[ti].is_text = true;
        d.pool[ti].text = hexText.toUtf8();
        kids.append(ti);
    }
    const int idx = d.pool.size();
    d.pool.append(DomSlot{});
    d.pool[idx].name = name;
    d.pool[idx].children = kids;
    return idx;
}

static int dom_new_layer(DomDoc &d)
{
    const int te = dom_new_elem(d, QStringLiteral("Type"), QString());
    const int ve = dom_new_elem(d, QStringLiteral("Value"), QString());
    const int le = d.pool.size();
    d.pool.append(DomSlot{});
    d.pool[le].name = QStringLiteral("LAYER");
    d.pool[le].children = {te, ve};
    return le;
}

static int dom_new_map(DomDoc &d, int poskey)
{
    const int pk = dom_new_elem(d, QStringLiteral("POSKey"),
                                to_hex_cp1252(QString::number(poskey)));
    const int me = d.pool.size();
    d.pool.append(DomSlot{});
    d.pool[me].name = QStringLiteral("MAP");
    d.pool[me].children = {pk};
    return me;
}

static void dom_erase_child(DomDoc &d, int parent, int child)
{
    QVector<int> &kids = d.pool[parent].children;
    kids.erase(std::find(kids.begin(), kids.end(), child));
}

static int dom_poskey(const DomDoc &d, int mapIdx)
{
    const int pk = dom_find_child(d, mapIdx, QStringLiteral("POSKey"));
    return pk >= 0 ? unhex_cp1252(dom_text(d, pk)).toInt() : -1;
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
    if (s.selfclose && s.children.isEmpty()) { out += '<' + s.name.toUtf8() + " />"; return; }
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

// model view of the DOM, decoded exactly like parse_pmap does
Pmap dom_view(const DomDoc &d)
{
    Pmap p;
    int root = -1;
    for (int r : d.roots)
        if (!d.pool[r].is_text) { root = r; break; }
    if (root < 0 || d.pool[root].name != QStringLiteral("POSKeyMapper"))
        return p;
    if (const int layout = dom_find_child(d, root, QStringLiteral("LAYOUT")); layout >= 0)
        if (const int ne = dom_find_child(d, layout, QStringLiteral("Name")); ne >= 0)
            p.name = unhex_cp1252(dom_text(d, ne)).trimmed();
    const int mapping = dom_find_child(d, root, QStringLiteral("MAPPING"));
    if (mapping < 0) return p;
    for (int me : dom_find_all(d, mapping, QStringLiteral("MAP"))) {
        Map m;
        if (const int pk = dom_find_child(d, me, QStringLiteral("POSKey")); pk >= 0)
            m.poskey = unhex_cp1252(dom_text(d, pk)).toInt();
        for (int le : dom_find_all(d, me, QStringLiteral("LAYER"))) {
            Layer la;
            if (const int te = dom_find_child(d, le, QStringLiteral("Type")); te >= 0)
                la.type = unhex_cp1252(dom_text(d, te)).trimmed();
            if (const int ve = dom_find_child(d, le, QStringLiteral("Value")); ve >= 0)
                la.value = unhex_cp1252(dom_text(d, ve));
            if (const int de = dom_find_child(d, le, QStringLiteral("Dependency")); de >= 0) {
                const QString dt = dom_text(d, de);
                if (!dt.trimmed().isEmpty())     // vendor empty = raw whitespace, not hex
                    la.dependency = unhex_cp1252(dt);
            }
            m.layers.push_back(la);
        }
        p.maps.push_back(m);
    }
    return p;
}

// hex of val in the letter-case of the original text (first hex letter wins)
static QString format_hex_like(const QString &orig_text, const QString &hex)
{
    for (const QChar &c : orig_text) {
        if (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
            return hex.toLower();
        if (c >= QLatin1Char('A') && c <= QLatin1Char('F'))
            return hex;   // our hex is produced uppercase; keep
    }
    return hex;
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
    const Pmap view = dom_view(d);   // diff base: current file content
    // Name — only if it actually differs, keeping the original hex case
    if (const int layout = dom_find_child(d, root, QStringLiteral("LAYOUT")); layout >= 0)
        if (const int ne = dom_find_child(d, layout, QStringLiteral("Name")); ne >= 0)
            if (pm.name != view.name)
                dom_set_text(d, ne,
                             format_hex_like(dom_text(d, ne), to_hex_cp1252(pm.name))
                             .toUtf8());
    const int mapping = dom_find_child(d, root, QStringLiteral("MAPPING"));
    if (mapping < 0) { err = QStringLiteral("SAVE BLOCKED: no MAPPING block"); return {}; }
    // 1) remove DOM MAPs the model dropped (poskey identity, order-safe loop)
    for (int me : dom_find_all(d, mapping, QStringLiteral("MAP"))) {
        bool keep = false;
        const int p = dom_poskey(d, me);
        for (const Map &m : pm.maps)
            if (m.poskey == p) { keep = true; break; }
        if (!keep && p >= 0)
            dom_erase_child(d, mapping, me);
    }
    // 2) per model map: reuse, clone (style donor = first DOM MAP) or synth
    for (const Map &m : pm.maps) {
        int me = -1;
        bool fresh = false;
        for (int cand : dom_find_all(d, mapping, QStringLiteral("MAP")))
            if (dom_poskey(d, cand) == m.poskey) { me = cand; break; }
        if (me < 0) {
            fresh = true;
            const QVector<int> donors = dom_find_all(d, mapping, QStringLiteral("MAP"));
            if (!donors.isEmpty()) {
                me = dom_clone(d, donors.first());
                if (const int pk = dom_find_child(d, me, QStringLiteral("POSKey")); pk >= 0)
                    dom_set_text(d, pk, to_hex_cp1252(QString::number(m.poskey))
                                 .toUtf8());   // digits: no case issue
                else { dom_erase_child(d, mapping, me); me = dom_new_map(d, m.poskey); }
                d.pool[mapping].children.append(me);
            } else {
                me = dom_new_map(d, m.poskey);
                d.pool[mapping].children.append(me);
            }
        }
        // POSKey text style untouched on reuse; layers:
        const int vmi = [&]() {           // view map with same poskey, if any
            for (int i = 0; i < view.maps.size(); ++i)
                if (view.maps[i].poskey == m.poskey) return i;
            return -1; }();
        const QVector<Layer> vlay = vmi >= 0 ? view.maps[vmi].layers
                                             : QVector<Layer>{};
        QVector<int> domLayers = dom_find_all(d, me, QStringLiteral("LAYER"));
        for (int j = 0; j < domLayers.size() && j < m.layers.size(); ++j) {
            const Layer &src = m.layers[j];
            const Layer &old = j < vlay.size() ? vlay[j] : Layer{};
            if (fresh || old.type != src.type)
                if (const int te = dom_find_child(d, domLayers[j], QStringLiteral("Type")); te >= 0)
                    if (!dom_set_text(d, te,
                        format_hex_like(dom_text(d, te), to_hex_cp1252(src.type)).toUtf8()))
                        { err = QStringLiteral("SAVE BLOCKED: ambiguous Type text"); return {}; }
            if (fresh || old.value != src.value)
                if (const int ve = dom_find_child(d, domLayers[j], QStringLiteral("Value")); ve >= 0)
                    if (!dom_set_text(d, ve,
                        format_hex_like(dom_text(d, ve), to_hex_cp1252(src.value)).toUtf8()))
                        { err = QStringLiteral("SAVE BLOCKED: ambiguous Value text"); return {}; }
            if (fresh || old.dependency != src.dependency) {
                if (const int ce = dom_find_child(d, domLayers[j], QStringLiteral("Dependency")); ce >= 0) {
                    if (!dom_set_text(d, ce,
                        format_hex_like(dom_text(d, ce), to_hex_cp1252(src.dependency)).toUtf8()))
                        { err = QStringLiteral("SAVE BLOCKED: ambiguous Dependency text"); return {}; }
                } else if (!src.dependency.isEmpty()) {
                    const int de = dom_new_elem(d, QStringLiteral("Dependency"),
                                                to_hex_cp1252(src.dependency));
                    d.pool[domLayers[j]].children.append(de);
                }
            }
        }
        for (int j = m.layers.size(); j < domLayers.size(); ++j)
            dom_erase_child(d, me, domLayers[j]);           // drop extra layers
        const QVector<int> styleDonors = domLayers;
        for (int j = domLayers.size(); j < m.layers.size(); ++j) {
            const int le = !styleDonors.isEmpty() ? dom_clone(d, styleDonors.first())
                                                  : dom_new_layer(d);
            if (const int te = dom_find_child(d, le, QStringLiteral("Type")); te >= 0)
                dom_set_text(d, te, to_hex_cp1252(m.layers[j].type).toUtf8());
            if (const int ve = dom_find_child(d, le, QStringLiteral("Value")); ve >= 0)
                dom_set_text(d, ve, to_hex_cp1252(m.layers[j].value).toUtf8());
            if (const int ce = dom_find_child(d, le, QStringLiteral("Dependency")); ce >= 0)
                dom_set_text(d, ce, to_hex_cp1252(m.layers[j].dependency).toUtf8());
            else if (!m.layers[j].dependency.isEmpty()) {
                const int de = dom_new_elem(d, QStringLiteral("Dependency"),
                                            to_hex_cp1252(m.layers[j].dependency));
                d.pool[le].children.append(de);
            }
            d.pool[me].children.append(le);
        }
    }
    return dom_serialize(d);
}

QByteArray dom_apply_model(DomDoc &d, const Pmap &pm, QString &err)
{
    return dom_apply_model_impl(d, pm, err);
}
