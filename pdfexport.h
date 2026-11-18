#ifndef PKT_PDFEXPORT_H
#define PKT_PDFEXPORT_H
#include <QHash>
#include <QVector>
#include <utility>
#include <QByteArray>
#include <QString>
namespace pdfx {
QByteArray make_cut_sheet(const QHash<int, QString> &texts, const QHash<int, QString> &fonts);
QByteArray make_label_pdf_geom(const QVector<std::pair<int, QString>> &rows,
                               const QVector<std::pair<int, QString>> &types,
                               const QHash<int, QString> &texts, const QHash<int, QString> &fonts);
QByteArray make_label_pdf(const QVector<std::pair<int, QString>> &rows);
}
#endif