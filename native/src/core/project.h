// project.json の読み込み(AGENTS.md の「project.json」節。vedit/project.py・render.py の該当部分の移植)
#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <vector>
#include "timeutil.h"

namespace vn {

struct OverlaySpec {
    QString file;            // 解決前の書き方
    QString kind;
    double start = 0, end = 0;
    bool hasPos = false;
    QString posName;         // full / center / top-left ... (posIsXY でなければ)
    bool posIsXY = false;
    double px = 0, py = 0;   // 1080p 基準 px
    bool hasScale = false;
    double scale = 0.3;
    QString fit;
    double in = 0;
    QString inAnim, outAnim; // fade / slide-left|right|up|down のみ(それ以外は none 扱い)
    double animDur = 0.3;
    int track = 2;
    bool hasGapAt = false;
    double gapAt = 0;
    bool audio = false;
    double volumeDb = 0;
    QString gen;             // file なしの生成要素(speedlines / gloom / flash)
    QJsonObject raw;         // 元の指定そのまま(エフェクトの計算に使う)
    bool sub = false;        // comp の中の層(start / end は comp の頭から。自分では画面に出ない)
    int compParent = -1;
    std::vector<int> subs;   // comp の中の層(overlays の番号。トラック順)
    bool fxPath = false;     // fx.py の needs_bake: ffmpeg の式ではなくエフェクトの計算で動かす
    int telop = -1;          // gen="telop"(動きを付けたテロップ。fx.py の telop_items): Project::telops の番号(comp の中の text 層は -1)
    QString subProject;      // gen="subproj"(kind:comp + project): 別プロジェクトの解決前の指定
};

struct SfxSpec {         // 効果音(Plan.sfx)
    QString file;
    double start = 0, end = 0, in = 0, gainDb = 0;
    bool hasEnd = false;
    bool hasGapAt = false;
    double gapAt = 0;
};

struct BgmSpec {
    bool on = false;
    QString file;
    double volumeDb = -23.5;
    bool duck = false;
};

struct SlipSpec { double a, b, d; };   // aslip(元素材 a〜b の音を out(a)+d へ)

// テロップ / トピック(ass.py の入力)。fx・keys などの段階4以降の指定は読まない(needsBake で印だけ付ける)
struct SubSpec {
    QString text;
    double start = 0, end = 0;
    bool hasGapAt = false;
    double gapAt = 0;
    QString style = "telop";
    bool hasPos = false;
    double px = 0, py = 0;     // 画面比(0〜1)
    QString anim = "none";     // telop: fade|pop|slide-up のみ(それ以外は none)。topic: none 以外なら fade
    double animDur = 0.2;
    QJsonObject raw;           // telop: 元の指定そのまま(needsBake のものを gen="telop" に渡す)
    bool needsBake = false;    // Python 版では fx.py で焼く(fx・keys・in_anim 等付き)。ASS には出さず、gen="telop" の差し込みとして描く
};

struct Project {
    QString path, dir;
    QString source;          // 解決前
    QString timebase = "source";
    TimeMap::Ranges cuts, gaps, vmute;   // gaps は (時刻, 長さ)
    QSet<QString> hide, mute;   // mute: a1 / se / bgm
    TimeMap::Ranges amute;
    std::vector<SlipSpec> aslip;
    std::vector<SfxSpec> sfx;
    BgmSpec bgm;
    double audioFadeMs = 8;   // cut_fx.audio_fade_ms
    std::vector<OverlaySpec> overlays;
    std::vector<SubSpec> telops, topics;
    QJsonArray transitions;  // transitions
    QJsonObject cutFx;       // cut_fx
    QJsonArray v1fx;         // v1_fx(背景のエフェクト)
    QJsonObject styles;      // styles の上書き
    double outFps = 0;       // output.fps (0=素材のまま)
    QString error;

    QString cacheDir() const;   // <名前>.cache(project.py の cache_dir。作らない)
    QString resolve(const QString& p) const;
    static bool load(const QString& path, Project* out);
    static bool loadBytes(const QString& path, const QByteArray& bytes, Project* out);   // 保存せずに、編集中の内容(JSON のバイト列)から読む
};

}  // namespace vn
