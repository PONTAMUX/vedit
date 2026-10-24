#include "mainwindow.h"
#include <QPainter>
#include <QMouseEvent>
#include <QInputDialog>
#include <QHBoxLayout>
#include <QDoubleSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QSlider>
#include <QColorDialog>
#include <QFontDatabase>
#include <QFileInfo>
#include <cmath>

static double r1(double v) { return std::round(v * 10) / 10; }
static QString U(const char *s) { return QString::fromUtf8(s); }
static const int ROWH = 30;

// ================================================================ レイヤー一覧
LayerList::LayerList(MainWindow *m) : QWidget(nullptr), mw(m) { setMouseTracking(true); }

void LayerList::sync() { setMinimumHeight(std::max(120, int(mw->doc.layers.size()) * ROWH)); update(); }

int LayerList::rowAt(int y) const { return y / ROWH; }

void LayerList::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor("#242429"));
  int n = mw->doc.layers.size();
  if (!n) {
    p.setPen(QColor("#8d8d99")); p.drawText(QRect(10, 10, width() - 20, 60), Qt::TextWordWrap, U("レイヤーがありません。T で文字、U で図形、画像はドラッグ&ドロップ。"));
    return;
  }
  QFont f = font();
  for (int i = 0; i < n; i++) {
    const Layer &l = mw->doc.layers[n - 1 - i];
    QRect r(0, i * ROWH, width(), ROWH);
    if (l.id == mw->sel) p.fillRect(r, QColor("#2b4a7a"));
    p.setPen(QColor("#3a3a43")); p.drawLine(r.bottomLeft(), r.bottomRight());
    p.setOpacity(l.visible ? 0.9 : 0.25); p.setPen(QColor("#e6e6ea"));
    p.drawText(QRect(4, r.y(), 24, ROWH), Qt::AlignCenter, U("👁"));
    p.setOpacity(1); p.setPen(QColor("#8d8d99"));
    p.drawText(QRect(30, r.y(), 20, ROWH), Qt::AlignCenter, l.type == "text" ? "T" : l.type == "shape" ? U("▭") : U("🖼"));
    p.setPen(QColor("#e6e6ea"));
    QString nm = l.name; QFontMetrics fm(f);
    int avail = width() - 58 - 30 - (l.exportIt ? 0 : 76);
    nm = fm.elidedText(nm, Qt::ElideRight, std::max(10, avail));
    p.drawText(QRect(58, r.y(), width() - 88, ROWH), Qt::AlignVCenter | Qt::AlignLeft, nm);
    if (!l.exportIt) {
      p.setPen(QColor("#f5a524")); QFont sf = f; sf.setPixelSize(11); p.setFont(sf);
      p.drawText(QRect(58 + fm.horizontalAdvance(nm) + 4, r.y(), 80, ROWH), Qt::AlignVCenter, U("書き出さない")); p.setFont(f);
    }
    p.setOpacity(l.locked ? 0.9 : 0.25); p.setPen(QColor("#e6e6ea"));
    p.drawText(QRect(width() - 28, r.y(), 24, ROWH), Qt::AlignCenter, U("🔒"));
    p.setOpacity(1);
  }
  if (dropAt >= 0) {   // 並べ替えの挿入線
    p.setPen(QPen(QColor("#3b82f6"), 2)); int y = dropAt * ROWH; p.drawLine(0, y, width(), y);
  }
}

void LayerList::mousePressEvent(QMouseEvent *e) {
  int n = mw->doc.layers.size(), row = rowAt(int(e->position().y()));
  if (row < 0 || row >= n) { mw->select(QString()); return; }
  Layer &l = mw->doc.layers[n - 1 - row];
  QString id = l.id; int x = int(e->position().x());
  press = e->pos(); dragRow = -1;
  if (x < 30) { mw->snap(); l.visible = !l.visible; mw->commit(false); return; }
  if (x > width() - 32) { mw->snap(); l.locked = !l.locked; mw->commit(); return; }
  mw->select(id); dragRow = row;
}

void LayerList::mouseMoveEvent(QMouseEvent *e) {
  if (dragRow < 0 || !(e->buttons() & Qt::LeftButton)) return;
  if ((e->pos() - press).manhattanLength() < 5 && dropAt < 0) return;
  int n = mw->doc.layers.size();
  int y = int(e->position().y()); int row = std::max(0, std::min(n - 1, y / ROWH));
  bool before = y < row * ROWH + ROWH / 2;
  dropAt = before ? row : row + 1; dropRow = row; dropBefore = before;
  update();
}

void LayerList::mouseReleaseEvent(QMouseEvent *) {
  if (dragRow >= 0 && dropAt >= 0) {
    int n = mw->doc.layers.size();
    int srcIdx = n - 1 - dragRow, tgtIdx = n - 1 - dropRow;
    if (srcIdx != tgtIdx) {
      mw->snap();
      Layer src = mw->doc.layers.takeAt(srcIdx);
      int ti = tgtIdx > srcIdx ? tgtIdx - 1 : tgtIdx;
      mw->doc.layers.insert(dropBefore ? ti + 1 : ti, src);
      mw->commit();
    }
  }
  dragRow = -1; dropAt = -1; update();
}

void LayerList::mouseDoubleClickEvent(QMouseEvent *e) {
  int n = mw->doc.layers.size(), row = rowAt(int(e->position().y()));
  if (row < 0 || row >= n || e->position().x() < 56 || mw->silent) return;
  Layer &l = mw->doc.layers[n - 1 - row];
  QString id = l.id; bool ok = false;
  QString t = QInputDialog::getText(this, U("名前を変更"), U("レイヤー名"), QLineEdit::Normal, l.name, &ok);
  if (!ok || t.isEmpty()) return;
  Layer *p = mw->doc.find(id); if (!p || p->name == t) return;
  mw->snap(); p->name = t; mw->commit();
}

// ================================================================ プロパティ
PropsPanel::PropsPanel(MainWindow *m) : QScrollArea(nullptr), mw(m) {
  setWidgetResizable(true); setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void PropsPanel::edit(void *key, const std::function<void(Layer &)> &f, bool structural) {
  Layer *l = mw->cur(); if (!l) return;
  if (!key || lastKey != key) { mw->snap(); lastKey = key; }
  f(*l);
  if (l->type == "text") fitText(*l);
  mw->commit(structural);
  if (structural) lastKey = nullptr;
}

void PropsPanel::rebuild() {
  lastKey = nullptr;
  if (body) { takeWidget(); body->hide(); body->deleteLater(); }
  body = new QWidget; auto *V = new QVBoxLayout(body); V->setContentsMargins(10, 8, 10, 8); V->setSpacing(6);
  setWidget(body);
  using LF = std::function<void(Layer &)>;
  auto h4 = [&](const QString &t) { auto *l = new QLabel(t); l->setProperty("h4", true); V->addWidget(l); };
  auto note = [&](const QString &t) { auto *l = new QLabel(t); l->setProperty("dim", true); l->setWordWrap(true); V->addWidget(l); };
  auto rowOf = [&](const QString &label, QList<QWidget *> ws) {
    auto *w = new QWidget; auto *h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(6);
    if (!label.isNull()) { auto *lb = new QLabel(label); lb->setProperty("dim", true); lb->setFixedWidth(64); h->addWidget(lb); }
    for (auto *x : ws) h->addWidget(x);
    h->addStretch(); V->addWidget(w); return w;
  };
  auto btn = [&](const QString &t, std::function<void()> fn, const QString &tip = QString()) {
    auto *b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); connect(b, &QPushButton::clicked, this, [fn] { fn(); }); return b;
  };
  auto seg = [&](const QList<QPair<QString, QString>> &opts, const QString &curv, std::function<void(Layer &, const QString &)> set, const QString &tip = QString()) {
    auto *w = new QWidget; auto *h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(0);
    for (auto &o : opts) {
      auto *b = new QPushButton(o.second); b->setFocusPolicy(Qt::NoFocus); b->setToolTip(tip); b->setProperty("on", o.first == curv); h->addWidget(b);
      QString v = o.first;
      connect(b, &QPushButton::clicked, this, [this, set, v] { edit(nullptr, [=](Layer &l) { set(l, v); }, true); });
    }
    return w;
  };
  auto num = [&](double v, double step, double mn, double mx, std::function<void(Layer &, double)> set, bool dis = false) {
    auto *s = new QDoubleSpinBox; s->setRange(mn, mx); s->setDecimals(step < 1 ? 2 : 1); s->setSingleStep(step); s->setValue(v); s->setEnabled(!dis);
    s->setFixedWidth(78); s->setKeyboardTracking(true);
    connect(s, &QDoubleSpinBox::valueChanged, this, [this, s, set](double x) { edit(s, [=](Layer &l) { set(l, x); }); });
    return s;
  };
  auto col = [&](const QString &c, std::function<void(Layer &, const QString &)> set) {
    auto *b = new QPushButton; b->setFixedSize(34, 24); b->setFocusPolicy(Qt::NoFocus);
    QColor qc(c); bool ok = qc.isValid();
    b->setStyleSheet(QString("background:%1;border:1px solid #3a3a43;border-radius:4px").arg(ok ? qc.name() : "#000000"));
    connect(b, &QPushButton::clicked, this, [this, c, set, qc] {
      QColor cc = QColorDialog::getColor(qc.isValid() ? qc : QColor(Qt::black), mw);
      if (!cc.isValid()) return;
      QString s = cc.name().toUpper();
      edit(nullptr, [=](Layer &l) { set(l, s); }, true);
    });
    return b;
  };
  auto chk = [&](const QString &t, bool v, std::function<void(Layer &, bool)> set, bool structural = true) {
    auto *c = new QCheckBox(t); c->setChecked(v); c->setFocusPolicy(Qt::NoFocus);
    connect(c, &QCheckBox::toggled, this, [this, set, structural](bool on) { edit(nullptr, [=](Layer &l) { set(l, on); }, structural); });
    return c;
  };

  Layer *lp = mw->cur();
  if (!lp) {   // キャンバス
    h4(U("キャンバス"));
    auto *wi = new QDoubleSpinBox, *hi = new QDoubleSpinBox;
    for (auto *s : {wi, hi}) { s->setRange(1, 16384); s->setDecimals(0); s->setFixedWidth(78); }
    wi->setValue(mw->doc.width); hi->setValue(mw->doc.height);
    rowOf(U("サイズ"), {wi, new QLabel(U("×")), hi, btn(U("変更"), [this, wi, hi] { mw->resizeCanvas(int(wi->value()), int(hi->value())); }, U("中央基準で広げる/狭める"))});
    {
      auto *w = new QWidget; auto *h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4);
      for (auto sz : {QSize(1920, 1080), QSize(1280, 720), QSize(1080, 1920), QSize(1080, 1080)}) {
        auto *b = btn(QString("%1×%2").arg(sz.width()).arg(sz.height()), [this, sz] { mw->resizeCanvas(sz.width(), sz.height()); });
        b->setStyleSheet("padding:3px 5px"); h->addWidget(b);
      }
      V->addWidget(w);
    }
    {
      bool has = !mw->doc.background.isEmpty();
      auto *w = new QWidget; auto *h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(0);
      for (int i = 0; i < 2; i++) {
        auto *b = new QPushButton(i ? U("単色") : U("透明")); b->setFocusPolicy(Qt::NoFocus); b->setProperty("on", has == bool(i)); h->addWidget(b);
        connect(b, &QPushButton::clicked, this, [this, i] { mw->snap(); mw->doc.background = i ? "#FFFFFF" : QString(); mw->commit(); });
      }
      QList<QWidget *> ws{w};
      if (has) {
        auto *b = new QPushButton; b->setFixedSize(34, 24); b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(QString("background:%1;border:1px solid #3a3a43;border-radius:4px").arg(mw->doc.background));
        connect(b, &QPushButton::clicked, this, [this] {
          QColor cc = QColorDialog::getColor(QColor(mw->doc.background), mw); if (!cc.isValid()) return;
          mw->snap(); mw->doc.background = cc.name().toUpper(); mw->commit();
        });
        ws << b;
      }
      rowOf(U("背景"), ws);
    }
    note(U("背景を透明にすると、動画に重ねたとき下が透けます。"));
    h4(U("操作"));
    note(U("V 移動 / T 文字 / U 図形 / C 切り抜き / Ctrl+T 変形 / Ctrl+J 複製 / Delete 削除 / Ctrl+Z・Ctrl+Shift+Z / Ctrl+S 保存(PNGも)"));
    note(U("画像はドラッグ&ドロップか Ctrl+V で追加できます。"));
    V->addStretch(); return;
  }
  Layer L = *lp;   // 表示用の控え(値はウィジェットに入れるだけ。編集は edit() が現在のレイヤーに対して行う)
  bool isT = L.type == "text";
  if (isT) fitText(L);
  h4((L.type == "text" ? U("テキスト") : L.type == "shape" ? U("図形") : U("画像")) + U("レイヤー"));
  { auto *le = new QLineEdit(L.name); connect(le, &QLineEdit::textEdited, this, [this, le](const QString &t) { edit(le, [=](Layer &l) { l.name = t; }); }); rowOf(U("名前"), {le}); }
  {
    auto *sl = new QSlider(Qt::Horizontal); sl->setRange(0, 100); sl->setValue(int(std::round(L.opacity * 100))); sl->setFixedWidth(110);
    auto *sp = num(std::round(L.opacity * 100), 1, 0, 100, [](Layer &l, double v) { l.opacity = v / 100; });
    connect(sl, &QSlider::valueChanged, this, [sp](int v) { sp->setValue(v); });
    connect(sp, &QDoubleSpinBox::valueChanged, sl, [sl](double v) { QSignalBlocker b(sl); sl->setValue(int(v)); });
    rowOf(U("不透明度"), {sl, sp, new QLabel("%")});
  }
  rowOf(U("位置"), {num(L.x, 1, -100000, 100000, [](Layer &l, double v) { l.x = v; }), num(L.y, 1, -100000, 100000, [](Layer &l, double v) { l.y = v; })});
  rowOf(U("大きさ"), {num(L.w, 1, 1, 100000, [isT](Layer &l, double v) { l.w = std::max(1.0, v); if (isT) l.autoFit = false; }, isT && L.autoFit),
                       num(L.h, 1, 1, 100000, [isT](Layer &l, double v) { l.h = std::max(1.0, v); if (isT) l.autoFit = false; }, isT && L.autoFit)});
  rowOf(U("回転"), {num(L.rot, 1, -3600, 3600, [](Layer &l, double v) { l.rot = v; }), new QLabel(U("°")), btn("0°", [this] { edit(nullptr, [](Layer &l) { l.rot = 0; }, true); })});
  {
    auto mk = [&](const QStringList &names, bool horiz) {
      auto *w = new QWidget; auto *h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(0);
      for (int i = 0; i < 3; i++) {
        auto *b = btn(names[i], [this, i, horiz] { mw->alignLayer(horiz ? i : -1, horiz ? -1 : i); }, U("キャンバスの") + names[i] + U("に"));
        h->addWidget(b);
      }
      return w;
    };
    rowOf(U("揃える"), {mk({U("左"), U("中"), U("右")}, true), mk({U("上"), U("中"), U("下")}, false)});
  }
  rowOf(QString(""), {chk(U(" 書き出す"), L.exportIt, [](Layer &l, bool v) { l.exportIt = v; }), chk(U(" ロック"), L.locked, [](Layer &l, bool v) { l.locked = v; })});

  if (isT) {
    h4(U("文字"));
    auto *te = new QPlainTextEdit(L.text); te->setObjectName("p-text"); te->setFixedHeight(70);
    connect(te, &QPlainTextEdit::textChanged, this, [this, te] {
      QString t = te->toPlainText();
      Layer *cl = mw->cur(); if (!cl || cl->text == t) return;
      edit(te, [=](Layer &l) {
        QString oldFirst = l.text.split('\n').value(0).left(20);
        if (l.name.isEmpty() || l.name == QString::fromUtf8("テキスト") || l.name == oldFirst) l.name = t.split('\n').value(0).left(20);
        l.text = t;
      });
    });
    V->addWidget(te);
    {
      auto *cb = new QComboBox; cb->setEditable(true);
      static QStringList fams = QFontDatabase::families();
      cb->addItems(fams); cb->setEditText(L.font);
      connect(cb, &QComboBox::editTextChanged, this, [this, cb](const QString &t) { edit(cb, [=](Layer &l) { l.font = t; }); });
      cb->setMinimumWidth(180);
      rowOf(U("フォント"), {cb});
    }
    rowOf(U("サイズ"), {num(L.size, 1, 1, 5000, [](Layer &l, double v) { l.size = std::max(1.0, v); }), new QLabel("px"), chk(U(" 太字"), L.bold, [](Layer &l, bool v) { l.bold = v; })});
    rowOf(U("色"), {col(L.color, [](Layer &l, const QString &c) { l.color = c; }),
                    chk(U(" グラデーション"), L.hasGradient, [](Layer &l, bool v) { l.hasGradient = v; if (v) { l.gradTop = "#FFF27A"; l.gradBottom = "#FF9A1F"; } })});
    if (L.hasGradient) rowOf(U("上→下"), {col(L.gradTop, [](Layer &l, const QString &c) { l.gradTop = c; }), col(L.gradBottom, [](Layer &l, const QString &c) { l.gradBottom = c; })});
    rowOf(U("行間"), {num(L.lineHeight, 0.05, 0.5, 10, [](Layer &l, double v) { l.lineHeight = std::max(0.5, v); }), new QLabel(U("倍"))});
    rowOf(U("字間"), {num(L.letterSpacing, 1, -1000, 1000, [](Layer &l, double v) { l.letterSpacing = v; }), new QLabel("px")});
    rowOf(U("横比率"), {num(L.scaleX, 1, 10, 400, [](Layer &l, double v) { l.scaleX = std::max(10.0, std::min(400.0, v)); }), new QLabel("%")});
    rowOf(U("最大幅"), {num(L.maxW, 1, 0, 100000, [](Layer &l, double v) { l.maxW = std::max(0.0, v); }), new QLabel(U("px(0=なし。超えたら横を縮める)"))});
    rowOf(U("横"), {seg({{"left", U("左")}, {"center", U("中央")}, {"right", U("右")}}, L.align, [](Layer &l, const QString &v) { l.align = v; })});
    rowOf(U("縦"), {seg({{"top", U("上")}, {"middle", U("中央")}, {"bottom", U("下")}}, L.valign, [](Layer &l, const QString &v) { l.valign = v; }),
                    chk(U(" 枠を文字に合わせる"), L.autoFit, [](Layer &l, bool v) { l.autoFit = v; })});
    h4(U("縁取り(外側→内側)"));
    for (int n = 0; n < L.outlines.size(); n++) {
      QList<QWidget *> ws{col(L.outlines[n].color, [n](Layer &l, const QString &c) { if (n < l.outlines.size()) l.outlines[n].color = c; }),
                          num(L.outlines[n].width, 1, 0, 1000, [n](Layer &l, double v) { if (n < l.outlines.size()) l.outlines[n].width = std::max(0.0, v); }), new QLabel("px")};
      if (n > 0) ws << btn(U("↑"), [this, n] { edit(nullptr, [n](Layer &l) { if (n < l.outlines.size()) l.outlines.move(n, n - 1); }, true); }, U("外側へ"));
      ws << btn(U("×"), [this, n] { edit(nullptr, [n](Layer &l) { if (n < l.outlines.size()) l.outlines.removeAt(n); }, true); });
      rowOf(QString(), ws);
    }
    rowOf(QString(), {btn(U("+ 縁を追加"), [this] { edit(nullptr, [](Layer &l) { l.outlines.append(Outline{"#FFFFFF", 4}); }, true); }),
                      btn(U("テロップ既定に戻す"), [this] {
                        edit(nullptr, [](Layer &l) {
                          Layer d; l.font = d.font; l.color = "#FFFFFF"; l.outlines = d.outlines; l.hasGradient = false; l.bold = false;
                        }, true);
                      })});
  }
  if (L.type == "shape") {
    h4(U("図形"));
    rowOf(U("形"), {seg({{"rect", U("矩形")}, {"roundrect", U("角丸矩形")}, {"ellipse", U("楕円")}}, L.shape, [](Layer &l, const QString &v) { l.shape = v; })});
    if (L.shape == "roundrect") rowOf(U("角の丸み"), {num(L.radius, 1, 0, 10000, [](Layer &l, double v) { l.radius = std::max(0.0, v); }), new QLabel("px")});
    { QList<QWidget *> ws{chk(U(" あり"), L.hasFill, [](Layer &l, bool v) { l.hasFill = v; if (v) l.fill = "#3B82F6"; })};
      if (L.hasFill) ws << col(L.fill, [](Layer &l, const QString &c) { l.fill = c; });
      rowOf(U("塗り"), ws); }
    { QList<QWidget *> ws{chk(U(" あり"), L.hasStroke, [](Layer &l, bool v) { l.hasStroke = v; if (v) l.stroke = Stroke{"#FFFFFF", 6}; })};
      if (L.hasStroke) { ws << col(L.stroke.color, [](Layer &l, const QString &c) { l.stroke.color = c; });
                         ws << num(L.stroke.width, 1, 0, 1000, [](Layer &l, double v) { l.stroke.width = std::max(0.0, v); }) << new QLabel("px"); }
      rowOf(U("線"), ws); }
  }
  if (L.type == "image") {
    h4(U("画像"));
    note(L.src);
    QString src = L.src;
    QImage im = vimg::loadImage(vimg::resolveSrc(mw->baseDir, src));
    rowOf(QString(), {btn(U("原寸"), [this, im] { if (im.isNull()) return; edit(nullptr, [im](Layer &l) { l.w = im.width(); l.h = im.height(); }, true); }),
                      btn(U("キャンバスに合わせる"), [this, im] {
                        if (im.isNull()) return;
                        edit(nullptr, [this, im](Layer &l) {
                          double k = std::min(double(mw->doc.width) / im.width(), double(mw->doc.height) / im.height());
                          l.w = r1(im.width() * k); l.h = r1(im.height() * k); l.x = mw->doc.width / 2.0; l.y = mw->doc.height / 2.0; l.rot = 0;
                        }, true);
                      }),
                      btn(U("左右反転"), [this] { edit(nullptr, [](Layer &l) { l.flip = !l.flip; }, true); })});
    h4(U("色調整"));
    auto adj = [&](const QString &name, double v, std::function<void(Layer &, double)> set) {
      auto *sl = new QSlider(Qt::Horizontal); sl->setRange(-100, 100); sl->setValue(int(v)); sl->setFixedWidth(130);
      auto *lb = new QLabel(QString::number(int(v))); lb->setFixedWidth(30);
      connect(sl, &QSlider::valueChanged, this, [this, sl, lb, set](int x) { lb->setText(QString::number(x)); edit(sl, [=](Layer &l) { set(l, x); }); });
      rowOf(name, {sl, lb});
    };
    adj(U("明るさ"), L.adjust.brightness, [](Layer &l, double v) { l.adjust.brightness = v; });
    adj(U("コントラスト"), L.adjust.contrast, [](Layer &l, double v) { l.adjust.contrast = v; });
    adj(U("彩度"), L.adjust.saturation, [](Layer &l, double v) { l.adjust.saturation = v; });
    rowOf(QString(), {btn(U("色調整をリセット"), [this] { edit(nullptr, [](Layer &l) { l.adjust = Adjust(); }, true); })});
  }
  h4(U("ドロップシャドウ"));
  rowOf(QString(""), {chk(U(" あり"), L.hasShadow, [](Layer &l, bool v) { l.hasShadow = v; if (v) l.shadow = ::Shadow(); })});
  if (L.hasShadow) {
    rowOf(U("色"), {col(L.shadow.color, [](Layer &l, const QString &c) { l.shadow.color = c; }),
                    num(std::round(L.shadow.opacity * 100), 1, 0, 100, [](Layer &l, double v) { l.shadow.opacity = v / 100; }), new QLabel("%")});
    rowOf(U("ずれ"), {num(L.shadow.x, 1, -10000, 10000, [](Layer &l, double v) { l.shadow.x = v; }), num(L.shadow.y, 1, -10000, 10000, [](Layer &l, double v) { l.shadow.y = v; })});
    rowOf(U("ぼかし"), {num(L.shadow.blur, 1, 0, 10000, [](Layer &l, double v) { l.shadow.blur = std::max(0.0, v); }), new QLabel("px")});
  }
  V->addStretch();
}
