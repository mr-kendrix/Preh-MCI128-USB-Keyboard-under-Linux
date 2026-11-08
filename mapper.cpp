// pktm rules layer — parse + match + window probe; own TU, mounts on pmap.cpp's
// cp1252 hex decoder without touching that shared file (kanban collision rule).
#include "mapper.h"
#include "pmap.h"
#include <QProcess>
#include <QRegularExpression>

namespace pktm {

static const QString k_unknown = QStringLiteral("<unknown-window>");

static QString unhex_body(const QString &body) {
    // vendor bodies are hex on one line; whitespace-only bodies (e.g.
    // <Dependency>\n        </Dependency>) mean "empty" — strip ws first.
    QString hex;
    for (const QChar &c : body)
        if (!c.isSpace()) hex += c;
    return unhex_cp1252(hex);
}

static QString grab(const QString &scope, const QString &tag) {
    QRegularExpression re("<" + tag + ">(.*?)</" + tag + ">",
                          QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(scope);
    return m.isValid() ? unhex_body(m.captured(1)).trimmed() : QString();
}

RuleSet parse_rules(const QString &xml) {
    RuleSet rs;
    rs.name = grab(xml, "Name");
    QRegularExpression mapRe("<MAP>(.*?)</MAP>",
                             QRegularExpression::DotMatchesEverythingOption);
    QRegularExpression layerRe("<LAYER>(.*?)</LAYER>",
                               QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator mit = mapRe.globalMatch(xml);
    while (mit.hasNext()) {
        const QString mb = mit.next().captured(1);
        Rule r;
        r.poskey = grab(mb, "POSKey").toInt();
        r.text = grab(mb, "Text");
        QRegularExpressionMatchIterator lit = layerRe.globalMatch(mb);
        while (lit.hasNext()) {
            const QString lb = lit.next().captured(1);
            Layer l;
            l.type = grab(lb, "Type");
            l.value = grab(lb, "Value");
            l.dep = grab(lb, "Dependency");
            l.btf = grab(lb, "BringToForeground");
            l.rep = grab(lb, "AutoRepeat");
            l.slow = grab(lb, "SlowOutput");
            r.layers.append(l);
        }
        rs.rules.append(r);
    }
    return rs;
}

static bool dep_matches(const QString &dep, const QString &window) {
    if (dep.isEmpty() || window == k_unknown) return false;
    return window.contains(dep, Qt::CaseInsensitive) ||
           dep.contains(window, Qt::CaseInsensitive);
}

int match_layer(const Rule &r, const QString &window) {
    // window-specific layer wins, else the unconditional one
    for (int i = 0; i < r.layers.size(); ++i)
        if (!r.layers[i].dep.isEmpty() && dep_matches(r.layers[i].dep, window))
            return i;
    for (int i = 0; i < r.layers.size(); ++i)
        if (r.layers[i].dep.isEmpty())
            return i;
    return -1;
}

QString literal_to_type(const Rule &r, const Layer &l) {
    // "{N5}", "{Esc}", "{TAB}" are key-macro placeholders, not literal text:
    // strip {..} runs; nothing literal left (empty Value too) -> cap <Text>.
    // no regex needed: drop {..} macro placeholders, keep literal chars
    QString v;
    bool in_macro = false;
    for (const QChar &c : l.value) {
        if (c == QLatin1Char('{')) in_macro = true;
        else if (c == QLatin1Char('}')) in_macro = false;
        else if (!in_macro) v += c;
    }
    return v.isEmpty() ? r.text : v;
}

QString active_window() {
    QProcess p;
    p.start(QStringLiteral("xdotool"),
            {QStringLiteral("getactivewindow"), QStringLiteral("getwindowname")});
    if (p.error() == QProcess::FailedToStart)
        return k_unknown;
    p.waitForFinished(1500);
    const QString name = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    return name.isEmpty() ? k_unknown : name;
}

QString unknown_window() { return k_unknown; }

}
