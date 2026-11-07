#include "pmap.h"
#include <QFile>
#include <QRegularExpression>

static QString unhex_local(const QString &h) {
    QString out;
    for (int i = 0; i + 1 < h.size(); i += 2)
        out += QChar((quint8)h.mid(i, 2).toUInt(nullptr, 16));
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
                            la.type = (int)tm.captured(1).toUInt(nullptr, 16);
                    }
                    {
                        QRegularExpressionMatch vm = valRe.match(lb);
                        if (vm.isValid())
                            la.value = unhex_local(vm.captured(1));
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
    QString x = "<PMAP><NAME>" + to_hex_cp1252(p.name) + "</NAME>";
    for (const Map &m : p.maps) {
        x += "<MAP><POSKEY>"
           + QString("%1").arg(m.poskey, 4, 16, QLatin1Char('0')).toUpper()
           + "</POSKEY>";
        for (const Layer &l : m.layers)
            x += "<LAYER><TYPE>"
               + QString("%1").arg(l.type, 2, 16, QLatin1Char('0')).toUpper()
               + "</TYPE><VALUE>" + to_hex_cp1252(l.value) + "</VALUE></LAYER>";
        x += "</MAP>";
    }
    return x + "</PMAP>";
}

QString unhex_cp1252(const QString &h) { return unhex_local(h); }
