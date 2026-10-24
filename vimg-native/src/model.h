#pragma once
// vimg 文書(*.vimg.json)のモデル。image.js の fix()/loadDoc() と同じ既定値の補完を行う。
#include <QJsonObject>
#include <QHash>
#include <QStringList>
#include <QJsonArray>
#include <QString>
#include <QList>
#include <QColor>
#include <QImage>

struct Outline { QString color = "#000000"; double width = 6; };
struct Shadow { QString color = "#000000"; double opacity = 0.6, x = 6, y = 6, blur = 8; };
struct Stroke { QString color = "#FFFFFF"; double width = 6; };
struct Adjust { double brightness = 0, contrast = 0, saturation = 0;
  bool any() const { return brightness || contrast || saturation; } };

struct Layer {
  QString type;            // text / shape / image
  QString id, name;
  double x = 0, y = 0, w = 100, h = 100, rot = 0, opacity = 1;
  bool visible = true, locked = false, exportIt = true;
  bool hasShadow = false; Shadow shadow;
  bool xNull = false, yNull = false, wNull = false, hNull = false;  // 省略(中央/原寸)
  // text
  QString text = "テキスト", font = QString::fromUtf8("HGP創英角ｺﾞｼｯｸUB"), color = "#FFFFFF";
  double size = 64, lineHeight = 1.25, letterSpacing = 0, scaleX = 100, maxW = 0;
  bool bold = false, autoFit = true;
  QString align = "center", valign = "middle";
  QList<Outline> outlines{Outline{}};
  bool hasGradient = false; QString gradTop = "#FFF27A", gradBottom = "#FF9A1F";
  // shape
  QString shape = "rect"; QString fill = "#3B82F6"; bool hasFill = true;
  bool hasStroke = false; Stroke stroke; double radius = 24;
  // image
  QString src; bool flip = false; Adjust adjust;
  QJsonObject extra;       // 知らないキーは捨てずに持ち回る
  QHash<QString, QStringList> order;   // 読み込み時のキー順(相対パス "" / "shadow" / "outlines/0" ...)。保存時に再現する
  static Layer fromJson(const QJsonObject &o);
  QJsonObject toJson() const;
};

struct Doc {
  int version = 1;
  int width = 1920, height = 1080;
  QString background;      // 空 = 透明
  QList<Layer> layers;
  QJsonObject extra;
  QHash<QString, QStringList> order;   // 文書側のキー順("" など。レイヤーは各 Layer::order)
  Layer *find(const QString &id);
  int indexOf(const QString &id) const;
  QJsonObject toJson() const;
  QByteArray toBytes() const;       // 保存形式(インデント付き)
  static Doc fromJson(const QJsonObject &o, const QString &baseDir, QString *err = nullptr);
  static bool load(const QString &path, Doc &out, QString *err);
  bool save(const QString &path, QString *err) const;
};

namespace vimg {
extern const QString EXT;                 // ".vimg.json"
bool isDoc(const QString &p);
QString pngOf(const QString &doc);        // a/b.vimg.json -> a/b.png
QString newId();
QString resolveSrc(const QString &baseDir, const QString &src);  // 絶対パスか文書からの相対か
QImage loadImage(const QString &path);
QString fromRaster(const QString &src, QString *err);   // 画像 -> 隣の .vimg.json(<名前>.src.png 退避)
QString newName(const Layer &l);
}
