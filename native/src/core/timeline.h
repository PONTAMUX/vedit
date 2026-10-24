// 出力時刻 T → 元素材の時刻・出す層の一覧(位置・大きさ・フェード/スライド)。画面とは独立
// 計算は render.py の build()(ffmpeg の式)と vengine/src/comp.rs の pos_at / fade_at に合わせる
#pragma once
#include <functional>
#include <vector>
#include <QJsonObject>
#include "project.h"
#include "timeutil.h"

namespace vn {

struct MediaProps {
    bool ok = false, isImage = false;
    int w = 0, h = 0;
    double fps = 0, start = 0, duration = 0;
};
using MediaProvider = std::function<MediaProps(const QString& absPath)>;

class Timeline {
public:
    struct PieceInfo { bool gap; double a, b, o0, o1; };   // gap: a,b 無効
    struct Geom {
        bool done = false;
        int lw = 0, lh = 0;            // 層(差し込みの枠)の大きさ
        int cx = 0, cy = 0, cw = 0, ch = 0;   // 枠の中で絵を置く四角
        double u0 = 0, v0 = 0, u1 = 1, v1 = 1;
        bool pad = false;              // 枠の残りを黒で埋める(全画面で縦横比が違うとき)
        double bx = 0, by = 0;         // アニメ前の位置
        int texW = 0, texH = 0;        // 画像テクスチャの大きさ(これ以下に縮めて持つ)
    };
    struct LayerSpec {
        int ov;                        // Project::overlays の番号
        double a, b;                   // 出力上の時刻
        QString path;
        QString gen;                   // 生成要素の種類(file なし)
        bool isImage = false;
        bool sub = false;              // comp の中の層(画面には comp が描く)
        MediaProps m;
        bool propsDone = false, bad = false;
        Geom g;
    };
    struct Shake { double a, b; QJsonObject P; };
    std::vector<Shake> shakes;         // v1_fx の揺れ(出力時刻)
    double shakeZoom = 1;              // 揺らしても端が見えないだけの拡大率
    void shakeAt(double T, double* dx, double* dy) const;   // その時刻の揺れ量(px)。揺れなしなら false 相当で 0
    bool shaking(double T) const;
    // トランジション(fx.py transitions / joint)。o=継ぎ目の出力時刻、jA/jB=継ぎ目の前後の元素材の時刻
    struct Trans { double o, a, b; QString name; QJsonObject P, se; double jA = 0, jB = 0; bool hasA = false, hasB = false; bool src = false; };
    std::vector<Trans> trans;
    QString dipColor;                  // cut_fx.video が dip-black / dip-white の色("" なら無し)
    double dipDur = 0.12;
    struct V1State { bool black = true; int piece = -1; double srcT = 0; };
    struct LayerState { int li; double x, y, alpha, srcT; };

    bool init(const Project& p, MediaProvider prov);
    QString error;

    TimeMap timemap;                   // カット・空白の時刻対応(音声の換算用)
    int W = 0, H = 0;
    double fps = 30, total = 0;
    MediaProps src;
    std::vector<PieceInfo> pieces;
    std::vector<LayerSpec> layers;     // 重なり順(トラック番号の小さい順 → リスト順)
    struct SubPlaced { int spec; double a, b; };   // Project::telops / topics の番号と、出力上の時刻(render.py の conv)
    std::vector<SubPlaced> telopsOut, topicsOut;

    int pieceAt(double T) const;
    V1State v1At(double T) const;
    std::vector<int> activeLayers(double T) const;                  // その時刻に出ている層(重なり順)
    std::vector<int> layersIn(double t0, double t1) const;          // t0〜t1 に出る層
    bool prepare(int li);                                           // 大きさを調べて Geom を作る。読めない素材は false
    LayerState state(int li, double T);                             // prepare 済みの層の、時刻 T の位置・不透明度・動画内の時刻
    double snapUp(double x, double start, double f) const;

private:
    Project p_;
    MediaProvider prov_;
    std::vector<std::pair<double, double>> black_;
    bool hideV1_ = false;
    void makeGeom(LayerSpec& L);
};

}  // namespace vn
