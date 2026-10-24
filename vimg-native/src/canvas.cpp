#include "mainwindow.h"
#include <QPainter>
#include <QKeyEvent>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QTimer>
#include <QFileInfo>
#include <QTextOption>
#include <cmath>

static double r1(double v) { return std::round(v * 10) / 10; }
static const double ROT_GAP = 28;
static const int HANDLES[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}};

Canvas::Canvas(MainWindow *m) : QWidget(m), mw(m) {
  setMouseTracking(true); setAcceptDrops(true); setFocusPolicy(Qt::StrongFocus); setMinimumSize(200, 200);
}

QPointF Canvas::lpt(const Layer &l, double lx, double ly) {
  double r = l.rot * M_PI / 180, c = std::cos(r), s = std::sin(r);
  return QPointF(l.x + lx * c - ly * s, l.y + lx * s + ly * c);
}
QPointF Canvas::toLocal(const Layer &l, double x, double y) {
  double r = -l.rot * M_PI / 180, c = std::cos(r), s = std::sin(r), dx = x - l.x, dy = y - l.y;
  return QPointF(dx * c - dy * s, dx * s + dy * c);
}
QPointF Canvas::extent(const Layer &l) {
  double r = l.rot * M_PI / 180, c = std::fabs(std::cos(r)), s = std::fabs(std::sin(r));
  return QPointF(c * l.w / 2 + s * l.h / 2, s * l.w / 2 + c * l.h / 2);
}

Layer *Canvas::hitLayer(double x, double y) {
  for (int i = mw->doc.layers.size() - 1; i >= 0; i--) {
    Layer &l = mw->doc.layers[i];
    if (!l.visible || l.locked) continue;
    fitText(l);
    QPointF p = toLocal(l, x, y);
    if (std::fabs(p.x()) <= l.w / 2 && std::fabs(p.y()) <= l.h / 2) return &l;
  }
  return nullptr;
}

void Canvas::invalidate() { cacheOk = false; update(); }

void Canvas::fitView() {
  autoFit = true;
  double W = mw->doc.width, H = mw->doc.height;
  if (width() < 60 || height() < 60 || W < 1 || H < 1) { zoom = 0.5; origin = QPointF(20, 20); }
  else {
    zoom = std::min((width() - 48) / W, (height() - 48) / H);
    zoom = std::max(0.02, std::min(8.0, zoom));
    origin = QPointF((width() - W * zoom) / 2, (height() - H * zoom) / 2);
  }
  mw->stZoom->setText(QString("%1%").arg(int(std::round(zoom * 100)))); mw->stSize->setText(QString("%1 × %2").arg(mw->doc.width).arg(mw->doc.height));
  update();
}

void Canvas::zoomAt(double f, QPointF at) {
  if (at.x() < 0) at = QPointF(width() / 2.0, height() / 2.0);
  double z2 = std::max(0.02, std::min(32.0, zoom * f));
  origin = at - (at - origin) * (z2 / zoom); zoom = z2; autoFit = false;
  mw->stZoom->setText(QString("%1%").arg(int(std::round(zoom * 100)))); update();
}

void Canvas::resizeEvent(QResizeEvent *) { if (autoFit) fitView(); }

void Canvas::setTool(Tool t) {
  finishEdit();
  if (tool == Crop && t != Crop) hasCrop = false;
  tool = t;
  if (t == Crop && !hasCrop) { hasCrop = true; crop = QRect(0, 0, mw->doc.width, mw->doc.height); }
  emit toolChanged(); update();
}

void Canvas::applyCrop() {
  if (!hasCrop || crop.width() < 2 || crop.height() < 2) { mw->toast(QString::fromUtf8("ドラッグで範囲を選んでください")); return; }
  QRect c = crop;
  mw->snap();
  for (auto &l : mw->doc.layers) { l.x = r1(l.x - c.x()); l.y = r1(l.y - c.y()); }
  mw->doc.width = c.width(); mw->doc.height = c.height();
  hasCrop = false; setTool(Move); fitView(); mw->commit();
}

void Canvas::ensureRender() {
  if (cacheOk) return;
  cache = vimg::renderDoc(mw->doc, mw->baseDir, false, nullptr); cacheOk = true;
}

// ---------------------------------------------------------------- 描画
void Canvas::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor("#101013"));
  QRectF dr(origin, QSizeF(mw->doc.width * zoom, mw->doc.height * zoom));
  static QPixmap tile;
  if (tile.isNull()) {
    tile = QPixmap(32, 32); QPainter t(&tile); t.fillRect(0, 0, 32, 32, QColor("#2c2c31"));
    t.fillRect(0, 0, 16, 16, QColor("#3a3a40")); t.fillRect(16, 16, 16, 16, QColor("#3a3a40"));
  }
  p.save(); p.setClipRect(dr); p.setBrushOrigin(dr.topLeft()); p.fillRect(dr, QBrush(tile)); p.restore();
  ensureRender();
  p.setRenderHint(QPainter::SmoothPixmapTransform, true);
  p.drawImage(dr, cache);
  p.setPen(QColor("#3a3a43")); p.setBrush(Qt::NoBrush); p.drawRect(dr.adjusted(-0.5, -0.5, 0.5, 0.5));
  drawOverlay(p);
}

void Canvas::drawOverlay(QPainter &p) {
  p.setRenderHint(QPainter::Antialiasing, true);
  if (tool == Crop && hasCrop) {
    QRectF cr(toScr(crop.topLeft()), QSizeF(crop.width() * zoom, crop.height() * zoom));
    QPainterPath out; out.addRect(rect()); QPainterPath in; in.addRect(cr);
    p.fillPath(out.subtracted(in), QColor(0, 0, 0, 140));
    p.setPen(QPen(Qt::white, 1)); p.setBrush(Qt::NoBrush); p.drawRect(cr);
    return;
  }
  Layer *l = mw->cur();
  if (l && l->visible) {
    fitText(*l);
    QPolygonF poly;
    for (auto pt : {QPointF(-1, -1), QPointF(1, -1), QPointF(1, 1), QPointF(-1, 1)}) poly << toScr(lpt(*l, pt.x() * l->w / 2, pt.y() * l->h / 2));
    QPen pen(QColor(l->locked ? "#f5a524" : "#3b82f6"), 1); if (l->locked) pen.setStyle(Qt::DashLine);
    p.setPen(pen); p.setBrush(Qt::NoBrush); p.drawPolygon(poly);
    if (!l->locked && tool == Move && !ed) {
      p.setPen(QPen(QColor("#3b82f6"), 1));
      QPointF t = toScr(lpt(*l, 0, -l->h / 2)), r = toScr(lpt(*l, 0, -l->h / 2 - ROT_GAP / zoom));
      p.drawLine(t, r); p.setBrush(Qt::white); p.drawEllipse(r, 5, 5);
      for (auto &h : HANDLES) {
        QPointF pt = toScr(lpt(*l, h[0] * l->w / 2, h[1] * l->h / 2));
        p.drawRect(QRectF(pt.x() - 4, pt.y() - 4, 8, 8));
      }
    }
  }
  p.setPen(QPen(QColor("#ff3ec8"), 1));
  for (auto &g : guides) {
    if (g.first) { double x = std::round(toScr(QPointF(g.second, 0)).x()) + 0.5; p.drawLine(QPointF(x, 0), QPointF(x, height())); }
    else { double y = std::round(toScr(QPointF(0, g.second)).y()) + 0.5; p.drawLine(QPointF(0, y), QPointF(width(), y)); }
  }
}

Canvas::Handle Canvas::handleAt(const Layer *l, QPointF s) const {
  Handle h;
  if (!l || l->locked || !l->visible) return h;
  QPointF r = toScr(lpt(*l, 0, -l->h / 2 - ROT_GAP / zoom));
  if (std::hypot(r.x() - s.x(), r.y() - s.y()) < 9) { h.rot = true; h.ok = true; return h; }
  for (auto &hh : HANDLES) {
    QPointF p = toScr(lpt(*l, hh[0] * l->w / 2, hh[1] * l->h / 2));
    if (std::fabs(p.x() - s.x()) < 7 && std::fabs(p.y() - s.y()) < 7) { h.hx = hh[0]; h.hy = hh[1]; h.ok = true; return h; }
  }
  return h;
}

QPointF Canvas::snapPos(const Layer &l, double nx, double ny) {
  guides.clear();
  if (!snapOn) return QPointF(nx, ny);
  QPointF e = extent(l); double th = 6 / zoom;
  QList<double> tx{0, mw->doc.width / 2.0, double(mw->doc.width)}, ty{0, mw->doc.height / 2.0, double(mw->doc.height)};
  for (auto &o : mw->doc.layers) if (o.id != l.id && o.visible && o.exportIt) {
    QPointF oe = extent(o); tx << o.x - oe.x() << o.x << o.x + oe.x(); ty << o.y - oe.y() << o.y << o.y + oe.y();
  }
  auto best = [&](double pos, double ex, const QList<double> &ts, bool &found, double &t) {
    double bd = 0; found = false;
    for (double tt : ts) for (double d : {-ex, 0.0, ex}) {
      double dd = tt - (pos + d);
      if (std::fabs(dd) < th && (!found || std::fabs(dd) < std::fabs(bd))) { found = true; bd = dd; t = tt; }
    }
    return bd;
  };
  bool fx, fy; double tX = 0, tY = 0;
  double dx = best(nx, e.x(), tx, fx, tX), dy = best(ny, e.y(), ty, fy, tY);
  if (fx) { nx += dx; guides.append({true, tX}); }
  if (fy) { ny += dy; guides.append({false, tY}); }
  return QPointF(nx, ny);
}

// ---------------------------------------------------------------- マウス
void Canvas::mousePressEvent(QMouseEvent *e) {
  setFocus();
  QPointF s = e->position();
  if (ed) { finishEdit(); if (tool == Text) return; }
  if (e->button() == Qt::MiddleButton || spaceDown || tool == Hand) {
    mode = Pan; s0 = s; pan0 = origin; setCursor(Qt::ClosedHandCursor); autoFit = false; return;
  }
  if (e->button() != Qt::LeftButton) return;
  QPointF d = toDoc(s);
  if (tool == Crop) { mode = Cropping; crop0 = d; return; }
  if (tool == Shape) { mode = Drawing; p0 = d; s0 = s; dragId.clear(); return; }
  if (tool == Text) {
    Layer *l = hitLayer(d.x(), d.y());
    if (l && l->type == "text") { QString id = l->id; mw->select(id); startEdit(id, false); }
    else mw->newText(d.x(), d.y());
    return;
  }
  Layer *c = mw->cur(); Handle h = handleAt(c, s);
  if (h.ok) {
    mode = h.rot ? Rotating : Handling; hd = h; o0 = *c; dragId = c->id; mw->snap(); p0 = d; return;
  }
  Layer *l = hitLayer(d.x(), d.y());
  if (!l) { mw->select(QString()); return; }
  if (l->id != mw->sel) mw->select(l->id);
  if (e->modifiers() & Qt::AltModifier) mw->dupLayer();
  l = mw->cur(); if (!l) return;
  dragId = l->id; o0 = *l; p0 = d; s0 = s; moved = false; mode = Moving;
}

void Canvas::mouseMoveEvent(QMouseEvent *e) {
  QPointF s = e->position(), d = toDoc(s);
  mw->stPos->setText(QString("x %1  y %2").arg(int(std::round(d.x()))).arg(int(std::round(d.y()))));
  bool shift = e->modifiers() & Qt::ShiftModifier, alt = e->modifiers() & Qt::AltModifier;
  if (!(e->buttons() & Qt::LeftButton) && mode != Pan) {
    Qt::CursorShape c = Qt::ArrowCursor;
    if (spaceDown || tool == Hand) c = Qt::OpenHandCursor;
    else if (tool == Text) c = Qt::IBeamCursor;
    else if (tool == Shape || tool == Crop) c = Qt::CrossCursor;
    else if (tool == Move) {
      Layer *l = mw->cur(); Handle h = handleAt(l, s);
      if (h.ok) {
        if (h.rot) c = Qt::CrossCursor;
        else {
          double ang = std::atan2(h.hy, h.hx) * 180 / M_PI + l->rot;
          int i = int(std::round(std::fmod(std::fmod(ang, 180) + 180, 180) / 45)) % 4;
          c = i == 0 ? Qt::SizeHorCursor : i == 1 ? Qt::SizeFDiagCursor : i == 2 ? Qt::SizeVerCursor : Qt::SizeBDiagCursor;
        }
      } else if (hitLayer(d.x(), d.y())) c = Qt::SizeAllCursor;
    }
    setCursor(c);
    return;
  }
  switch (mode) {
  case Pan: origin = pan0 + (s - s0); update(); break;
  case Moving: {
    Layer *l = mw->doc.find(dragId); if (!l) break;
    if (!moved) { if (std::hypot(s.x() - s0.x(), s.y() - s0.y()) < 3) break; mw->snap(); moved = true; }
    double dx = d.x() - p0.x(), dy = d.y() - p0.y();
    if (shift) { if (std::fabs(dx) > std::fabs(dy)) dy = 0; else dx = 0; }
    QPointF n = snapPos(*l, o0.x + dx, o0.y + dy);
    l->x = r1(n.x()); l->y = r1(n.y()); invalidate(); break; }
  case Rotating: {
    Layer *l = mw->doc.find(dragId); if (!l) break;
    double a0 = std::atan2(p0.y() - o0.y, p0.x() - o0.x);
    double r = o0.rot + (std::atan2(d.y() - o0.y, d.x() - o0.x) - a0) * 180 / M_PI;
    if (shift) r = std::round(r / 15) * 15;
    r = std::fmod(std::fmod(r + 180, 360) + 360, 360) - 180;
    l->rot = r1(r); invalidate(); break; }
  case Handling: {
    Layer *l = mw->doc.find(dragId); if (!l) break;
    QPointF lp = toLocal(o0, d.x(), d.y());
    int hx = hd.hx, hy = hd.hy;
    double ax = alt ? 0 : -hx * o0.w / 2, ay = alt ? 0 : -hy * o0.h / 2, nw = o0.w, nh = o0.h;
    if (hx) nw = std::max(2.0, (lp.x() - ax) * hx * (alt ? 2 : 1));
    if (hy) nh = std::max(2.0, (lp.y() - ay) * hy * (alt ? 2 : 1));
    bool prop = hx && hy && (l->type == "image" ? !shift : (shift || l->type == "text"));
    if (prop) { double k = std::max(nw / o0.w, nh / o0.h); nw = o0.w * k; nh = o0.h * k; }
    double cx = (alt || !hx) ? 0 : ax + hx * nw / 2, cy = (alt || !hy) ? 0 : ay + hy * nh / 2;
    QPointF n = lpt(o0, cx, cy);
    l->x = r1(n.x()); l->y = r1(n.y()); l->w = r1(nw); l->h = r1(nh);
    if (l->type == "text") { if (prop) l->size = std::max(1.0, r1(o0.size * nw / o0.w)); else l->autoFit = false; }
    invalidate(); break; }
  case Drawing: {
    Layer *l = dragId.isEmpty() ? nullptr : mw->doc.find(dragId);
    if (!l) {
      if (std::hypot(s.x() - s0.x(), s.y() - s0.y()) < 4) break;
      l = mw->newShape(p0.x(), p0.y(), 1, 1); if (!l) break; dragId = l->id;
    }
    double w = d.x() - p0.x(), h = d.y() - p0.y();
    if (shift) { double m = std::max(std::fabs(w), std::fabs(h)); w = (w < 0 ? -1 : 1) * m; h = (h < 0 ? -1 : 1) * m; }
    l->x = r1(p0.x() + w / 2); l->y = r1(p0.y() + h / 2); l->w = r1(std::max(1.0, std::fabs(w))); l->h = r1(std::max(1.0, std::fabs(h)));
    invalidate(); break; }
  case Cropping: {
    double px = d.x(), py = d.y();
    if (shift) { double m = std::max(std::fabs(px - crop0.x()), std::fabs(py - crop0.y())); px = crop0.x() + ((px - crop0.x()) < 0 ? -1 : 1) * m; py = crop0.y() + ((py - crop0.y()) < 0 ? -1 : 1) * m; }
    crop = QRect(int(std::round(std::min(crop0.x(), px))), int(std::round(std::min(crop0.y(), py))),
                 int(std::round(std::fabs(px - crop0.x()))), int(std::round(std::fabs(py - crop0.y()))));
    hasCrop = true; update(); mw->renderOpts(); break; }
  default: break;
  }
}

void Canvas::mouseReleaseEvent(QMouseEvent *) {
  Mode m = mode; mode = None;
  switch (m) {
  case Pan: unsetCursor(); break;
  case Moving: guides.clear(); if (moved) mw->commit(); else update(); break;
  case Rotating: case Handling: mw->commit(); break;
  case Drawing: if (dragId.isEmpty()) mw->newShape(p0.x(), p0.y()); else mw->commit(); dragId.clear(); break;
  default: break;
  }
}

void Canvas::mouseDoubleClickEvent(QMouseEvent *e) {
  QPointF d = toDoc(e->position());
  Layer *l = hitLayer(d.x(), d.y());
  if (l && l->type == "text") { QString id = l->id; mw->select(id); startEdit(id, false); }
}

void Canvas::wheelEvent(QWheelEvent *e) {
  double dy = e->angleDelta().y(), dx = e->angleDelta().x();
  if (e->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) zoomAt(dy > 0 ? 1.15 : 1 / 1.15, e->position());
  else if (e->modifiers() & Qt::ShiftModifier) { origin.rx() += dy; autoFit = false; update(); }
  else { origin += QPointF(dx, dy); autoFit = false; update(); }
  e->accept();
}

void Canvas::dragEnterEvent(QDragEnterEvent *e) { if (e->mimeData()->hasUrls()) e->acceptProposedAction(); }
void Canvas::dropEvent(QDropEvent *e) {
  QPointF at = toDoc(e->position());
  for (const QUrl &u : e->mimeData()->urls()) {
    if (!u.isLocalFile()) continue;
    QString suf = QFileInfo(u.toLocalFile()).suffix().toLower();
    if (QStringList{"png", "jpg", "jpeg", "webp", "gif", "bmp"}.contains(suf)) mw->addImage(u.toLocalFile(), at);
  }
}

// ---------------------------------------------------------------- 文字の直接編集
void Canvas::startEdit(const QString &id, bool selectAll) {
  Layer *l = mw->doc.find(id);
  if (!l || l->type != "text") return;
  finishEdit();
  fitText(*l);
  edId = id; edOrig = l->text; edSnapped = false; mw->sel = id; mw->lastTextStyle = l->toJson();
  ed = new QPlainTextEdit(this);
  QFont f = vimg::fontOf(*l); f.setPixelSize(std::max(1, int(l->size * zoom))); ed->setFont(f);
  ed->setStyleSheet("QPlainTextEdit{background:rgba(0,0,0,140);color:#fff;border:1px dashed #3b82f6;border-radius:0;padding:2px}");
  ed->setFrameShape(QFrame::NoFrame);
  ed->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); ed->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  ed->setLineWrapMode(QPlainTextEdit::NoWrap);
  QTextOption to; to.setAlignment(l->align == "left" ? Qt::AlignLeft : l->align == "right" ? Qt::AlignRight : Qt::AlignHCenter);
  ed->document()->setDefaultTextOption(to);
  ed->setPlainText(l->text);
  QPointF ex = extent(*l), tl = toScr(QPointF(l->x - ex.x(), l->y - ex.y()));
  ed->setGeometry(int(tl.x()), int(tl.y()), std::max(40, int(ex.x() * 2 * zoom)), std::max(20, int(ex.y() * 2 * zoom)));
  ed->installEventFilter(this);
  connect(ed, &QPlainTextEdit::textChanged, this, [this] {
    Layer *l = mw->doc.find(edId); if (!l || !ed) return;
    QString t = ed->toPlainText(); if (t == l->text) return;
    if (!edSnapped) { mw->snap(); edSnapped = true; }
    l->text = t; fitText(*l); mw->commit(false);
  });
  ed->show(); ed->setFocus();
  if (selectAll) ed->selectAll();
  mw->list->sync(); mw->props->rebuild(); update();
}

void Canvas::finishEdit() {
  if (!ed) return;
  QPlainTextEdit *e = ed; ed = nullptr;
  bool changed = edSnapped; edSnapped = false;
  Layer *l = mw->doc.find(edId);
  e->removeEventFilter(this); e->hide(); e->deleteLater();
  setFocus();
  if (!l) return;
  if (l->text.trimmed().isEmpty()) {
    int i = mw->doc.indexOf(edId); mw->doc.layers.removeAt(i);
    if (mw->sel == edId) mw->sel.clear();
    mw->commit(); return;
  }
  if (!changed) { update(); return; }
  QString oldFirst = edOrig.split('\n').value(0).left(20);
  if (l->name.isEmpty() || l->name == QString::fromUtf8("テキスト") || l->name == oldFirst) l->name = l->text.split('\n').value(0).left(20);
  mw->lastTextStyle = l->toJson();
  mw->commit();
}

bool Canvas::eventFilter(QObject *o, QEvent *ev) {
  if (o == ed && ed) {
    if (ev->type() == QEvent::KeyPress) {
      auto *k = static_cast<QKeyEvent *>(ev);
      if (k->key() == Qt::Key_Escape || ((k->modifiers() & Qt::ControlModifier) && (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter))) {
        QTimer::singleShot(0, this, [this] { finishEdit(); }); return true;
      }
    } else if (ev->type() == QEvent::FocusOut) {
      QTimer::singleShot(0, this, [this] { if (ed && !ed->hasFocus()) finishEdit(); });
    }
  }
  return QWidget::eventFilter(o, ev);
}
