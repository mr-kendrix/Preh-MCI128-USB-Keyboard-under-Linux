#pragma once
#include <QString>
#include <QVector>
struct Layer { QString type = QStringLiteral("TEXT"); QString value; };
struct Map  { int poskey = 0; QVector<Layer> layers; };
struct Pmap { QString name; QVector<Map> maps; };

Pmap parse_pmap(const QString &xml);
QString serialize_pmap(const Pmap &p);
QString unhex_cp1252(const QString &hexbytes);
QString to_hex_cp1252(const QString &s);
