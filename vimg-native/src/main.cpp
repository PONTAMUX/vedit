// vimg: 画像編集アプリ(C++/Qt)。
//   vimg.exe --render doc.vimg.json -o out.png   画面なしで透過PNGに書き出す
//   vimg.exe [ファイル(.vimg.json か画像) | フォルダ]  編集画面
#include "mainwindow.h"
#include "render.h"
#include <QApplication>
#include <QGuiApplication>
#include <QFileInfo>
#include <QDir>
#include <cstdio>

static int renderCli(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QString in, out;
  const QStringList a = app.arguments();
  for (int i = 1; i < a.size(); ++i) {
    if (a[i] == "--render" && i + 1 < a.size()) in = a[++i];
    else if ((a[i] == "-o" || a[i] == "--out") && i + 1 < a.size()) out = a[++i];
  }
  if (in.isEmpty()) { fprintf(stderr, "usage: vimg --render doc.vimg.json -o out.png\n"); return 2; }
  QString err; Doc d;
  if (!Doc::load(in, d, &err)) { fprintf(stderr, "%s\n", qPrintable(err)); return 1; }
  if (out.isEmpty()) out = vimg::pngOf(in);
  QStringList miss;
  QImage im = vimg::renderDoc(d, QFileInfo(in).absolutePath(), true, &miss);
  if (!miss.isEmpty()) { fprintf(stderr, "画像を読み込めません: %s\n", qPrintable(miss.join(", "))); return 1; }
  if (!vimg::writePng(im, out, &err)) { fprintf(stderr, "%s\n", qPrintable(err)); return 1; }
  return 0;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i)
    if (QByteArray(argv[i]) == "--render") return renderCli(argc, argv);
  QApplication app(argc, argv);
  app.setApplicationName("vimg");
  MainWindow w;
  w.show();
  const QStringList a = app.arguments();
  QString arg = a.size() > 1 ? a[1] : QString();
  if (!arg.isEmpty() && QFileInfo(arg).isDir()) w.showStart(QDir(arg).absolutePath());
  else if (!arg.isEmpty()) { if (!w.openPath(QFileInfo(arg).absoluteFilePath())) w.showStart(); }
  else w.showStart();
  return app.exec();
}
