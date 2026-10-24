#include "imageload.h"
#include <QImageReader>

namespace vn {

QImage loadImageScaled(const QString& path, int tw, int th) {
    QImageReader rd(path);
    rd.setAutoTransform(false);
    QImage im = rd.read();
    if (im.isNull()) return QImage();
    if (im.format() != QImage::Format_ARGB32_Premultiplied && im.format() != QImage::Format_RGBA8888_Premultiplied)
        im = im.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (tw > 0 && th > 0 && (im.width() != tw || im.height() != th))
        im = im.scaled(tw, th, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return im.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

}  // namespace vn
