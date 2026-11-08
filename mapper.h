#pragma once
#include <QString>
#include <QVector>
#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QSet>

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
int match_layer(const Rule &r, const QString &process, const QString &window);

// literal text an inject would type: the decoded Value with {...} macro
// placeholders stripped; empty Value (or macro-only) falls back to the cap <Text>
QString literal_to_type(const Rule &r, const Layer &l);

// active window title via QProcess xdotool; '<unknown-window>' if absent/failing
QString active_window();

// the sentinel string active_window() returns when no window can be observed
QString unknown_window();

// active window's process name via getwindowpid -> /proc/PID/comm (empty if unavailable)
QString active_process();

// ---- live IN feed (pktm --live) -------------------------------------
// port of pktwire's decode_report/split_reports: IN reports are
// [04 b1 b2 b3 b4]; 0xFE = key-up marker, 0xE0 = extended prefix
// (scancode then 0xE000|code); remaining bytes are raw AT Set-1 makes.
struct KeyEvent { int scancode = 0; bool down = false; bool extended = false; };
QVector<KeyEvent> decode_report(const unsigned char *payload); // 4 bytes, id byte stripped
QVector<KeyEvent> decode_stream(const QByteArray &raw);        // resyncs on 0x04 frames

// scancode->POSKey from pktd's MK.calib.json (keys "0xSCNC"), QJsonDocument parse
QHash<int, int> load_calib_json(const QString &path);

// literal text -> (evdev code, state) pairs: word table (Tab/Löschen/… + a
// few EN synonyms) else printable chars (uppercase = shift chain).
// Empty result = nothing injectable (never a silent half-string).
QVector<QPair<int, int>> evdev_sequence(const QString &text);

// ydotool 1.x client: unix DGRAM socket, one 24-byte input_event record per send
// {int64 tv_sec, int64 tv_usec, u16 type, u16 code, s32 value} + an EV_SYN record
// per event — mirrors uinput_emit() in Client/ydotool.c of ydotool 1.0.4 (the
// daemon's recv reads sizeof(input_event)=24 on x86_64: a 16-byte datagram is
// silently DROPPED, verified headless against the live ydotoold at /tmp/.ydotool_socket).
// 0 = all sent; negative = -errno of connect/send failure.
int ydot_send(const QString &socket_path, const QVector<QPair<int, int>> &events);

}
