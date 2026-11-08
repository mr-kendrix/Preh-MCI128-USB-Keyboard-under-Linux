#pragma once
#include <QString>
#include <QVector>

// pktm — the rules layer above the raw pmap: every <MAP> becomes one rule
// carrying <POSKey>, <Text>, every <LAYER><Type/Value>, <Dependency>,
// <BringToForeground>, <AutoRepeat>, <SlowOutput>; ALL tag bodies are
// cp1252-hexed (vendor convention: <POSKey>313234</POSKey> = "124").
// Namespaced so pktm::Layer cannot collide with pmap.h's global Layer.
namespace pktm {

struct Layer { QString type, value, dep, btf, rep, slow; };
struct Rule  { int poskey = 0; QString text; QVector<Layer> layers; };
struct RuleSet { QString name; QVector<Rule> rules; };

RuleSet parse_rules(const QString &xml);

// index of the <MAP>.layers entry an inject would use for this window
// ('<unknown-window>' admits only unconditional/empty-dep layers), or -1
int match_layer(const Rule &r, const QString &window);

// literal text an inject would type: the decoded Value with {...} macro
// placeholders stripped; empty Value (or macro-only) falls back to the cap <Text>
QString literal_to_type(const Rule &r, const Layer &l);

// active window title via QProcess xdotool; '<unknown-window>' if absent/failing
QString active_window();

// the sentinel string active_window() returns when no window can be observed
QString unknown_window();

}
