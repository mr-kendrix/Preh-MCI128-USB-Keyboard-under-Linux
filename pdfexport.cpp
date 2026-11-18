#include "fitlabel.h"
#include "pmap.h"
#include <QHash>
#include <QString>
#include <QVector>
#include <QByteArray>
#include <utility>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cmath>
namespace pdfx {
// Vendor-style label sheet: cut-out cells sorted by key size, tiled on A4 pages
QByteArray make_cut_sheet(const QHash<int, QString> &texts,
                                 const QHash<int, QString> &fonts)
{
    if (g_geom.isEmpty()) return {};
    QVector<int> keys;
    for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) keys << it.key();
    std::sort(keys.begin(), keys.end(), [](int a, int b) {   // sorted by key size
        const KeyGeom &ka = g_geom.value(a), &kb = g_geom.value(b);
        return ka.w * ka.h != kb.w * kb.h ? ka.w * ka.h > kb.w * kb.h
                                          : ka.w > kb.w;
    });
    auto n1 = [](double v) { return QString::number(v, 'f', 1); };
    auto esc = [](const QString &s) {
        QString r;
        for (const QChar c : s) { if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('\\')) r += QLatin1Char('\\'); r += c; }
        return r;
    };
    const double pw = 595.0, ph = 842.0, m = 20.0, gap = 3.0;
    QVector<QByteArray> pagesStreams;
    QString body;
    double cx = m, cy = m, rowH = 0;
    int cells = 0;
    auto flushPage = [&]() { pagesStreams << body.toLatin1(); body.clear(); cx = m; cy = m; rowH = 0; };
    for (int pk : keys) {
        const KeyGeom &k = g_geom.value(pk);
        const double kw = qMax(24.0, k.w * 0.0022), kh = qMax(12.0, k.h * 0.0022); // board units (17000 = ~17 mm) -> pt
        QString lbl = k.text;
        if (lbl.isEmpty()) lbl = texts.value(pk);
        if (lbl.isEmpty()) lbl = QStringLiteral("%1").arg(pk);
        if (cx + kw > pw - m) { cx = m; cy += rowH + gap; rowH = 0; }
        if (cy + kh > ph - m) flushPage();
        const double cellw = kw + 6, cellh = kh + 6;   // cut margin
        body += QStringLiteral("0.4 w %1 %2 %3 %4 re S\n")
                    .arg(n1(cx)).arg(n1(cy)).arg(n1(cellw)).arg(n1(cellh));
        const Fit f = fit_label(lbl, cellw, cellh, 14.0);
        if (!f.lines.isEmpty()) {
            body += QStringLiteral("0 0 0 rg\n");
            const double ccx = cx + cellw / 2.0;
            double ty = cy + (cellh - f.hmax) / 2.0 + f.pt * 0.15;
            for (const QString &line : f.lines) {
                const double w = text_w(line, f.pt);
                body += QStringLiteral("BT /F0 %1 Tf %2 %3 Td(")
                            .arg(n1(f.pt)).arg(n1(ccx - w / 2.0)).arg(n1(ty))
                            + esc(line) + QStringLiteral(") Tj ET\n");
                ty -= f.pt * 1.2;
            }
        }
        cx += cellw + gap; rowH = qMax(rowH, cellh);
        ++cells;
    }
    flushPage();
    const int P = pagesStreams.size();
    if (!P) return {};
    // objects: 1 catalog, 2 pages, 3 font, page objs 4..3+P, streams 4+P..3+2P
    const int nObj = 3 + 2 * P;
    QByteArray pdf("%PDF-1.4\n");
    QVector<int> off(nObj + 1, 0);
    auto put = [&](int n, const QString &st) {
        off[n] = pdf.size();
        pdf += QStringLiteral("%1 0 obj\n").arg(n).toUtf8();
        pdf += st.toUtf8();
        pdf += QByteArrayLiteral("\nendobj\n");
    };
    put(1, QStringLiteral("<< /Pages 2 0 R >>"));
    QString kids;
    for (int i = 0; i < P; ++i) kids += QStringLiteral("%1 0 R ").arg(4 + i);
    put(2, QStringLiteral("<< /Type /Pages /Kids [") + kids + QStringLiteral("] /Count ") + QString::number(P) + QStringLiteral(" >>"));
    put(3, QStringLiteral("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"));
    for (int i = 0; i < P; ++i)
        put(4 + i, QStringLiteral("<< /Type /Page /Parent 2 0 R /Resources << /Font << /F0 3 0 R >> >> /MediaBox [0 0 595 842] /Contents ")
            + QString::number(4 + P + i) + QStringLiteral(" 0 R >>"));
    for (int i = 0; i < P; ++i) {
        const int obj = 4 + P + i;
        const QByteArray c = pagesStreams[i];
        off[obj] = pdf.size();
        pdf += QStringLiteral("%1 0 obj\n").arg(obj).toUtf8();
        pdf += QStringLiteral("<< /Length %1 >>\nstream\n").arg(c.size()).toUtf8();
        pdf += c;
        pdf += QByteArrayLiteral("\nendstream\nendobj\n");
    }
    QString xref = QStringLiteral("xref\n0 %1\n0000000000 65535 f \n").arg(nObj + 1);
    for (int n = 1; n <= nObj; ++n) xref += QStringLiteral("%1 00000 n \n").arg(off[n], 10, 10, QLatin1Char('0'));
    const int xpos = pdf.size();
    pdf += xref.toUtf8();
    pdf += (QStringLiteral("trailer\n<< /Root 1 0 R /Size ") + QString::number(nObj + 1) +
            QStringLiteral(" >>\nstartxref\n") + QString::number(xpos) + QStringLiteral("\n%%EOF\n")).toUtf8();
    return pdf;
}

QByteArray make_label_pdf_geom(const QVector<std::pair<int, QString>> &rows,
                                      const QVector<std::pair<int, QString>> &rows_types,
                                      const QHash<int, QString> &print_texts,
                                      const QHash<int, QString> &font_descs)
{
    // one A4 page: rect per POSKey at the pmap's own <Key> geometry + label inside
    const QChar L('<'), G('>');
    auto dict = [&](const QString &inner){ QString s; s += L; s += L; s += inner; s += G; s += G; return s; };
    auto esc = [](const QString &s) {
        QString r;
        for (const QChar c : s) { if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('\\')) r += QLatin1Char('\\'); r += c; }
        return r;
    };
    int maxx = 1, maxy = 1;
    for (auto it = g_geom.constBegin(); it != g_geom.constEnd(); ++it) {
        const KeyGeom &k = it.value();
        maxx = qMax(maxx, k.x + k.w); maxy = qMax(maxy, k.y + k.h);
    }
    const double s = qMin((595.0 - 40) / maxx, (842.0 - 40) / maxy);
    QString body;
    body += QStringLiteral("0.25 w\n");
    QHash<int, QString> hasv;
    QHash<int, QString> hast;
    for (const auto &r : rows) { hasv.insert(r.first, r.second); }
    for (const auto &r : rows_types) { hast.insert(r.first, r.second); }
    for (auto it = g_geom.constBegin(); it != g_geom.constEnd(); ++it) {
        const KeyGeom &k = it.value();
        const double x = 20 + k.x * s, y = 842 - 20 - (k.y + k.h) * s;   // flip Y
        const bool has = hasv.contains(it.key()) && hasv.value(it.key()) != QLatin1String("-");
        const QString ty = hast.value(it.key());
        const bool web = (ty == QStringLiteral("WEBSITE") || ty == QStringLiteral("APPL"));
        const QString col = !has ? QStringLiteral("0.90 0.91 0.92")                                   // light
                          : web ? QStringLiteral("0.71 0.74 0.78")                                   // blue-grey
                                : QStringLiteral("0.60 0.78 0.62");                                  // green
        body += col + QStringLiteral(" rg\n")
              + QStringLiteral("%1 %2 %3 %4 re f S\n")
                    .arg(QString::number(x, 'f', 1)).arg(QString::number(y, 'f', 1))
                    .arg(QString::number(k.w * s, 'f', 1)).arg(QString::number(k.h * s, 'f', 1));
    }
    auto n1 = [](double v) { return QString::number(v, 'f', 1); };
    for (auto it = g_geom.cbegin(); it != g_geom.cend(); ++it) {
        const KeyGeom &k = it.value();
        QString lbl = k.text;
        if (lbl.isEmpty() && print_texts.contains(it.key()))
            lbl = print_texts.value(it.key());
        if (lbl.isEmpty()) {
            const auto rv = hasv.find(it.key());
            if (rv != hasv.end() && rv.value() != QLatin1String("-"))
                lbl = QStringLiteral("%1: %2").arg(it.key()).arg(rv.value());
        }
        if (lbl.isEmpty()) continue;
        const double kw = k.w * s, kh = k.h * s;
        double start = 12.0;
        if (font_descs.contains(it.key())) {
            QRegularExpression nRe(QStringLiteral("[^0-9]*([0-9][0-9.]*)pt"));
            if (auto nm = nRe.match(font_descs.value(it.key())); nm.isValid())
                start = nm.captured(1).toDouble() / 100.0 * s;
        } else if (k.fsize > 0) start = k.fsize * s;
        const Fit f = fit_label(lbl, kw, kh, qBound(5.0, start, 40.0));
        if (f.lines.isEmpty()) continue;
        const double x0 = 20 + k.x * s, y0 = 842 - 20 - (k.y + k.h) * s;
        int c = k.fore;
        if (c < 0) c = 0;
        body += QStringLiteral("%1 %2 %3 rg\n")
                    .arg(n1(((c >> 16) & 0xFF) / 255.0)).arg(n1(((c >> 8) & 0xFF) / 255.0)).arg(n1((c & 0xFF) / 255.0));
        const double cx = x0 + kw / 2.0;
        double ty = y0 + (kh - f.hmax) / 2.0 + f.pt * 0.8;
        for (const QString &line : f.lines) {
            const double w = text_w(line, f.pt);
            body += QStringLiteral("BT /F0 %1 Tf %2 %3 Td(")
                        .arg(n1(f.pt)).arg(n1(cx - w / 2.0)).arg(n1(ty))
                        + esc(line) + QStringLiteral(") Tj ET\n");
            ty -= f.pt * 1.2;
        }
    }
    const QByteArray c = body.toLatin1();
    QByteArray pdf("%PDF-1.4\n");
    QVector<int> off(6, 0);
    auto put = [&](int n, const QString &st) { off[n] = pdf.size(); pdf += QStringLiteral("%1 0 obj\n").arg(n).toUtf8(); pdf += st.toUtf8(); pdf += QByteArrayLiteral("\nendobj\n"); };
    put(1, dict(QStringLiteral("/Pages 2 0 R ")));
    put(2, dict(QStringLiteral("/Type /Pages /Kids [3 0 R] /Count 1 ")));
    put(3, dict(QStringLiteral("/Type /Page /Parent 2 0 R /Resources ") + dict(QStringLiteral("/Font ") + dict(QStringLiteral("/F0 5 0 R ")) + QStringLiteral(" ")) + QStringLiteral(" /MediaBox [0 0 595 842] /Contents 4 0 R ")));
    {   // content stream RAW (latin1 bytes, never toUtf8)
        off[4] = pdf.size();
        pdf += QByteArrayLiteral("4 0 obj\n");
        pdf += (dict(QStringLiteral("/Length ") + QString::number(c.size()) + QStringLiteral(" "))
                + QStringLiteral("stream\n")).toLatin1();
        pdf += c;
        pdf += QByteArrayLiteral("\nendstream\nendobj\n");
    }
    put(5, dict(QStringLiteral("/Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding ")));
    QString xref = QStringLiteral("xref\n0 5\n0000000000 65535 f \n");
    for (int n = 1; n <= 5; ++n) xref += QStringLiteral("%1 00000 n \n").arg(off[n], 10, 10, QLatin1Char('0'));
    const int xpos = pdf.size();
    pdf += xref.toUtf8();
    pdf += (QStringLiteral("trailer\n") + dict(QStringLiteral("/Root 1 0 R /Size 5"))
            + QStringLiteral("\nstartxref\n") + QString::number(xpos) + QStringLiteral("\n%%EOF\n")).toUtf8();
    return pdf;
}

// Minimal one-page PDF (Helvetica text) for "Export labels (PDF)".
QByteArray make_label_pdf(const QVector<std::pair<int, QString>> &rows)
{
    const QChar L('<'), G('>');
    auto dict = [&](const QString &inner){ QString s; s += L; s += L; s += inner; s += G; s += G; return s; };
    auto esc = [](const QString &s) {
        QString r;
        for (const QChar c : s) { if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('\\')) r += QLatin1Char('\\'); r += c; }
        return r;
    };
    const int per_page = 52;                       // 14pt leading on A4
    const int pages = qMax(1, (rows.size() + per_page - 1) / per_page);
    // object layout: 1 catalog, 2 pages-tree, 3..2+P page objects,
    // 3+P..2+2P content streams, 3+2P font
    const int fontObj = 2 + 2 * pages + 1;
    QByteArray pdf("%PDF-1.4\n");
    QVector<int> off(fontObj + 1, 0);
    auto put = [&](int n, const QString &s) { off[n] = pdf.size(); pdf += QStringLiteral("%1 0 obj\n").arg(n).toUtf8(); pdf += s.toUtf8(); pdf += QByteArrayLiteral("\nendobj\n"); };
    QString kids; for (int p = 0; p < pages; ++p) kids += QStringLiteral("%1 0 R ").arg(3 + p);
    put(1, dict(QStringLiteral("/Pages 2 0 R ")));
    put(2, dict(QStringLiteral("/Type /Pages /Kids [") + kids.trimmed() + QStringLiteral("] /Count ") + QString::number(pages) + QStringLiteral(" ")));
    const QString f0 = dict(QStringLiteral("/F0 ") + QString::number(fontObj) + QStringLiteral(" 0 R "));
    const QString res = dict(QStringLiteral("/Font ") + f0 + QStringLiteral(" "));
    for (int p = 0; p < pages; ++p) {
        const int pg = 3 + p, cont = 3 + pages + p;
        put(pg, dict(QStringLiteral("/Type /Page /Parent 2 0 R /Resources ") + res
            + QStringLiteral(" /MediaBox [0 0 595 842] /Contents ") + QString::number(cont) + QStringLiteral(" 0 R ")));
        QString body = QStringLiteral("BT\n/F0 9 Tf\n14 TL\n20 790 Td\n");
        for (int i = p * per_page; i < qMin((p + 1) * per_page, rows.size()); ++i)
            body += QStringLiteral("(") + esc(QStringLiteral("%1: %2").arg(rows[i].first).arg(rows[i].second))
                  + QStringLiteral(") Tj\nT*\n");
        body += QStringLiteral("ET\n");
        const QByteArray c = body.toUtf8();
        put(cont, dict(QStringLiteral("/Length ") + QString::number(c.size()) + QStringLiteral(" "))
                + QStringLiteral("stream\n") + QString::fromLatin1(c) + QStringLiteral("\nendstream"));
    }
    put(fontObj, dict(QStringLiteral("/Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding ")));
    QString xref = QStringLiteral("xref\n0 %1\n0000000000 65535 f \n").arg(fontObj);
    for (int n = 1; n <= fontObj; ++n) xref += QStringLiteral("%1 00000 n \n").arg(off[n], 10, 10, QLatin1Char('0'));
    const int xpos = pdf.size();
    pdf += xref.toUtf8();
    pdf += (QStringLiteral("trailer\n") + dict(QStringLiteral("/Root 1 0 R /Size ") + QString::number(fontObj))
            + QStringLiteral("\nstartxref\n") + QString::number(xpos) + QStringLiteral("\n%%EOF\n")).toUtf8();
    return pdf;
}
} // namespace pdfx
