// テロップ / トピックバーを ASS 字幕にする(vedit/ass.py の移植。見た目は書き出しと同じ libass で描く)
// と、「いつ絵が変わるか」の判定(キャッシュのキー)。画面とは独立
#pragma once
#include <QString>
#include <vector>
#include "project.h"
#include "timeline.h"

namespace vn {

QString buildAss(const Project& p, int W, int H, const std::vector<Timeline::SubPlaced>& telops,
                 const std::vector<Timeline::SubPlaced>& topics);

// 動きを付けたテロップの絵の素(fx.py _ass_render の変種): variant j は j*0.1〜(j+1)*0.1 秒に出る。
// hideFrom/showOnly は文字の番号(-1=なし)、layers: 0=両方 1=縁だけ 2=文字だけ
struct TelopVariant { int spec; int hideFrom = -1, showOnly = -1, layers = 0; };
QString buildTelopVariantsAss(const Project& p, int W, int H, const std::vector<TelopVariant>& vars);

class SubPlan {
public:
    void init(const Project& p, const Timeline& tl);
    bool empty() const { return items_.empty(); }
    const QString& ass() const { return ass_; }
    static long long nowMs(double T);
    // その時刻に出ている要素の組と、動いている最中の要素の時刻から作る文字列。空 = 何も出ない。同じ文字列 = 同じ絵
    QString keyAt(long long nowMs) const;

private:
    struct Item { int id; long long a, b; int inMs, outMs; };   // a,b: ASS の時刻(1/100 秒に丸めた ms)
    std::vector<Item> items_;
    QString ass_;
};

}  // namespace vn
