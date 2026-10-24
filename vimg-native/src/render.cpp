#include "render.h"
#include <QPainter>
#include <QPainterPath>
#include <QFontMetricsF>
#include <QRawFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>
#include <algorithm>

namespace vimg {

// 太字の面が無いフォントは Chrome(Skia)が輪郭を太らせて擬似太字にする。その判定
static int fbMode() { static int m = qEnvironmentVariableIsSet("VIMG_FB") ? qgetenv("VIMG_FB").toInt() : 1; return m; }
bool fakeBold(const Layer &l) {
  if (!l.bold || fbMode() == 0) return false;
  static QHash<QString, bool> cache; auto it = cache.find(l.font); if (it != cache.end()) return it.value();
  QFont probe(l.font); probe.setBold(true);
  bool real = false;
  for (const QString &st : QFontDatabase::styles(QFontInfo(probe).family())) if (QFontDatabase::weight(QFontInfo(probe).family(), st) >= 600) real = true;
  cache.insert(l.font, !real); return !real;
}
QFont fontOf(const Layer &l) {
  QFont f(l.font);
  f.setPixelSize(std::max(1, int(std::lround(l.size))));
  f.setBold(l.bold && !fakeBold(l));
  f.setHintingPreference(QFont::PreferNoHinting);
  f.setStyleStrategy(QFont::StyleStrategy(QFont::PreferAntialias | QFont::PreferQuality));
  if (l.letterSpacing != 0) f.setLetterSpacing(QFont::AbsoluteSpacing, l.letterSpacing);
  return f;
}
double outlinePad(const Layer &l) { double s = 0; for (auto &o : l.outlines) s += o.width; return s; }

QColor cssColor(const QString &s, double alpha) {
  QColor c(s.trimmed());
  if (!c.isValid()) c = QColor(0, 0, 0);
  c.setAlphaF(std::clamp(c.alphaF() * alpha, 0.0, 1.0));
  return c;
}

TextMetrics measure(const Layer &l) {
  TextMetrics m; QFont f = fontOf(l); QFontMetricsF fm(f);
  m.lines = l.text.split('\n');
  QVector<double> raw; for (auto &s : m.lines) raw.append(fm.horizontalAdvance(s));
  double sx = (l.scaleX != 0 ? l.scaleX : 100) / 100.0, rw = 1; for (double w : raw) rw = std::max(rw, w);
  if (l.maxW > 0 && rw * sx > l.maxW) sx = l.maxW / rw;
  m.sx = sx; for (double w : raw) m.ws.append(w * sx);
  m.adv = l.size * l.lineHeight; m.tw = 1; for (double w : m.ws) m.tw = std::max(m.tw, w);
  m.th = m.adv * m.lines.size(); m.pad = outlinePad(l);
  return m;
}
void fitText(Layer &l) {
  if (l.type != "text" || !l.autoFit) return;
  TextMetrics m = measure(l);
  l.w = std::ceil(m.tw + m.pad * 2); l.h = std::ceil(m.th + m.pad * 2);
}

// canvas の textBaseline="middle" 用: (ascent-descent)/2。Chrome は OS/2 の typo 値を使うので、同じ表を読む(無ければ Qt の値)
static double middleOffset(const QFont &f) {
  static QHash<QString, double> cache;
  QString key = f.family() + (f.bold() ? "|b|" : "|n|") + QString::number(f.pixelSize());
  auto it = cache.find(key); if (it != cache.end()) return it.value();
  QFontMetricsF fm(f); double r = (fm.ascent() - fm.descent()) / 2;
  QRawFont rf = QRawFont::fromFont(f);
  if (rf.isValid()) {
    QByteArray os2 = rf.fontTable("OS/2"), head = rf.fontTable("head");
    if (os2.size() >= 72 && head.size() >= 20) {
      auto i16 = [](const QByteArray &b, int o) { return qint16((quint8(b[o]) << 8) | quint8(b[o + 1])); };
      double upm = quint16((quint8(head[18]) << 8) | quint8(head[19])), ta = i16(os2, 68), td = i16(os2, 70);
      if (upm > 0 && ta - td > 0) r = (ta + td) / 2.0 * f.pixelSize() / upm;
    }
  }
  cache.insert(key, r); return r;
}

static void drawText(QPainter &p, const Layer &l) {
  TextMetrics m = measure(l); QFont f = fontOf(l); QVector<double> ys; double top = l.valign == "top" ? -l.h / 2 + m.pad : l.valign == "bottom" ? l.h / 2 - m.pad - m.th : -m.th / 2;
  for (int i = 0; i < m.lines.size(); i++) ys.append(top + m.adv * (i + 0.5));
  double txr = l.align == "left" ? -l.w / 2 + m.pad : l.align == "right" ? l.w / 2 - m.pad : 0;
  if (m.sx != 1) p.scale(m.sx, 1);
  double tx = txr / m.sx;
  double mid = middleOffset(f);
  QVector<QPainterPath> paths;
  for (int i = 0; i < m.lines.size(); i++) {
    double raw = m.ws[i] / m.sx, start = l.align == "left" ? tx : l.align == "right" ? tx - raw : tx - raw / 2;
    QPainterPath pp; pp.setFillRule(Qt::WindingFill);
    double by = ys[i] + mid;
    { double dy = p.worldTransform().map(QPointF(0, by)).y(); by += std::round(dy) - dy; }  // Skia は水平な文字のベースラインを整数 px に置く
    static int xq = 4;
    double bx = start;
    { double dx = p.worldTransform().map(QPointF(start, 0)).x(); double q = xq; bx += std::round(dx * q) / q - dx; }
    pp.addText(QPointF(bx, by), f, m.lines[i]);
    paths.append(pp);
  }
  p.setRenderHint(QPainter::Antialiasing, true);
  // 擬似太字: Skia は size に応じて 1/24(9px以下)〜1/32(36px以上)だけ輪郭を太らせる
  double fb = 0;
  if (fakeBold(l)) { double t = std::clamp((l.size - 9) / 27.0, 0.0, 1.0); fb = l.size * ((1 - t) / 24.0 + t / 32.0); }
  int n = l.outlines.size();
  for (int k = 0; k < n; k++) {
    double w = 0; for (int j = k; j < n; j++) w += l.outlines[j].width;
    if (w <= 0) continue;
    QPen pen(cssColor(l.outlines[k].color), w * 2 + fb, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    for (auto &pp : paths) p.strokePath(pp, pen);
  }
  static double embK = qEnvironmentVariableIsSet("VIMG_EMB") ? qgetenv("VIMG_EMB").toDouble() : 0.4;
  auto lum = [](const QColor &c) { return 0.299 * c.redF() + 0.587 * c.greenF() + 0.114 * c.blueF(); };
  for (int i = 0; i < paths.size(); i++) {
    // Skia の文字は明るい色ほど太って見える(コントラスト補正)。塗りの明るさに比例して、ごく細い縁を足して合わせる
    double lm = l.hasGradient ? (lum(cssColor(l.gradTop)) + lum(cssColor(l.gradBottom))) / 2 : lum(cssColor(l.color));
    double emb = embK * lm + fb;
    if (l.hasGradient) {
      QLinearGradient g(0, ys[i] - l.size / 2, 0, ys[i] + l.size / 2);
      g.setColorAt(0, cssColor(l.gradTop)); g.setColorAt(1, cssColor(l.gradBottom));
      if (emb > 0.01) p.strokePath(paths[i], QPen(QBrush(g), emb, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.fillPath(paths[i], QBrush(g));
    } else {
      if (emb > 0.01) p.strokePath(paths[i], QPen(cssColor(l.color), emb, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.fillPath(paths[i], cssColor(l.color));
    }
  }
}

static void drawShape(QPainter &p, const Layer &l) {
  QPainterPath pp;
  if (l.shape == "ellipse") pp.addEllipse(QRectF(-l.w / 2, -l.h / 2, l.w, l.h));
  else if (l.shape == "roundrect") {
    double r = std::max(0.0, std::min({l.radius, l.w / 2, l.h / 2}));
    if (r <= 0) pp.addRect(QRectF(-l.w / 2, -l.h / 2, l.w, l.h));
    else pp.addRoundedRect(QRectF(-l.w / 2, -l.h / 2, l.w, l.h), r, r);
  } else pp.addRect(QRectF(-l.w / 2, -l.h / 2, l.w, l.h));
  p.setRenderHint(QPainter::Antialiasing, true);
  if (l.hasFill) p.fillPath(pp, cssColor(l.fill));
  if (l.hasStroke && l.stroke.width > 0) p.strokePath(pp, QPen(cssColor(l.stroke.color), l.stroke.width, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
}

// 文字・図形は別の画像にいったん描く(不透明度や影が縁ごとに重ならないように)
static QImage rasterOf(const Layer &l) {
  static QHash<QByteArray, QImage> cache;
  QJsonObject o = l.toJson();
  for (const char *k : {"x", "y", "rot", "opacity", "shadow", "name", "id", "visible", "locked", "export"}) o.remove(k);
  QByteArray key = QJsonDocument(o).toJson(QJsonDocument::Compact) + QByteArray::number(l.w, 'f', 2) + QByteArray::number(l.h, 'f', 2);
  auto it = cache.find(key); if (it != cache.end()) return it.value();
  double m = (l.type == "text" ? outlinePad(l) + l.size * 0.2 : (l.hasStroke ? l.stroke.width : 0) / 2) + 4;
  int cw = std::max(1, int(std::ceil(l.w + m * 2))), ch = std::max(1, int(std::ceil(l.h + m * 2)));
  QImage img(cw, ch, QImage::Format_ARGB32_Premultiplied); img.fill(Qt::transparent);
  QPainter p(&img); p.translate(cw / 2.0, ch / 2.0);
  if (l.type == "text") drawText(p, l); else drawShape(p, l);
  p.end();
  if (cache.size() > 200) cache.clear();
  cache.insert(key, img); return img;
}

QImage cachedImage(const QString &path) {
  struct E { QDateTime t; qint64 sz; QImage im; };
  static QHash<QString, E> cache;
  QFileInfo fi(path); auto it = cache.find(path);
  if (it != cache.end() && it->t == fi.lastModified() && it->sz == fi.size()) return it->im;
  QImage im = loadImage(path);
  if (!im.isNull()) im = im.convertToFormat(QImage::Format_ARGB32);
  cache.insert(path, {fi.lastModified(), fi.size(), im}); return im;
}

static QImage adjusted(const QImage &src, const Adjust &a) {  // CSS filter: brightness → contrast → saturate(sRGB)
  QImage im = src.convertToFormat(QImage::Format_ARGB32);
  double b = 1 + a.brightness / 100, c = 1 + a.contrast / 100, s = 1 + a.saturation / 100;
  auto cl = [](double v) { return v < 0 ? 0.0 : v > 1 ? 1.0 : v; };
  double m[9] = {0.213 + 0.787 * s, 0.715 - 0.715 * s, 0.072 - 0.072 * s,
                 0.213 - 0.213 * s, 0.715 + 0.285 * s, 0.072 - 0.072 * s,
                 0.213 - 0.213 * s, 0.715 - 0.715 * s, 0.072 + 0.928 * s};
  for (int y = 0; y < im.height(); y++) {
    QRgb *px = reinterpret_cast<QRgb *>(im.scanLine(y));
    for (int x = 0; x < im.width(); x++) {
      double r = qRed(px[x]) / 255.0, g = qGreen(px[x]) / 255.0, bl = qBlue(px[x]) / 255.0;
      r = cl((cl(r * b) - 0.5) * c + 0.5); g = cl((cl(g * b) - 0.5) * c + 0.5); bl = cl((cl(bl * b) - 0.5) * c + 0.5);
      double R = cl(m[0] * r + m[1] * g + m[2] * bl), G = cl(m[3] * r + m[4] * g + m[5] * bl), B = cl(m[6] * r + m[7] * g + m[8] * bl);
      px[x] = qRgba(int(std::lround(R * 255)), int(std::lround(G * 255)), int(std::lround(B * 255)), qAlpha(px[x]));
    }
  }
  return im;
}

// 透明度だけをぼかす(ガウス。canvas の shadowBlur は σ = blur/2)
static QVector<float> blurAlpha(const QImage &img, double sigma) {
  int w = img.width(), h = img.height();
  QVector<float> a(w * h);
  for (int y = 0; y < h; y++) { const QRgb *px = reinterpret_cast<const QRgb *>(img.constScanLine(y)); for (int x = 0; x < w; x++) a[y * w + x] = qAlpha(px[x]) / 255.0f; }
  if (sigma < 0.3) return a;
  int r = std::max(1, int(std::ceil(sigma * 3)));
  QVector<float> k(2 * r + 1); float sum = 0;
  for (int i = -r; i <= r; i++) { k[i + r] = std::exp(-(i * i) / (2 * sigma * sigma)); sum += k[i + r]; }
  for (auto &v : k) v /= sum;
  QVector<float> t(w * h);
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    float s = 0; for (int i = -r; i <= r; i++) { int xx = x + i; if (xx >= 0 && xx < w) s += a[y * w + xx] * k[i + r]; }
    t[y * w + x] = s;
  }
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    float s = 0; for (int i = -r; i <= r; i++) { int yy = y + i; if (yy >= 0 && yy < h) s += t[yy * w + x] * k[i + r]; }
    a[y * w + x] = s;
  }
  return a;
}

static bool drawLayer(QPainter &dst, Layer &l, const QString &baseDir, QStringList *missing) {
  if (!l.visible || l.opacity <= 0) return true;
  fitText(l);
  QImage content; QRectF rect;   // ローカル座標(中心原点)の描画先
  bool flip = false;
  if (l.type == "image") {
    QImage im = cachedImage(resolveSrc(baseDir, l.src));
    if (im.isNull()) { if (missing) missing->append(l.src); return false; }
    if (l.adjust.any()) im = adjusted(im, l.adjust);
    // 大きな縮小は段階的に(canvas の drawImage に近づける)
    double k = std::min(l.w / im.width(), l.h / im.height());
    if (k < 0.5) im = im.scaled(std::max(1, int(std::lround(l.w))), std::max(1, int(std::lround(l.h))), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    content = im.convertToFormat(QImage::Format_ARGB32_Premultiplied); rect = QRectF(-l.w / 2, -l.h / 2, l.w, l.h); flip = l.flip;
  } else {
    content = rasterOf(l); rect = QRectF(-content.width() / 2.0, -content.height() / 2.0, content.width(), content.height());
  }
  QTransform T; T.translate(l.x, l.y); if (l.rot) T.rotate(l.rot);
  if (flip) T.scale(-1, 1);
  QRectF bb = T.mapRect(rect);
  double sig = l.hasShadow ? l.shadow.blur / 2 : 0;
  QRectF area = bb;
  if (l.hasShadow) area = bb.united(bb.translated(l.shadow.x, l.shadow.y)).adjusted(-sig * 3 - 2, -sig * 3 - 2, sig * 3 + 2, sig * 3 + 2);
  QRect ar = area.adjusted(-2, -2, 2, 2).toAlignedRect().intersected(QRect(0, 0, dst.device()->width(), dst.device()->height()));
  if (ar.isEmpty()) return true;
  QImage S(ar.size(), QImage::Format_ARGB32_Premultiplied); S.fill(Qt::transparent);
  if (l.type != "image" && !l.rot) {
    // 平行移動だけのとき、canvas(Skia)は小数位置の drawImage を双一次で補間する。Qt は整数に丸めるので自前で同じ補間をする
    QPointF tl = T.map(rect.topLeft()) - ar.topLeft();
    int ix = int(std::floor(tl.x())), iy = int(std::floor(tl.y())); double fx = tl.x() - ix, fy = tl.y() - iy;
    QVector<float> acc(size_t(S.width()) * S.height() * 4, 0.f);
    for (int sy = 0; sy < content.height(); sy++) {
      const QRgb *px = reinterpret_cast<const QRgb *>(content.constScanLine(sy));
      for (int sx = 0; sx < content.width(); sx++) {
        QRgb v = px[sx]; if (!qAlpha(v)) continue;
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
          int X = ix + sx + dx, Y = iy + sy + dy; if (X < 0 || Y < 0 || X >= S.width() || Y >= S.height()) continue;
          float w = float((dx ? fx : 1 - fx) * (dy ? fy : 1 - fy)); if (w <= 0) continue;
          float *a = &acc[(size_t(Y) * S.width() + X) * 4];
          a[0] += w * qAlpha(v); a[1] += w * qRed(v); a[2] += w * qGreen(v); a[3] += w * qBlue(v);
        }
      }
    }
    for (int y = 0; y < S.height(); y++) { QRgb *o = reinterpret_cast<QRgb *>(S.scanLine(y));
      for (int x = 0; x < S.width(); x++) { const float *a = &acc[(size_t(y) * S.width() + x) * 4];
        o[x] = qRgba(int(std::lround(a[1])), int(std::lround(a[2])), int(std::lround(a[3])), int(std::lround(a[0]))); } }
  } else { QPainter p(&S); p.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing, true);
    p.translate(-ar.topLeft()); p.setTransform(T, true);
    p.drawImage(rect, content); }
  dst.save(); dst.setOpacity(std::clamp(l.opacity, 0.0, 1.0));
  if (l.hasShadow) {
    // 影は、影を付けない状態で描いた像の透明度から作る。範囲外にはみ出す分は ar に含めてある
    QImage Sfull = S;
    QVector<float> a = blurAlpha(Sfull, sig);
    QColor c = cssColor(l.shadow.color, l.shadow.opacity);
    QImage sh(S.size(), QImage::Format_ARGB32_Premultiplied);
    int w = S.width(), h = S.height();
    double ca = c.alphaF();
    for (int y = 0; y < h; y++) { QRgb *px = reinterpret_cast<QRgb *>(sh.scanLine(y));
      for (int x = 0; x < w; x++) { float al = a[y * w + x] * float(ca);
        px[x] = qRgba(int(std::lround(c.red() * al)), int(std::lround(c.green() * al)), int(std::lround(c.blue() * al)), int(std::lround(al * 255))); } }
    dst.drawImage(QPointF(ar.x() + l.shadow.x, ar.y() + l.shadow.y), sh);
  }
  dst.drawImage(ar.topLeft(), S);
  dst.restore();
  return true;
}

QImage renderDoc(Doc &d, const QString &baseDir, bool forExport, QStringList *missing) {
  QImage out(std::max(1, d.width), std::max(1, d.height), QImage::Format_ARGB32_Premultiplied);
  out.fill(Qt::transparent);
  QPainter p(&out);
  if (!d.background.isEmpty()) p.fillRect(out.rect(), cssColor(d.background));
  for (auto &l : d.layers) if (!(forExport && !l.exportIt)) drawLayer(p, l, baseDir, missing);
  p.end(); return out;
}

bool writePng(const QImage &img, const QString &path, QString *err) {
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly)) { if (err) *err = "書き込めません: " + path; return false; }
  if (!img.convertToFormat(QImage::Format_ARGB32).save(&f, "PNG")) { if (err) *err = "PNG を書けません"; return false; }
  if (!f.commit()) { if (err) *err = "保存に失敗: " + path; return false; }
  return true;
}
}
