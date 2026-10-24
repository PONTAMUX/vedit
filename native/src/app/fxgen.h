// 生成要素(overlays の kind: speedlines / gloom)を CPU で描く(fx.py の _g_* の移植)。flash は単色なので描画側で塗る
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QString>

namespace vn::fxgen {

// 戻り値は RGBA8888(掛け前の透明度)。W×H の画面全体
QImage speedlines(int W, int H, double fps, const QJsonObject& P, double tau);
QImage gloom(int W, int H, const QJsonObject& P, double tau);

}  // namespace vn::fxgen
