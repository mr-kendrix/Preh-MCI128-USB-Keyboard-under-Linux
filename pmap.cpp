#include "pmap.h"
#include <QFile>
#include <QRegularExpression>

static QString unhex_local(const QString &h) {
    // cp1252 (vendor encoding): bytes 0x80-0x9F are NOT latin-1, they are the
    // windows-1252 block (€ = 0x80 -> U+20AC). Without this the euro sign and
    // quotes decode to invisible control chars and xkb refuses them.
    static const ushort cp1252[32] = {
        0x20AC,0x81,0x201A,0x0192,0x201E,0x2026,0x2020,0x2021,
        0x02C6,0x2030,0x0160,0x2039,0x0152,0x8D,0x017D,0x8F,
        0x90,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,
        0x02DC,0x2122,0x0161,0x203A,0x0153,0x9E,0x017E,0x0178,
    };
    QString out;
    for (int i = 0; i + 1 < h.size(); i += 2) {
        const uint b = h.mid(i, 2).toUInt(nullptr, 16);
        out += (b >= 0x80 && b <= 0x9F) ? QChar(cp1252[b - 0x80]) : QChar(b);
    }
    return out;
}

Pmap parse_pmap(const QString &xml) {
    Pmap p;
    QRegularExpression nameRe("<Name>(.*?)</Name>",
                              QRegularExpression::DotMatchesEverythingOption);
    if (auto m = nameRe.match(xml); m.isValid())
        p.name = unhex_local(m.captured(1));

    QRegularExpression mapRe("<MAP>(.*?)</MAP>",
                             QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression pkRe("<POSKey>(.*?)</POSKey>",
                            QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression layerRe("<LAYER>(.*?)</LAYER>",
                               QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression typeRe("<Type>(.*?)</Type>",
                              QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression valRe("<Value>(.*?)</Value>",
                             QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression depRe("<Dependency>(.*?)</Dependency>",
                             QRegularExpression::DotMatchesEverythingOption);

    {
        QRegularExpressionMatchIterator it = mapRe.globalMatch(xml);
        while (it.hasNext()) {
            const QRegularExpressionMatch mm = it.next();
            const QString body = mm.captured(1);
            Map m;
            {
                QRegularExpressionMatch pm = pkRe.match(body);
                if (pm.isValid()) {
                    const QString digits = unhex_local(pm.captured(1));
                    m.poskey = digits.toInt();
                }
            }
            {
                QRegularExpressionMatchIterator lt = layerRe.globalMatch(body);
                while (lt.hasNext()) {
                    const QRegularExpressionMatch lm = lt.next();
                    const QString lb = lm.captured(1);
                    Layer la;
                    {
                        QRegularExpressionMatch tm = typeRe.match(lb);
                        if (tm.isValid())
                            la.type = unhex_local(tm.captured(1)).trimmed();
                    }
                    {
                        QRegularExpressionMatch vm = valRe.match(lb);
                        if (vm.isValid())
                            la.value = unhex_local(vm.captured(1));
                    }
                    {
                        QRegularExpressionMatch dm = depRe.match(lb);
                        if (dm.isValid()) {
                            const QString b = dm.captured(1);
                            if (!b.trimmed().isEmpty())   // vendor empty = whitespace, not hex
                                la.dependency = unhex_local(b).trimmed();
                        }
                    }
                    m.layers.append(la);
                }
            }
            p.maps.append(m);
        }
    }
    return p;
}

QString to_hex_cp1252(const QString &s) {
    QString out;
    for (const QChar &c : s)
        out += QString("%1").arg(static_cast<unsigned>(c.unicode()), 2, 16, QLatin1Char('0')).toUpper();
    return out;
}

QString serialize_pmap(const Pmap &p) {
    // vendor shape (Mr.Kendrix.pmap): <POSKeyMapper><MAPPING><MAP><POSKey>..
    // ..</POSKey><LAYER><Type>..</Type><Value>..</Value></LAYER></MAP></MAPPING>
    // <LAYOUT><Name>..</Name></LAYOUT></POSKeyMapper>; every text field is
    // hex-encoded cp1252, so POSKey is the hex of the DECIMAL DIGITS string.
    QString x = "<POSKeyMapper><MAPPING>";
    for (const Map &m : p.maps) {
        x += "<MAP><POSKey>" + to_hex_cp1252(QString::number(m.poskey)) + "</POSKey>";
        for (const Layer &l : m.layers) {
            x += "<LAYER><Type>" + to_hex_cp1252(l.type)
               + "</Type><Value>" + to_hex_cp1252(l.value) + "</Value>";
            if (!l.dependency.isEmpty())
                x += "<Dependency>" + to_hex_cp1252(l.dependency) + "</Dependency>";
            x += "</LAYER>";
        }
        x += "</MAP>";
    }
    x += "</MAPPING><LAYOUT><Name>" + to_hex_cp1252(p.name)
       + "</Name></LAYOUT></POSKeyMapper>";
    return x;
}

QString unhex_cp1252(const QString &h) { return unhex_local(h); }
