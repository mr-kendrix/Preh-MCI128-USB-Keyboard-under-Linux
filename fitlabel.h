#ifndef PKT_FITLABEL_H
#define PKT_FITLABEL_H
#include <QHash>
#include <QString>
#include <QVector>
#include <QRegularExpression>
#include <utility>
#include <algorithm>
// ------- Helvetica width table + label fit: SAME table for fit AND draw -------
struct Fit { double pt = 0; QVector<QString> lines; bool truncated = false;
             double wmax = 0, hmax = 0; };
static const QHash<int, int> &helv_w()
{
    static const QHash<int, int> t = {
        {32,278},{33,278},{34,355},{35,556},{36,556},{37,889},{38,667},{39,191},
        {40,333},{41,333},{42,389},{43,584},{44,278},{45,333},{46,278},{47,278},
        {48,556},{49,556},{50,556},{51,556},{52,556},{53,556},{53,556},{54,556},{55,556},{56,556},{57,556},
        {58,278},{59,278},{60,584},{61,584},{62,584},{63,556},{64,1015},
        {65,667},{66,667},{67,722},{68,722},{69,667},{70,611},{71,778},{72,722},
        {73,278},{74,500},{75,667},{76,278},{77,833},{78,722},{79,778},{80,667},
        {81,778},{82,722},{83,667},{84,611},{85,667},{86,667},{87,944},{88,667},
        {89,667},{90,611},
        {91,278},{92,278},{93,278},{94,469},{95,556},{96,333},
        {97,556},{98,556},{99,556},{100,556},{101,556},{102,278},{103,556},{104,556},
        {105,278},{106,278},{107,556},{108,278},{109,833},{110,556},{111,556},{112,556},
        {113,556},{114,333},{115,500},{116,278},{117,556},{118,556},{119,722},{120,556},
        {121,556},{122,500},{123,334},{124,260},{125,334},{126,584},
        {0xA0,278},{0xC4,778},{0xD6,778},{0xDC,778},{0xDF,556},{0xE4,556},{0xF6,556},{0xFC,556},
        {0x80,556},{0x85,1000},{0x91,222},{0x92,222},{0x93,333},{0x94,333},{0x96,556},{0x97,1000}};
    return t;
}
static double text_w(const QString &s, double fpt)
{
    const QHash<int, int> &t = helv_w();
    double w = 0;
    for (const QChar c : s) w += t.value(c.unicode(), 556);
    return w / 1000.0 * fpt;
}
static Fit fit_label(const QString &text, double kw, double kh, double start_pt)
{
    // keep the caption's own line breaks; shrink from start_pt in 0.25 steps (never grow);
    // wrap at spaces when still too wide; floor 5 pt -> ellipsis truncate
    const double pad = 2.0, avail_w = qMax(4.0, kw - 2 * pad), avail_h = qMax(4.0, kh - 2 * pad);
    QVector<QString> base;
    for (const QString &ln : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+"))))
        if (!ln.trimmed().isEmpty()) base << ln.trimmed();
    if (base.isEmpty()) return {};
    auto wrap = [&](double fpt) {
        QVector<QString> out;
        for (const QString &ln : base) {
            QString cur;
            for (const QString &wd : ln.split(QLatin1Char(' '))) {
                const QString cand = cur.isEmpty() ? wd : cur + QLatin1Char(' ') + wd;
                if (text_w(cand, fpt) <= avail_w || cur.isEmpty()) cur = cand;
                else { if (!cur.isEmpty()) out << cur; cur = wd; }
            }
            out << cur;
        }
        double wmax = 0;
        for (const QString &l : out) wmax = qMax(wmax, text_w(l, fpt));
        return std::pair{out, wmax};
    };
    for (double fpt = start_pt; fpt >= 5.0; fpt -= 0.25) {
        auto [out, wmax] = wrap(fpt);
        const double hmax = out.size() * fpt * 1.2;
        if (wmax <= avail_w && hmax <= avail_h) return {fpt, out, false, wmax, hmax};
    }
    const double fpt = 5.0;
    auto [out, wmax0] = wrap(fpt);
    bool trunc = false;
    for (QString &l : out)
        while (text_w(l, fpt) > avail_w && l.size() > 1) {
            if (!l.endsWith(QChar(0x85))) l += QChar(0x85);
            const bool e = l.endsWith(QChar(0x85));
            l = l.left(qMax(1, l.size() - (e ? 2 : 1)));
            trunc = true;
        }
    while (out.size() * fpt * 1.2 > avail_h && out.size() > 1) { out.removeLast(); trunc = true; }
    double wmax = 0;
    for (const QString &l : out) wmax = qMax(wmax, text_w(l, fpt));
    return {fpt, out, trunc, wmax, out.size() * fpt * 1.2};
}

#endif
