#pragma once
// 描画エンジン(画面とは独立)。image.js の drawText / drawShape / drawLayer / renderTo と同じ手順で QPainter に描く。
#include "model.h"
#include <QImage>
#include <QStringList>
#include <QVector>
#include <QFont>

namespace vimg {
struct TextMetrics {
  QStringList lines; QVector<double> ws;
  double sx = 1, tw = 1, th = 0, adv = 0, pad = 0;
};
QFont fontOf(const Layer &l);
bool fakeBold(const Layer &l);
double outlinePad(const Layer &l);
QColor cssColor(const QString &s, double alpha = 1.0);
TextMetrics measure(const Layer &l);
void fitText(Layer &l);                       // auto のとき枠を文字に合わせる(中心は動かさない)
// 文書全体を透過 ARGB32_Premultiplied に描く。forExport=true なら export:false の下敷きを除く。
// missing に、読み込めなかった(表示中の)画像の src を入れる。
QImage renderDoc(Doc &d, const QString &baseDir, bool forExport, QStringList *missing = nullptr);
QImage cachedImage(const QString &path);      // 読み込み済み画像(更新日時が変わったら読み直す)
bool writePng(const QImage &img, const QString &path, QString *err = nullptr);
}
