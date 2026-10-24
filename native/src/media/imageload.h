// 画像を1回だけ読み、出す大きさへ縮めた「透明度掛け済み RGBA8」にする(GPU テクスチャの元)
#pragma once
#include <QImage>
#include <QString>

namespace vn {
QImage loadImageScaled(const QString& path, int tw, int th);
}
