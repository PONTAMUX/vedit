#include "model.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QImageReader>
#include <QUuid>
#include <QSet>
#include <cmath>

namespace vimg {
const QString EXT = ".vimg.json";
bool isDoc(const QString &p) { return p.toLower().endsWith(EXT); }
QString pngOf(const QString &d) {
  return isDoc(d) ? d.left(d.size() - EXT.size()) + ".png" : QFileInfo(d).path() + "/" + QFileInfo(d).completeBaseName() + ".png";
}
QString newId() { return "L" + QUuid::createUuid().toString(QUuid::Id128).left(8); }
QString resolveSrc(const QString &baseDir, const QString &src) {
  if (QFileInfo(src).isAbsolute()) return src;
  return QDir::cleanPath(baseDir + "/" + src);
}
QImage loadImage(const QString &path) {
  QImageReader r(path); r.setAutoTransform(false);
  return r.read();
}
QString newName(const Layer &l) {
  if (l.type == "text") { QString s = l.text.section('\n', 0, 0).left(20); return s.isEmpty() ? QString::fromUtf8("テキスト") : s; }
  if (l.type == "shape") return QString::fromUtf8("図形");
  return QFileInfo(l.src.isEmpty() ? QString::fromUtf8("画像") : l.src).fileName();
}
QString fromRaster(const QString &srcPath, QString *err) {
  QFileInfo si(srcPath); QString dir = si.absolutePath(), stem = si.completeBaseName(), suf = si.suffix().toLower();
  QString docPath = dir + "/" + stem + EXT;
  if (QFile::exists(docPath)) return docPath;
  if (suf != "png" && QFile::exists(pngOf(docPath))) {
    docPath = dir + "/" + stem + "_" + suf + EXT;
    if (QFile::exists(docPath)) return docPath;
  }
  QString keep = dir + "/" + stem + ".src." + suf;
  if (!QFile::exists(keep) && !QFile::copy(srcPath, keep)) { if (err) *err = "元画像を退避できません: " + keep; return {}; }
  Doc d; d.width = 0; d.height = 0;
  Layer l; l.type = "image"; l.name = si.fileName(); l.src = QFileInfo(keep).fileName(); l.id = newId();
  d.layers.append(l);
  // サイズ未定のまま書く(読み込み時に画像の原寸・中央になる)
  QJsonObject o = d.toJson();
  QJsonArray a; QJsonObject lo = l.toJson();
  for (const char *k : {"x", "y", "w", "h"}) lo[k] = QJsonValue(QJsonValue::Null);
  a.append(lo); o["layers"] = a; o["width"] = QJsonValue(QJsonValue::Null); o["height"] = QJsonValue(QJsonValue::Null);
  QFile f(docPath);
  if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "書き込めません: " + docPath; return {}; }
  f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
  return docPath;
}
}

static QString colorStr(const QJsonValue &v, const QString &def) { return v.isString() ? v.toString() : def; }
static double num(const QJsonObject &o, const char *k, double def) {
  QJsonValue v = o.value(k);
  if (v.isDouble()) return v.toDouble();
  if (v.isString()) { bool ok; double d = v.toString().toDouble(&ok); if (ok) return d; }
  return def;
}
static bool flag(const QJsonObject &o, const char *k, bool def) {
  QJsonValue v = o.value(k); return v.isBool() ? v.toBool() : (v.isDouble() ? v.toDouble() != 0 : def);
}

Layer Layer::fromJson(const QJsonObject &o) {
  Layer l; l.type = o.value("type").toString("text");
  l.id = o.value("id").toString(); l.name = o.value("name").toString();
  static const QStringList known = {"type","id","name","x","y","w","h","rot","opacity","visible","locked","export","shadow",
    "text","font","size","color","bold","line_height","letter_spacing","scale_x","max_w","align","valign","outlines","gradient","auto",
    "shape","fill","stroke","radius","src","flip","adjust"};
  for (auto it = o.begin(); it != o.end(); ++it) if (!known.contains(it.key())) l.extra.insert(it.key(), it.value());
  auto pos = [&](const char *k, double &v, bool &isNull) { if (!o.contains(k) || o.value(k).isNull()) { isNull = true; v = 0; } else v = num(o, k, 0); };
  pos("x", l.x, l.xNull); pos("y", l.y, l.yNull);
  if (l.type == "image") { pos("w", l.w, l.wNull); pos("h", l.h, l.hNull); }
  else { l.w = num(o, "w", 100); l.h = num(o, "h", 100); }
  l.rot = num(o, "rot", 0); l.opacity = num(o, "opacity", 1);
  l.visible = flag(o, "visible", true); l.locked = flag(o, "locked", false); l.exportIt = flag(o, "export", true);
  if (o.value("shadow").isObject()) {
    QJsonObject s = o.value("shadow").toObject(); l.hasShadow = true;
    l.shadow.color = colorStr(s.value("color"), "#000000"); l.shadow.opacity = s.contains("opacity") ? num(s, "opacity", 1) : 1;
    l.shadow.x = num(s, "x", 0); l.shadow.y = num(s, "y", 0); l.shadow.blur = num(s, "blur", 0);
  }
  if (l.type == "text") {
    l.text = o.contains("text") ? o.value("text").toVariant().toString() : l.text;
    l.font = o.value("font").toString(l.font); l.size = num(o, "size", 64); l.color = colorStr(o.value("color"), "#FFFFFF");
    l.bold = flag(o, "bold", false); l.lineHeight = num(o, "line_height", 1.25); l.letterSpacing = num(o, "letter_spacing", 0);
    l.scaleX = num(o, "scale_x", 100); l.maxW = num(o, "max_w", 0);
    l.align = o.value("align").toString("center"); l.valign = o.value("valign").toString("middle");
    l.autoFit = flag(o, "auto", true);
    if (o.value("outlines").isArray()) {
      l.outlines.clear();
      for (auto v : o.value("outlines").toArray()) { QJsonObject q = v.toObject(); l.outlines.append({colorStr(q.value("color"), "#000000"), num(q, "width", 0)}); }
    }
    if (o.value("gradient").isObject()) {
      QJsonObject g = o.value("gradient").toObject(); l.hasGradient = true;
      l.gradTop = colorStr(g.value("top"), l.gradTop); l.gradBottom = colorStr(g.value("bottom"), l.gradBottom);
    }
  } else if (l.type == "shape") {
    l.shape = o.value("shape").toString("rect");
    if (o.contains("fill")) { l.hasFill = o.value("fill").isString(); l.fill = o.value("fill").toString(); }
    if (o.value("stroke").isObject()) { QJsonObject s = o.value("stroke").toObject(); l.hasStroke = true; l.stroke.color = colorStr(s.value("color"), "#FFFFFF"); l.stroke.width = num(s, "width", 0); }
    l.radius = num(o, "radius", 24);
  } else if (l.type == "image") {
    l.src = o.value("src").toString(); l.flip = flag(o, "flip", false);
    if (o.value("adjust").isObject()) { QJsonObject a = o.value("adjust").toObject(); l.adjust = {num(a, "brightness", 0), num(a, "contrast", 0), num(a, "saturation", 0)}; }
  }
  if (l.name.isEmpty()) l.name = vimg::newName(l);
  return l;
}

QJsonObject Layer::toJson() const {
  QJsonObject o = extra;
  auto cn = [](double v) { return std::round(v * 1000) / 1000; };
  o["type"] = type; o["id"] = id; o["name"] = name;
  auto setPos = [&](const char *k, double v, bool n) { o[k] = n ? QJsonValue(QJsonValue::Null) : QJsonValue(cn(v)); };
  setPos("x", x, xNull); setPos("y", y, yNull);
  if (type == "image") { setPos("w", w, wNull); setPos("h", h, hNull); } else { o["w"] = cn(w); o["h"] = cn(h); }
  o["rot"] = cn(rot); o["opacity"] = cn(opacity); o["visible"] = visible; o["locked"] = locked; o["export"] = exportIt;
  if (hasShadow) o["shadow"] = QJsonObject{{"color", shadow.color}, {"opacity", shadow.opacity}, {"x", shadow.x}, {"y", shadow.y}, {"blur", shadow.blur}};
  else o["shadow"] = QJsonValue(QJsonValue::Null);
  if (type == "text") {
    o["text"] = text; o["font"] = font; o["size"] = cn(size); o["color"] = color; o["bold"] = bold; o["line_height"] = lineHeight;
    o["letter_spacing"] = letterSpacing; o["scale_x"] = scaleX; o["max_w"] = maxW; o["align"] = align; o["valign"] = valign;
    QJsonArray oa; for (auto &q : outlines) oa.append(QJsonObject{{"color", q.color}, {"width", q.width}});
    o["outlines"] = oa;
    o["gradient"] = hasGradient ? QJsonValue(QJsonObject{{"top", gradTop}, {"bottom", gradBottom}}) : QJsonValue(QJsonValue::Null);
    o["auto"] = autoFit;
  } else if (type == "shape") {
    o["shape"] = shape; o["fill"] = hasFill ? QJsonValue(fill) : QJsonValue(QJsonValue::Null);
    o["stroke"] = hasStroke ? QJsonValue(QJsonObject{{"color", stroke.color}, {"width", stroke.width}}) : QJsonValue(QJsonValue::Null);
    o["radius"] = radius;
  } else if (type == "image") {
    o["src"] = src; o["flip"] = flip;
    o["adjust"] = QJsonObject{{"brightness", adjust.brightness}, {"contrast", adjust.contrast}, {"saturation", adjust.saturation}};
  }
  return o;
}

Layer *Doc::find(const QString &id) { for (auto &l : layers) if (l.id == id) return &l; return nullptr; }
int Doc::indexOf(const QString &id) const { for (int i = 0; i < layers.size(); i++) if (layers[i].id == id) return i; return -1; }

QJsonObject Doc::toJson() const {
  QJsonObject o = extra;
  o["version"] = version; o["width"] = width; o["height"] = height;
  o["background"] = background.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(background);
  QJsonArray a; for (auto &l : layers) a.append(l.toJson());
  o["layers"] = a; return o;
}
// ---- キー順を保つ読み書き(Python の json.dumps(indent=2, ensure_ascii=False) と同じ体裁) ----
typedef QHash<QString, QStringList> OrderMap;
static void skipWs(const char *&p, const char *e) { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
static QString scanString(const char *&p, const char *e) {
  const char *s = ++p; QByteArray raw;
  while (p < e && *p != '"') { if (*p == '\\' && p + 1 < e) p++; p++; }
  raw = QByteArray(s - 1, int(p - s) + 2); if (p < e) p++;
  QJsonParseError pe; QJsonDocument d = QJsonDocument::fromJson("[" + raw + "]", &pe);
  return d.array().first().toString();
}
static void scanValue(const char *&p, const char *e, const QString &path, OrderMap &out) {
  skipWs(p, e); if (p >= e) return;
  if (*p == '{') {
    p++; QStringList keys;
    for (;;) {
      skipWs(p, e); if (p >= e) break;
      if (*p == '}') { p++; break; }
      if (*p == ',') { p++; continue; }
      QString k = scanString(p, e); keys << k; skipWs(p, e); if (p < e && *p == ':') p++;
      scanValue(p, e, path.isEmpty() ? k : path + "/" + k, out);
    }
    out[path] = keys;
  } else if (*p == '[') {
    p++; int i = 0;
    for (;;) {
      skipWs(p, e); if (p >= e) break;
      if (*p == ']') { p++; break; }
      if (*p == ',') { p++; continue; }
      scanValue(p, e, path.isEmpty() ? QString::number(i) : path + "/" + QString::number(i), out); i++;
    }
  } else if (*p == '"') scanString(p, e);
  else while (p < e && *p != ',' && *p != '}' && *p != ']') p++;
}
static void jstr(QByteArray &o, const QString &s) {
  o += '"';
  for (QChar c : s) {
    ushort u = c.unicode();
    if (u == '"') o += "\\\""; else if (u == 0x5c) o += "\\\\"; else if (u == '\n') o += "\\n"; else if (u == '\r') o += "\\r"; else if (u == '\t') o += "\\t";
    else if (u < 0x20) o += QByteArray("\\u") + QByteArray::number(u, 16).rightJustified(4, '0');
    else o += QString(c).toUtf8();
  }
  o += '"';
}
static void putValue(QByteArray &o, const QJsonValue &v, const QString &path, const OrderMap &om, int ind) {
  QByteArray pad(ind * 2, ' '), pad2((ind + 1) * 2, ' ');
  if (v.isObject()) {
    QJsonObject ob = v.toObject(); if (ob.isEmpty()) { o += "{}"; return; }
    QStringList keys; for (const QString &k : om.value(path)) if (ob.contains(k) && !keys.contains(k)) keys << k;
    for (const QString &k : ob.keys()) if (!keys.contains(k)) keys << k;   // 新しいキーは末尾
    o += "{\n";
    for (int i = 0; i < keys.size(); i++) {
      o += pad2; jstr(o, keys[i]); o += ": ";
      putValue(o, ob.value(keys[i]), path.isEmpty() ? keys[i] : path + "/" + keys[i], om, ind + 1);
      o += i + 1 < keys.size() ? ",\n" : "\n";
    }
    o += pad + "}";
  } else if (v.isArray()) {
    QJsonArray a = v.toArray(); if (a.isEmpty()) { o += "[]"; return; }
    o += "[\n";
    for (int i = 0; i < a.size(); i++) {
      o += pad2; putValue(o, a[i], path.isEmpty() ? QString::number(i) : path + "/" + QString::number(i), om, ind + 1);
      o += i + 1 < a.size() ? ",\n" : "\n";
    }
    o += pad + "]";
  } else if (v.isString()) jstr(o, v.toString());
  else if (v.isBool()) o += v.toBool() ? "true" : "false";
  else if (v.isNull() || v.isUndefined()) o += "null";
  else { double d = v.toDouble(); if (d == std::floor(d) && std::fabs(d) < 1e15) o += QByteArray::number(qlonglong(d)); else o += QByteArray::number(d, 'g', QLocale::FloatingPointShortest); }
}
QByteArray Doc::toBytes() const {
  OrderMap om = order;
  if (!om.contains("")) om[""] = {"version", "width", "height", "background", "layers"};
  for (int i = 0; i < layers.size(); i++)
    for (auto it = layers[i].order.begin(); it != layers[i].order.end(); ++it)
      om["layers/" + QString::number(i) + (it.key().isEmpty() ? "" : "/" + it.key())] = it.value();
  QByteArray o; putValue(o, toJson(), "", om, 0); o += '\n'; return o;
}

Doc Doc::fromJson(const QJsonObject &o, const QString &baseDir, QString *) {
  Doc d;
  for (auto it = o.begin(); it != o.end(); ++it)
    if (it.key() != "version" && it.key() != "width" && it.key() != "height" && it.key() != "background" && it.key() != "layers") d.extra.insert(it.key(), it.value());
  d.version = o.contains("version") ? int(num(o, "version", 1)) : 1;
  d.width = int(std::lround(num(o, "width", 0))); d.height = int(std::lround(num(o, "height", 0)));
  d.background = o.value("background").isString() ? o.value("background").toString() : QString();
  for (auto v : o.value("layers").toArray()) d.layers.append(Layer::fromJson(v.toObject()));
  QSet<QString> used;
  for (auto &l : d.layers) {
    if (l.id.isEmpty() || used.contains(l.id)) l.id = vimg::newId();
    used.insert(l.id);
    if (l.type == "image" && (l.wNull || l.hNull || l.xNull || l.yNull)) {
      QImage im = vimg::loadImage(vimg::resolveSrc(baseDir, l.src));
      if (l.wNull || l.hNull) { l.w = im.isNull() ? 100 : im.width(); l.h = im.isNull() ? 100 : im.height(); l.wNull = l.hNull = false; }
      if (!d.width || !d.height) { d.width = int(std::lround(l.w)); d.height = int(std::lround(l.h)); }
      if (l.xNull) { l.x = (d.width ? d.width : l.w) / 2; l.xNull = false; }
      if (l.yNull) { l.y = (d.height ? d.height : l.h) / 2; l.yNull = false; }
    }
  }
  if (!d.width) d.width = 1920;
  if (!d.height) d.height = 1080;
  for (auto &l : d.layers) { if (l.xNull) { l.x = d.width / 2.0; l.xNull = false; } if (l.yNull) { l.y = d.height / 2.0; l.yNull = false; } l.wNull = l.hNull = false; }
  return d;
}

bool Doc::load(const QString &path, Doc &out, QString *err) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "ファイルを開けません: " + path; return false; }
  QJsonParseError pe; QJsonDocument jd = QJsonDocument::fromJson(f.readAll(), &pe);
  if (!jd.isObject()) { if (err) *err = "JSON が読めません: " + pe.errorString(); return false; }
  out = Doc::fromJson(jd.object(), QFileInfo(path).absolutePath(), err);
  // キー順の記録(保存時に再現する)
  OrderMap om; QByteArray raw = jd.toJson(); QFile f2(path); f2.open(QIODevice::ReadOnly); raw = f2.readAll();
  const char *p = raw.constData(); if (raw.startsWith("\xEF\xBB\xBF")) p += 3;
  scanValue(p, raw.constData() + raw.size(), "", om);
  for (auto it = om.begin(); it != om.end(); ++it) {
    const QString &k = it.key();
    if (k.startsWith("layers/")) {
      QStringList parts = k.split('/'); int i = parts[1].toInt();
      if (i >= 0 && i < out.layers.size()) out.layers[i].order[parts.mid(2).join('/')] = it.value();
    } else if (k != "layers") out.order[k] = it.value();
  }
  return true;
}
bool Doc::save(const QString &path, QString *err) const {
  QFile f(path + ".tmp");
  if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "書き込めません: " + path; return false; }
  f.write(toBytes()); f.close();
  QFile::remove(path);
  if (!QFile::rename(path + ".tmp", path)) { if (err) *err = "保存に失敗: " + path; return false; }
  return true;
}
