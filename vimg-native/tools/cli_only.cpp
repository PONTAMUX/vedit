#include "render.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <cstdio>
int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QString in = argv[1], out = argv[2]; Doc d; QString err;
  if (!Doc::load(in, d, &err)) { fprintf(stderr, "%s\n", qPrintable(err)); return 1; }
  QStringList miss; QImage im = vimg::renderDoc(d, QFileInfo(in).absolutePath(), true, &miss);
  vimg::writePng(im, out, &err); return 0;
}
