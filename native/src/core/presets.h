// プリセット展開(vedit/presets.py の expand_data の移植)。"preset": 名前 の要素を overlays / telops / sfx / v1_fx / transitions に展開し、
// "gain_db": "match:基準[±n]" を数値にする。JSON の段階で行うので、後段は展開済みの data だけを見る
#pragma once
#include <QJsonObject>
#include <QString>

namespace vn {

// 何も展開するものが無ければ data をそのまま返す。projDir は相対パス(効果音)の基準
QJsonObject expandPresets(const QJsonObject& data, const QString& projDir);

}  // namespace vn
