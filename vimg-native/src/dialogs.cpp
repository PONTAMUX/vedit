// はじめの画面・ファイルダイアログ
#include "mainwindow.h"
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QListWidget>
#include <QSpinBox>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStyle>

static QString U(const char *s) { return QString::fromUtf8(s); }
static QString g_lastDir;
static const char *IMG_FILTER = "*.png *.jpg *.jpeg *.webp *.bmp *.gif";

static QString startDir() {
  if (!g_lastDir.isEmpty()) return g_lastDir;
  return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
}

QString pickImageFile(QWidget *parent) {
  QString f = QFileDialog::getOpenFileName(parent, U("画像を選ぶ"), startDir(), U("画像 (") + IMG_FILTER + U(");;すべて (*)"));
  if (!f.isEmpty()) g_lastDir = QFileInfo(f).absolutePath();
  return f;
}

void MainWindow::openDialog() {
  if (!discardOk()) return;
  QString f = QFileDialog::getOpenFileName(this, U("開く"), startDir(),
    U("vimg 編集データ・画像 (*.vimg.json ") + IMG_FILTER + U(");;すべて (*)"));
  if (f.isEmpty()) return;
  g_lastDir = QFileInfo(f).absolutePath();
  openPath(f);
}

void MainWindow::saveAsDialog() {
  canvas->finishEdit();
  QString init = temp ? startDir() + "/image" + vimg::EXT : docPath;
  QString f = QFileDialog::getSaveFileName(this, U("名前を付けて保存"), init, U("vimg 編集データ (*.vimg.json)"));
  if (f.isEmpty()) return;
  g_lastDir = QFileInfo(f).absolutePath();
  saveAs(f);
}

void MainWindow::newDialog() { if (discardOk()) showStart(); }

void MainWindow::showStart(const QString &dir) {
  QDialog dlg(this); dlg.setWindowTitle(U("vimg 画像編集")); dlg.setMinimumWidth(640);
  auto *V = new QVBoxLayout(&dlg); V->setSpacing(8);
  V->addWidget(new QLabel(U("<span style='font-size:20px;font-weight:bold'>vimg</span> <span style='color:#8d8d99'>画像編集(ブイイメージ)</span>")));
  auto *h4 = new QLabel(U("新規作成")); h4->setProperty("h4", true); V->addWidget(h4);
  QString bg;   // 透明
  struct S { int w, h; const char *t; } sizes[6] = {{1920, 1080, "フルHD(動画)"}, {1280, 720, "HD"}, {3840, 2160, "4K"}, {1080, 1920, "縦長(ショート)"}, {1080, 1080, "正方形"}, {1280, 720, "サムネイル"}};
  int chosenW = 0, chosenH = 0; QString opened;
  auto *grid = new QGridLayout; grid->setSpacing(6);
  for (int i = 0; i < 6; i++) {
    auto *b = new QPushButton(QString("%1 × %2\n%3").arg(sizes[i].w).arg(sizes[i].h).arg(U(sizes[i].t)));
    b->setStyleSheet("text-align:left;padding:7px 10px"); b->setFocusPolicy(Qt::NoFocus);
    QObject::connect(b, &QPushButton::clicked, &dlg, [&, i] { chosenW = sizes[i].w; chosenH = sizes[i].h; dlg.accept(); });
    grid->addWidget(b, i / 3, i % 3);
  }
  V->addLayout(grid);
  auto *row = new QHBoxLayout;
  auto *wi = new QSpinBox, *hi = new QSpinBox;
  for (auto *s : {wi, hi}) { s->setRange(1, 16384); s->setFixedWidth(84); }
  wi->setValue(1920); hi->setValue(1080);
  row->addWidget(new QLabel(U("サイズを指定"))); row->addWidget(wi); row->addWidget(new QLabel(U("×"))); row->addWidget(hi); row->addWidget(new QLabel("px"));
  row->addStretch(); row->addWidget(new QLabel(U("背景")));
  QList<QPushButton *> bgs; const char *bgn[3] = {"透明", "白", "黒"}; const char *bgv[3] = {"", "#FFFFFF", "#000000"};
  for (int i = 0; i < 3; i++) {
    auto *b = new QPushButton(U(bgn[i])); b->setFocusPolicy(Qt::NoFocus); b->setProperty("on", i == 0); bgs << b; row->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, &dlg, [&, i] {
      bg = bgv[i]; for (int j = 0; j < 3; j++) { bgs[j]->setProperty("on", j == i); bgs[j]->style()->unpolish(bgs[j]); bgs[j]->style()->polish(bgs[j]); }
    });
  }
  auto *mk = new QPushButton(U("作成")); mk->setProperty("primary", true); row->addWidget(mk);
  QObject::connect(mk, &QPushButton::clicked, &dlg, [&] { chosenW = wi->value(); chosenH = hi->value(); dlg.accept(); });
  V->addLayout(row);
  auto *h4b = new QLabel(U("開く")); h4b->setProperty("h4", true); V->addWidget(h4b);
  auto *orow = new QHBoxLayout;
  auto *ob = new QPushButton(U("ファイルを開く…")); ob->setProperty("primary", true); orow->addWidget(ob);
  auto *hint = new QLabel(U(".vimg.json(編集データ)か画像(PNG / JPG / WebP)。画像は隣に .vimg.json を作って開きます")); hint->setProperty("dim", true); orow->addWidget(hint, 1);
  V->addLayout(orow);
  bool pick = false;
  QObject::connect(ob, &QPushButton::clicked, &dlg, [&] { pick = true; dlg.accept(); });
  if (!dir.isEmpty()) {
    auto *t = new QLabel(U("このフォルダ: ") + dir); t->setProperty("h4", true); V->addWidget(t);
    auto *lw = new QListWidget; lw->setMaximumHeight(220);
    QDir d(dir);
    for (const QFileInfo &fi : d.entryInfoList(QStringList{"*.vimg.json", "*.png", "*.jpg", "*.jpeg", "*.webp", "*.bmp", "*.gif"}, QDir::Files, QDir::Name)) {
      if (fi.fileName().contains(".src.")) continue;
      auto *it = new QListWidgetItem((fi.fileName().endsWith(".vimg.json", Qt::CaseInsensitive) ? U("🎨 ") : U("🖼 ")) + fi.fileName());
      it->setData(Qt::UserRole, fi.absoluteFilePath()); lw->addItem(it);
    }
    QObject::connect(lw, &QListWidget::itemClicked, &dlg, [&](QListWidgetItem *it) { opened = it->data(Qt::UserRole).toString(); dlg.accept(); });
    V->addWidget(lw);
  }
  dlg.exec();
  if (!opened.isEmpty()) { if (discardOk()) openPath(opened); }
  else if (pick) openDialog();
  else if (chosenW > 0) { if (discardOk()) newDoc(chosenW, chosenH, bg); }
}
