#pragma once
#include <QString>
#include <QVector>
#include <QHash>
struct Layer { QString type = QStringLiteral("TEXT"); QString value; QString dependency;
               QString btf, rep, slow; /* BringToForeground/AutoRepeat/SlowOutput, decoded */ };
struct Map  { int poskey = 0; QVector<Layer> layers; };
struct Pmap { QString name; QVector<Map> maps; };
struct KeyGeom { int x = 0, y = 0, w = 0, h = 0, parent = 0;
                 QString text; double fsize = 0; int fore = -1, back = -1; };  // <Key>/<Print> incl. print spec
inline QHash<int, KeyGeom> g_geom;   // POSKey -> visual key geometry (empty = legacy mode)

Pmap parse_pmap(const QString &xml);
QString serialize_pmap(const Pmap &p);
QString unhex_cp1252(const QString &hexbytes);
QString to_hex_cp1252(const QString &s);
