#pragma once
// Lossless pmap DOM (Phase 0, 2026-11-12): verbatim round trip.
// Vendor files verified (Mr.Kendrix + vendor samples): only simple <TAG>text</TAG>,
// no attributes, no CDATA, no entities, no self-closing, indentation lives in
// text nodes. serialize(parse(x)) == x BY CONSTRUCTION (text kept verbatim).
// DESIGN: one flat slot pool; every reference is an index (realloc-safe:
// parents write child indices only AFTER nested parses finished).
#include <QByteArray>
#include <QString>
#include <QVector>
#include "pmap.h"   // Pmap model types for dom_apply_model

struct DomSlot {
    bool is_text = false;
    bool selfclose = false;    // '<TAG />' form (kept for byte-exact round trip)
    QString name;              // element name when !is_text
    QByteArray text;           // verbatim bytes when is_text
    QVector<int> children;     // slot indices when !is_text (text children included)
};
struct DomDoc {
    QVector<int> roots;      // top-level slot indices
    QVector<DomSlot> pool;   // all slots (RENAMED from 'slots': KDE macro clash)
};

bool dom_parse(const QByteArray &raw, DomDoc &out);
QByteArray dom_serialize(const DomDoc &d);

// element child of slot `slot` named `tag`, or -1
int dom_find_child(const DomDoc &d, int slot, const QString &tag);
// concatenated text of direct text-children of slot
QByteArray dom_text(const DomDoc &d, int slot);
// direct element children of slot named tag, in order
QVector<int> dom_find_all(const DomDoc &d, int slot, const QString &tag);
// set the single text payload of element slot (creates one if absent)
bool dom_set_text(DomDoc &d, int slot, const QByteArray &t);
// model view re-applied onto a DOM (Phase 0: value edits only; structural
// changes => err + empty return). to_hex_cp1252/unhex_cp1252 from pmap.cpp.
QByteArray dom_apply_model(DomDoc &d, const Pmap &pm, QString &err);
