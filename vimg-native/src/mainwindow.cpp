#include "mainwindow.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QKeyEvent>
#include <QCloseEvent>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QTextEdit>
#include <QComboBox>
#include <QMessageBox>
#include <QTimer>
#include <QClipboard>
#include <QMimeData>
#include <QScrollArea>
#include <QStatusBar>
#include <QStyle>
#include <cmath>

static const char *STYLE = R"(
* { font-family: "Yu Gothic UI", "Segoe UI", sans-serif; font-size: 13px; color: #e6e6ea; }
QMainWindow, QWidget#central { background: #1b1b1f; }
QWidget#top, QWidget#tools, QWidget#side, QWidget#foot { background: #242429; }
QWidget#top { border-bottom: 1px solid #3a3a43; }
QWidget#opts { background: #2d2d34; border-bottom: 1px solid #3a3a43; }
QWidget#tools { border-right: 1px solid #3a3a43; }
QWidget#side { border-left: 1px solid #3a3a43; }
QWidget#foot { border-top: 1px solid #3a3a43; }
QWidget#foot QLabel { color: #8d8d99; font-size: 12px; }
QWidget#layhd { border-top: 1px solid #3a3a43; border-bottom: 1px solid #3a3a43; }
QPushButton { background: #2d2d34; border: 1px solid #3a3a43; border-radius: 5px; padding: 4px 10px; }
QPushButton:hover { border-color: #3b82f6; }
QPushButton[primary="true"], QPushButton[on="true"] { background: #3b82f6; border-color: #3b82f6; color: white; }
QPushButton:disabled { color: #6a6a74; }
QWidget#tools QPushButton { padding: 0; font-size: 16px; min-width: 34px; max-width: 34px; min-height: 34px; max-height: 34px; }
QLineEdit, QPlainTextEdit, QAbstractSpinBox, QComboBox { background: #1b1b1f; border: 1px solid #3a3a43; border-radius: 4px; padding: 2px 4px; selection-background-color: #3b82f6; }
QComboBox QAbstractItemView { background: #242429; selection-background-color: #3b82f6; }
QLabel[dim="true"] { color: #8d8d99; }
QLabel[h4="true"] { font-weight: bold; margin-top: 8px; }
QScrollArea { border: none; background: #242429; }
QScrollBar:vertical { background: #242429; width: 10px; }
QScrollBar::handle:vertical { background: #3a3a43; border-radius: 5px; min-height: 20px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QCheckBox { spacing: 4px; }
QSlider::groove:horizontal { height: 4px; background: #3a3a43; border-radius: 2px; }
QSlider::handle:horizontal { background: #3b82f6; width: 12px; margin: -5px 0; border-radius: 6px; }
QToolTip { background: #2d2d34; color: #e6e6ea; border: 1px solid #3a3a43; }
)";

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
  qApp->setStyleSheet(STYLE);
  buildUi();
  resize(1360, 860);
  newDoc(1920, 1080, QString());
}
MainWindow::~MainWindow() {}

QPushButton *MainWindow::mkBtn(const QString &t, const QString &tip, QWidget *parent) {
  auto *b = new QPushButton(t, parent); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b;
}

void MainWindow::buildUi() {
  auto *c = new QWidget; c->setObjectName("central"); setCentralWidget(c);
  auto *v = new QVBoxLayout(c); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
  // 上のバー
  auto *top = new QWidget; top->setObjectName("top"); auto *th = new QHBoxLayout(top); th->setContentsMargins(12, 8, 12, 8); th->setSpacing(6);
  auto *logo = new QLabel(QString::fromUtf8("<b>vimg</b> <span style='color:#8d8d99'>画像編集</span>")); logo->setToolTip(QString::fromUtf8("vimg(ブイイメージ)"));
  th->addWidget(logo);
  auto *bNew = mkBtn(QString::fromUtf8("新規"), QString::fromUtf8("Ctrl+N : 新規作成(サイズを選ぶ)"), top);
  auto *bOpen = mkBtn(QString::fromUtf8("開く"), QString::fromUtf8("Ctrl+O : ファイルを開く(.vimg.json / 画像)"), top);
  auto *bSaveAs = mkBtn(QString::fromUtf8("名前を付けて保存"), QString::fromUtf8("Ctrl+Shift+S : 名前を付けて保存"), top);
  th->addSpacing(6); th->addWidget(bNew); th->addWidget(bOpen); th->addWidget(bSaveAs);
  pname = new QLabel; pname->setProperty("dim", true); pname->setMinimumWidth(10); pname->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  th->addSpacing(6); th->addWidget(pname, 1);
  savestate = new QLabel; savestate->setProperty("dim", true); th->addWidget(savestate);
  auto *bUndo = mkBtn(QString::fromUtf8("元に戻す"), "Ctrl+Z", top), *bRedo = mkBtn(QString::fromUtf8("やり直す"), "Ctrl+Shift+Z", top);
  auto *bSave = mkBtn(QString::fromUtf8("保存"), QString::fromUtf8("Ctrl+S : 編集データ(.vimg.json)と透過PNGを保存"), top); bSave->setProperty("primary", true);
  th->addWidget(bUndo); th->addWidget(bRedo); th->addWidget(bSave);
  v->addWidget(top);
  connect(bNew, &QPushButton::clicked, this, [this] { newDialog(); });
  connect(bOpen, &QPushButton::clicked, this, [this] { openDialog(); });
  connect(bSaveAs, &QPushButton::clicked, this, [this] { saveAsDialog(); });
  connect(bUndo, &QPushButton::clicked, this, [this] { undo(); });
  connect(bRedo, &QPushButton::clicked, this, [this] { redo(); });
  connect(bSave, &QPushButton::clicked, this, [this] { save(); });
  // オプションバー
  opts = new QWidget; opts->setObjectName("opts"); opts->setFixedHeight(36);
  optsLay = new QHBoxLayout(opts); optsLay->setContentsMargins(12, 0, 12, 0); optsLay->setSpacing(8);
  v->addWidget(opts);
  // 本体
  auto *main = new QWidget; auto *mh = new QHBoxLayout(main); mh->setContentsMargins(0, 0, 0, 0); mh->setSpacing(0);
  auto *tools = new QWidget; tools->setObjectName("tools"); tools->setFixedWidth(44);
  auto *tv = new QVBoxLayout(tools); tv->setContentsMargins(5, 6, 5, 6); tv->setSpacing(4);
  const char *tn[5][2] = {{"✥", "V : 移動・変形"}, {"T", "T : 文字(クリックで新規 / 文字をクリックで編集)"}, {"▭", "U : 図形(ドラッグで描く)"},
                          {"⌗", "C : 切り抜き(ドラッグで範囲 → Enter)"}, {"✋", "H : 手のひら(Space+ドラッグでも)"}};
  for (int i = 0; i < 5; i++) {
    toolBtn[i] = mkBtn(QString::fromUtf8(tn[i][0]), QString::fromUtf8(tn[i][1]), tools); tv->addWidget(toolBtn[i]);
    connect(toolBtn[i], &QPushButton::clicked, this, [this, i] { canvas->setTool(Canvas::Tool(i)); });
  }
  tv->addStretch(); mh->addWidget(tools);
  canvas = new Canvas(this); mh->addWidget(canvas, 1);
  auto *side = new QWidget; side->setObjectName("side"); side->setFixedWidth(320);
  auto *sv = new QVBoxLayout(side); sv->setContentsMargins(0, 0, 0, 0); sv->setSpacing(0);
  props = new PropsPanel(this); sv->addWidget(props, 55);
  auto *lh = new QWidget; lh->setObjectName("layhd"); auto *lhh = new QHBoxLayout(lh); lhh->setContentsMargins(8, 6, 8, 6); lhh->setSpacing(4);
  lhh->addWidget(new QLabel(QString::fromUtf8("<b>レイヤー</b>"))); lhh->addStretch();
  struct B { const char *t, *tip; } lb[5] = {{"T", "テキストレイヤーを追加"}, {"▭", "図形レイヤーを追加"}, {"🖼", "画像を追加(ドラッグ&ドロップ・Ctrl+V でも)"}, {"⧉", "Ctrl+J : 複製"}, {"🗑", "Delete : 削除"}};
  QPushButton *lbs[5];
  for (int i = 0; i < 5; i++) { lbs[i] = mkBtn(QString::fromUtf8(lb[i].t), QString::fromUtf8(lb[i].tip), lh); lbs[i]->setStyleSheet("padding:2px 7px"); lhh->addWidget(lbs[i]); }
  connect(lbs[0], &QPushButton::clicked, this, [this] { newText(doc.width / 2.0, doc.height / 2.0); });
  connect(lbs[1], &QPushButton::clicked, this, [this] { newShape(doc.width / 2.0, doc.height / 2.0); });
  connect(lbs[2], &QPushButton::clicked, this, [this] {
    QString f = pickImageFile(this);
    if (!f.isEmpty()) addImage(f);
  });
  connect(lbs[3], &QPushButton::clicked, this, [this] { dupLayer(); });
  connect(lbs[4], &QPushButton::clicked, this, [this] { delLayer(); });
  sv->addWidget(lh);
  auto *sc = new QScrollArea; sc->setWidgetResizable(true); list = new LayerList(this); sc->setWidget(list); sv->addWidget(sc, 45);
  mh->addWidget(side);
  v->addWidget(main, 1);
  // ステータス
  auto *foot = new QWidget; foot->setObjectName("foot"); foot->setFixedHeight(24);
  auto *fh = new QHBoxLayout(foot); fh->setContentsMargins(12, 0, 12, 0); fh->setSpacing(16);
  stZoom = new QLabel; stSize = new QLabel; stPos = new QLabel;
  fh->addWidget(stZoom); fh->addWidget(stSize); fh->addWidget(stPos); fh->addStretch();
  fh->addWidget(new QLabel(QString::fromUtf8("Space+ドラッグ=移動 / Alt+ホイール=拡大縮小 / Ctrl+0=全体表示 / ダブルクリック=文字を編集")));
  v->addWidget(foot);
  toastLbl = new QLabel(canvas); toastLbl->setStyleSheet("background:#2d2d34;border:1px solid #3a3a43;border-radius:6px;padding:6px 12px"); toastLbl->hide();
  connect(canvas, &Canvas::toolChanged, this, [this] {
    for (int i = 0; i < 5; i++) { toolBtn[i]->setProperty("on", int(canvas->tool) == i); toolBtn[i]->style()->unpolish(toolBtn[i]); toolBtn[i]->style()->polish(toolBtn[i]); }
    renderOpts();
  });
  setFocusPolicy(Qt::StrongFocus);
}

void MainWindow::renderOpts() {
  while (QLayoutItem *it = optsLay->takeAt(0)) { if (it->widget()) it->widget()->deleteLater(); delete it; }
  auto hint = [&](const QString &t) { auto *l = new QLabel(t); l->setProperty("dim", true); optsLay->addWidget(l); };
  auto seg = [&](const QStringList &names, int curIdx, std::function<void(int)> fn) {
    auto *w = new QWidget; auto *h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(0);
    for (int i = 0; i < names.size(); i++) {
      auto *b = mkBtn(names[i], QString(), w); b->setProperty("on", i == curIdx); h->addWidget(b);
      connect(b, &QPushButton::clicked, this, [fn, i] { fn(i); });
    }
    optsLay->addWidget(w);
  };
  switch (canvas->tool) {
  case Canvas::Move: {
    auto *b = mkBtn(QString::fromUtf8("🧲 スナップ"), QString(), opts); b->setProperty("on", canvas->snapOn); optsLay->addWidget(b);
    connect(b, &QPushButton::clicked, this, [this] { canvas->snapOn = !canvas->snapOn; renderOpts(); });
    hint(QString::fromUtf8("角=拡大縮小(画像は比率固定・Shift で自由 / 図形は Shift で比率固定 / 文字は文字サイズごと) / 辺=枠 / 上の丸=回転(Shift で15°刻み) / Alt=中心から / Alt+ドラッグ=複製 / 矢印=1px(Shift で10px)"));
    break; }
  case Canvas::Text: hint(QString::fromUtf8("クリックで文字を追加 / 文字をクリックで編集 / Esc・枠の外をクリックで確定")); break;
  case Canvas::Shape:
    seg({QString::fromUtf8("矩形"), QString::fromUtf8("角丸矩形"), QString::fromUtf8("楕円")}, canvas->shapeKind, [this](int i) { canvas->shapeKind = i; renderOpts(); });
    hint(QString::fromUtf8("ドラッグで描く(Shift で正方形・正円) / Shift+U で種類切替")); break;
  case Canvas::Crop: {
    auto *t = new QLabel(QString::fromUtf8("<b>切り抜き</b>")); optsLay->addWidget(t);
    if (canvas->hasCrop) optsLay->addWidget(new QLabel(QString("%1 × %2  (x %3, y %4)").arg(canvas->crop.width()).arg(canvas->crop.height()).arg(canvas->crop.x()).arg(canvas->crop.y())));
    auto *a = mkBtn(QString::fromUtf8("適用 (Enter)"), QString(), opts); a->setProperty("primary", true); optsLay->addWidget(a);
    auto *x = mkBtn(QString::fromUtf8("キャンセル (Esc)"), QString(), opts); optsLay->addWidget(x);
    connect(a, &QPushButton::clicked, this, [this] { canvas->applyCrop(); });
    connect(x, &QPushButton::clicked, this, [this] { canvas->setTool(Canvas::Move); });
    hint(QString::fromUtf8("ドラッグで範囲(Shift で正方形)。キャンバスの外まで広げると余白が増えます")); break; }
  case Canvas::Hand: hint(QString::fromUtf8("ドラッグで表示位置を移動")); break;
  }
  optsLay->addStretch();
}

// ---------------------------------------------------------------- 文書
void MainWindow::loadFrom(const Doc &d, const QString &path, bool isTemp) {
  canvas->finishEdit();
  doc = d; docPath = path; baseDir = QFileInfo(path).absolutePath(); temp = isTemp; dirty = false;
  undoS.clear(); redoS.clear(); sel.clear(); canvas->hasCrop = false; lastTextStyle = QJsonObject();
  for (auto &l : doc.layers) if (l.type == "text") lastTextStyle = l.toJson();
  canvas->fitView(); canvas->setTool(Canvas::Move);
  canvas->invalidate(); list->sync(); props->rebuild(); updateTitle();
  savestate->setText(isTemp ? QString::fromUtf8("未保存(新規)") : QString::fromUtf8("保存済み"));
}

bool MainWindow::newDoc(int w, int h, const QString &bg) {
  tmp = std::make_unique<QTemporaryDir>();
  Doc d; d.width = qBound(1, w, 16384); d.height = qBound(1, h, 16384); d.background = bg;
  loadFrom(d, tmp->path() + "/" + QString::fromUtf8("無題") + vimg::EXT, true);
  return true;
}

bool MainWindow::openPath(const QString &path) {
  QString p = path, err;
  bool madeNew = false;
  if (!vimg::isDoc(p)) {
    p = vimg::fromRaster(path, &err);
    if (p.isEmpty()) { if (!silent) QMessageBox::warning(this, "vimg", QString::fromUtf8("開けませんでした: ") + err); return false; }
    madeNew = !QFile::exists(vimg::pngOf(p));
  }
  Doc d;
  if (!Doc::load(p, d, &err)) { if (!silent) QMessageBox::warning(this, "vimg", QString::fromUtf8("開けませんでした: ") + err); return false; }
  tmp.reset();
  loadFrom(d, QFileInfo(p).absoluteFilePath(), false);
  if (madeNew) save();
  return true;
}

QString MainWindow::importImage(const QString &file) {
  QFileInfo fi(file);
  QDir base(baseDir);
  QString abs = fi.absoluteFilePath();
  if (!temp && abs.startsWith(QDir::cleanPath(baseDir) + "/", Qt::CaseInsensitive)) return base.relativeFilePath(abs);   // 文書の配下ならそのまま参照
  QDir().mkpath(baseDir + "/assets");
  QString dst = baseDir + "/assets/" + fi.fileName();
  for (int n = 2; QFile::exists(dst); n++) dst = baseDir + "/assets/" + fi.completeBaseName() + "_" + QString::number(n) + "." + fi.suffix();
  if (!QFile::copy(abs, dst)) return QString();
  return "assets/" + QFileInfo(dst).fileName();
}

Layer *MainWindow::addImage(const QString &file, QPointF at) {
  QString src = importImage(file);
  if (src.isEmpty()) { toast(QString::fromUtf8("取り込み失敗: ") + file, 4000); return nullptr; }
  QImage im = vimg::loadImage(vimg::resolveSrc(baseDir, src));
  if (im.isNull()) { toast(QString::fromUtf8("画像を読み込めません: ") + src, 4000); return nullptr; }
  double w = im.width(), h = im.height(), k = std::min({1.0, doc.width / w, doc.height / h}); w *= k; h *= k;
  if (at.x() < 0) at = QPointF(doc.width / 2.0, doc.height / 2.0);
  Layer l; l.type = "image"; l.src = src; l.x = std::round(at.x() * 10) / 10; l.y = std::round(at.y() * 10) / 10;
  l.w = std::round(w * 10) / 10; l.h = std::round(h * 10) / 10; l.name = QFileInfo(src).fileName();
  return addLayer(l);
}

bool MainWindow::saveAs(const QString &docFile) {
  canvas->finishEdit();
  QString f = QFileInfo(docFile).absoluteFilePath();
  if (!vimg::isDoc(f)) f += vimg::EXT;
  QFileInfo fi(f); QDir().mkpath(fi.absolutePath());
  QString newBase = fi.absolutePath();
  if (QDir::cleanPath(newBase) != QDir::cleanPath(baseDir)) {
    QString tmpRoot = tmp ? QDir::cleanPath(tmp->path()) : QString();
    for (auto &l : doc.layers) {
      if (l.type != "image" || l.src.isEmpty()) continue;
      QString src = QFileInfo(vimg::resolveSrc(baseDir, l.src)).absoluteFilePath();
      if (!tmpRoot.isEmpty() && src.startsWith(tmpRoot + "/", Qt::CaseInsensitive)) {   // 一時フォルダの取り込み画像は保存先の assets/ へ
        QDir().mkpath(newBase + "/assets");
        QFileInfo si(src); QString dst = newBase + "/assets/" + si.fileName();
        for (int n = 2; QFile::exists(dst); n++) dst = newBase + "/assets/" + si.completeBaseName() + "_" + QString::number(n) + "." + si.suffix();
        if (QFile::exists(src)) QFile::copy(src, dst);
        l.src = "assets/" + QFileInfo(dst).fileName();
      } else l.src = QDir(newBase).relativeFilePath(src);
    }
  }
  docPath = f; baseDir = newBase; temp = false; tmp.reset();
  undoS.clear(); redoS.clear();
  return save();
}

bool MainWindow::save() {
  if (temp) { if (silent) return false; saveAsDialog(); return !temp; }
  canvas->finishEdit();
  if (canvas->hasCrop && canvas->tool == Canvas::Crop) { toast(QString::fromUtf8("切り抜きを確定(Enter)かキャンセル(Esc)してください")); return false; }
  for (auto &l : doc.layers) fitText(l);
  QString err;
  if (!doc.save(docPath, &err)) { savestate->setText(QString::fromUtf8("保存失敗: ") + err); return false; }
  QStringList miss;
  QImage im = vimg::renderDoc(doc, baseDir, true, &miss);
  if (!vimg::writePng(im, vimg::pngOf(docPath), &err)) { savestate->setText(QString::fromUtf8("保存失敗: ") + err); return false; }
  dirty = false; savestate->setText(QString::fromUtf8("保存済み"));
  toast(QString::fromUtf8("保存しました: ") + QFileInfo(vimg::pngOf(docPath)).fileName(), 1600);
  if (!miss.isEmpty()) toast(QString::fromUtf8("画像を読み込めません: ") + miss.join(", "), 4000);
  updateTitle();
  return true;
}

void MainWindow::updateTitle() {
  QString name = temp ? QString::fromUtf8("無題(未保存)") : QFileInfo(docPath).fileName();
  if (name.endsWith(vimg::EXT, Qt::CaseInsensitive)) name.chop(vimg::EXT.size());
  setWindowTitle(QString::fromUtf8("vimg 画像編集 - ") + name);
  pname->setText(temp ? QString::fromUtf8("無題(保存すると場所を選べます)") : docPath);
  stSize->setText(QString("%1 × %2").arg(doc.width).arg(doc.height));
}

// ---------------------------------------------------------------- 編集
Layer *MainWindow::cur() { return sel.isEmpty() ? nullptr : doc.find(sel); }

void MainWindow::select(const QString &id) {
  if (canvas->editing() && canvas->edId != id) canvas->finishEdit();
  sel = id;
  if (Layer *l = cur(); l && l->type == "text") lastTextStyle = l->toJson();
  canvas->guides.clear(); canvas->update(); list->sync(); props->rebuild();
}

void MainWindow::snap() {
  undoS.append(QJsonDocument(doc.toJson()).toJson(QJsonDocument::Compact));
  if (undoS.size() > 100) undoS.removeFirst();
  redoS.clear();
}

void MainWindow::commit(bool propsToo) {
  dirty = true; savestate->setText(QString::fromUtf8("未保存"));
  canvas->invalidate(); list->sync(); updateTitle();
  if (propsToo) props->rebuild();
}

static void doUndo(MainWindow *m, QList<QByteArray> &from, QList<QByteArray> &to) {
  m->canvas->finishEdit();
  if (from.isEmpty()) return;
  int w0 = m->doc.width, h0 = m->doc.height;
  to.append(QJsonDocument(m->doc.toJson()).toJson(QJsonDocument::Compact));
  QByteArray b = from.takeLast();
  m->doc = Doc::fromJson(QJsonDocument::fromJson(b).object(), m->baseDir);
  m->canvas->guides.clear();
  if (m->canvas->hasCrop) { m->canvas->hasCrop = false; if (m->canvas->tool == Canvas::Crop) m->canvas->setTool(Canvas::Move); }
  if (!m->cur()) m->sel.clear();
  if (m->doc.width != w0 || m->doc.height != h0) m->canvas->fitView();
  m->commit();
}
void MainWindow::undo() { doUndo(this, undoS, redoS); }
void MainWindow::redo() { doUndo(this, redoS, undoS); }

Layer *MainWindow::addLayer(Layer l, bool above) {
  snap();
  if (l.id.isEmpty()) l.id = vimg::newId();
  if (l.name.isEmpty()) l.name = vimg::newName(l);
  int i = doc.layers.size();
  if (above) { int c = doc.indexOf(sel); if (c >= 0) i = c + 1; }
  doc.layers.insert(i, l); sel = l.id;
  if (l.type == "text") lastTextStyle = l.toJson();
  commit();
  return &doc.layers[i];
}

Layer *MainWindow::newText(double x, double y, const QString &text) {
  Layer base; base.type = "text";
  QJsonObject o = base.toJson();
  for (const char *k : {"font", "size", "color", "bold", "line_height", "letter_spacing", "scale_x", "max_w", "align", "outlines", "gradient", "shadow"})
    if (lastTextStyle.contains(k)) o[k] = lastTextStyle[k];
  Layer l = Layer::fromJson(o); l.type = "text"; l.id.clear(); l.name.clear(); l.text = text; l.visible = true; l.locked = false; l.exportIt = true;
  l.x = std::round(x * 10) / 10; l.y = std::round(y * 10) / 10; l.rot = 0; l.opacity = 1; l.autoFit = true; l.xNull = l.yNull = false;
  Layer *r = addLayer(l);
  QString id = r->id;
  if (!silent) canvas->startEdit(id, true);
  return doc.find(id);
}

Layer *MainWindow::newShape(double x, double y, double w, double h) {
  Layer l; l.type = "shape"; l.shape = canvas->shapeKind == 1 ? "roundrect" : canvas->shapeKind == 2 ? "ellipse" : "rect";
  l.x = std::round(x * 10) / 10; l.y = std::round(y * 10) / 10; l.w = w; l.h = h; l.fill = "#3B82F6"; l.hasFill = true; l.hasStroke = false; l.radius = 24;
  return addLayer(l);
}

void MainWindow::delLayer() {
  Layer *l = cur(); if (!l) return;
  canvas->finishEdit(); l = cur(); if (!l) return;
  snap(); int i = doc.indexOf(sel); doc.layers.removeAt(i);
  sel = doc.layers.isEmpty() ? QString() : doc.layers[std::min(i, int(doc.layers.size()) - 1)].id;
  commit();
}

void MainWindow::dupLayer() {
  Layer *l = cur(); if (!l) return;
  QJsonObject o = l->toJson(); o.remove("id");
  Layer c = Layer::fromJson(o); c.id.clear(); c.name = l->name + QString::fromUtf8(" のコピー"); c.locked = false;
  addLayer(c);
}

void MainWindow::moveLayer(int d) {
  int i = doc.indexOf(sel); if (i < 0) return;
  int j = qBound(0, i + d, int(doc.layers.size()) - 1); if (i == j) return;
  snap(); doc.layers.move(i, j); commit();
}

void MainWindow::copyLayers() {
  Layer *l = cur(); if (!l) return;
  canvas->finishEdit(); l = cur(); if (!l) return;
  clip = {l->toJson()}; clipN = 0;
  toast(QString::fromUtf8("コピー: ") + l->name, 1200);
}

void MainWindow::pasteLayers() {
  if (clip.isEmpty()) return;
  clipN++; double off = 20 * clipN;
  snap();
  int i = doc.layers.size(); if (int c = doc.indexOf(sel); c >= 0) i = c + 1;
  QString last;
  for (const auto &src : clip) {
    QJsonObject o = src; o.remove("id");
    Layer c = Layer::fromJson(o); c.id = vimg::newId(); c.locked = false;
    c.x = std::round((c.x + off) * 10) / 10; c.y = std::round((c.y + off) * 10) / 10; c.xNull = c.yNull = c.wNull = c.hNull = false;
    doc.layers.insert(i++, c); last = c.id;
  }
  if (!last.isEmpty()) sel = last;
  commit();
}

void MainWindow::alignLayer(int ax, int ay) {
  Layer *l = cur(); if (!l) return;
  fitText(*l);
  QPointF e = Canvas::extent(*l);
  snap();
  if (ax >= 0) l->x = std::round((ax == 0 ? e.x() : ax == 1 ? doc.width / 2.0 : doc.width - e.x()) * 10) / 10;
  if (ay >= 0) l->y = std::round((ay == 0 ? e.y() : ay == 1 ? doc.height / 2.0 : doc.height - e.y()) * 10) / 10;
  commit();
}

void MainWindow::setText(const QString &t) {
  Layer *l = cur(); if (!l || l->type != "text") return;
  snap();
  QString oldName = l->text.split('\n').value(0).left(20);
  if (l->name.isEmpty() || l->name == QString::fromUtf8("テキスト") || l->name == oldName) l->name = t.split('\n').value(0).left(20);
  l->text = t; fitText(*l);
  commit();
}

void MainWindow::nudge(double dx, double dy) {
  Layer *l = cur(); if (!l || l->locked) return;
  snap(); l->x = std::round((l->x + dx) * 10) / 10; l->y = std::round((l->y + dy) * 10) / 10;
  commit();
}

void MainWindow::resizeCanvas(int w, int h) {
  w = qBound(1, w, 16384); h = qBound(1, h, 16384);
  snap();
  double dx = (w - doc.width) / 2.0, dy = (h - doc.height) / 2.0;
  for (auto &l : doc.layers) { l.x = std::round((l.x + dx) * 10) / 10; l.y = std::round((l.y + dy) * 10) / 10; }
  doc.width = w; doc.height = h; canvas->fitView(); commit();
}

bool MainWindow::discardOk() {
  if (!dirty || silent) return true;
  return QMessageBox::question(this, "vimg", QString::fromUtf8("保存していない変更があります。破棄してよいですか?")) == QMessageBox::Yes;
}

void MainWindow::toast(const QString &s, int ms) {
  if (!toastLbl) return;
  toastLbl->setText(s); toastLbl->adjustSize();
  toastLbl->move((canvas->width() - toastLbl->width()) / 2, canvas->height() - toastLbl->height() - 24);
  toastLbl->show(); toastLbl->raise();
  QTimer::singleShot(ms, toastLbl, [this] { toastLbl->hide(); });
}

void MainWindow::closeEvent(QCloseEvent *e) {
  if (!discardOk()) e->ignore(); else e->accept();
}

// ---------------------------------------------------------------- キーボード(Photoshop 準拠)
void MainWindow::keyReleaseEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { canvas->spaceDown = false; canvas->unsetCursor(); }
  QMainWindow::keyReleaseEvent(e);
}

void MainWindow::keyPressEvent(QKeyEvent *e) {
  QWidget *fw = QApplication::focusWidget();
  bool typing = fw && (qobject_cast<QLineEdit *>(fw) || qobject_cast<QAbstractSpinBox *>(fw) || qobject_cast<QPlainTextEdit *>(fw) || qobject_cast<QTextEdit *>(fw));
  bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
  int k = e->key();
  if (ctrl && k == Qt::Key_S) { if (shift) saveAsDialog(); else save(); return; }
  if (ctrl && k == Qt::Key_O) { openDialog(); return; }
  if (ctrl && k == Qt::Key_N) { newDialog(); return; }
  if (canvas->editing()) { QMainWindow::keyPressEvent(e); return; }
  if (typing) {
    if (k == Qt::Key_Escape) { fw->clearFocus(); canvas->setFocus(); return; }
    QMainWindow::keyPressEvent(e); return;
  }
  Layer *l = cur();
  if (ctrl && !shift && k == Qt::Key_C) { copyLayers(); return; }
  if (ctrl && !shift && k == Qt::Key_X) { copyLayers(); delLayer(); return; }
  if (ctrl && !shift && k == Qt::Key_V) {
    const QMimeData *md = QApplication::clipboard()->mimeData();
    if (md && md->hasImage() && clip.isEmpty()) {
      QImage im = qvariant_cast<QImage>(md->imageData());
      if (!im.isNull()) { QString f = baseDir + "/paste.png"; im.save(f); addImage(f); return; }
    }
    pasteLayers(); return;
  }
  if (ctrl) {
    switch (k) {
    case Qt::Key_Z: if (shift) redo(); else undo(); return;
    case Qt::Key_Y: redo(); return;
    case Qt::Key_J: dupLayer(); return;
    case Qt::Key_D: select(QString()); return;
    case Qt::Key_T: canvas->setTool(Canvas::Move); if (l) toast(QString::fromUtf8("変形: 角・辺・上の丸をドラッグ"), 1500); return;
    case Qt::Key_Plus: case Qt::Key_Equal: case Qt::Key_Semicolon: canvas->zoomAt(1.25); return;
    case Qt::Key_Minus: canvas->zoomAt(0.8); return;
    case Qt::Key_0: canvas->fitView(); canvas->update(); return;
    case Qt::Key_1: canvas->zoomAt(1 / canvas->zoom); return;
    case Qt::Key_BracketRight: moveLayer(1); return;
    case Qt::Key_BracketLeft: moveLayer(-1); return;
    default: break;
    }
    QMainWindow::keyPressEvent(e); return;
  }
  switch (k) {
  case Qt::Key_V: canvas->setTool(Canvas::Move); return;
  case Qt::Key_T: canvas->setTool(Canvas::Text); return;
  case Qt::Key_U: if (shift) canvas->shapeKind = (canvas->shapeKind + 1) % 3; canvas->setTool(Canvas::Shape); if (shift) renderOpts(); return;
  case Qt::Key_C: canvas->setTool(Canvas::Crop); return;
  case Qt::Key_H: canvas->setTool(Canvas::Hand); return;
  case Qt::Key_Delete: case Qt::Key_Backspace: delLayer(); return;
  case Qt::Key_Return: case Qt::Key_Enter:
    if (canvas->tool == Canvas::Crop) canvas->applyCrop(); else if (l && l->type == "text") canvas->startEdit(l->id, false);
    return;
  case Qt::Key_Escape: if (canvas->tool == Canvas::Crop) canvas->setTool(Canvas::Move); else select(QString()); return;
  case Qt::Key_Space:
    if (!e->isAutoRepeat()) { canvas->spaceDown = true; canvas->setCursor(Qt::OpenHandCursor); }
    return;
  case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down: {
    if (!l || l->locked) return;
    double n = shift ? 10 : 1;
    nudge(k == Qt::Key_Left ? -n : k == Qt::Key_Right ? n : 0, k == Qt::Key_Up ? -n : k == Qt::Key_Down ? n : 0);
    return; }
  default: break;
  }
  QMainWindow::keyPressEvent(e);
}
