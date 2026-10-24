// エフェクトの値の計算(vedit/fx.py の移植のうち、画面を使わない部分)。
// イージング・キーフレーム・式つきの値(pval)・出入りの動き・揺れ/寄り/流れ・文字アニメーションの値
#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <vector>

namespace vn::fx {

// ---- 値の読み出し(Python の float(x) 相当)
double num(const QJsonValue& v, double def = 0.0);
bool truthy(const QJsonValue& v);
bool parseColor(const QJsonValue& v, int rgb[3], const int def[3]);   // "#RRGGBB" か [r,g,b]。null は def

// ---- イージング
double ease(const QString& name, double u);   // u は 0〜1 に収める。知らない名前は linear
double smooth(double u);
double backEase(double u, double s = 1.70158);
double bounceEase(double u);

// ---- キーフレーム・式
struct Dyn { double dur = 0, fps = 30; };       // 式の dur / fps
QJsonValue keyValue(const QJsonArray& keys, double t, const QString& loop);
bool isDyn(const QJsonValue& v);
bool hasDyn(const QJsonValue& v);
QJsonValue pval(const QJsonValue& v, double tau, const Dyn& d, long long seed = 0, double base = 0);
long long seedOf(const QString& name, int extra = 0);

// ---- 既定値つきのパラメータ(cat = anim|fx|gen|trans|text)。知らない名前は ok=false
QJsonObject params(const char* cat, const QString& name, const QJsonObject& user, bool* ok = nullptr);

// ---- 動き
struct St {
    double sx = 1, sy = 1, rot = 0, dx = 0, dy = 0, op = 1, blur = 0, feather = 0, ax = 0.5, ay = 0.5;
    double clip[4] = {-9, -9, 10, 10};
    bool clipped() const { return !(clip[0] == -9 && clip[1] == -9 && clip[2] == 10 && clip[3] == 10); }
    bool identity() const;
    void clipTo(double l, double t, double r, double b, bool hl, bool ht, bool hr, bool hb);
};

struct PixFx { QString name; QJsonObject P; double t = 0; };
struct AnimUse { bool in = true; QString name; QJsonObject P; double dur = 0.3; };
struct FxUse { QString type; QJsonObject P; double s = 0, e = 0; bool hasE = false, dyn = false; long long seed = 0; bool state = false; };

// 要素1つの動きの指定(overlays / telops の1項目から作る)
struct ElemFx {
    QJsonObject it;
    double a = 0, b = 0;                 // 出力上の時刻
    std::vector<AnimUse> anims;
    std::vector<std::pair<bool, double>> typer;   // typewriter: (in?, 1文字の秒)
    QJsonObject keys;
    std::vector<FxUse> fxs;
    void init(const QJsonObject& item, double a_, double b_);
};

// 計算に使う画面側の値
struct StateCtx {
    double W = 1920, H = 1080, fps = 30, k = 1;
    double rx = 0, ry = 0, rw = 0, rh = 0;   // 絵を置く場所(E.rect)
    double bbw = 1, bbh = 1;                  // 中身の外枠の大きさ(wipe 用)
};
void computeState(const ElemFx& e, double tau, const StateCtx& c, St* st, std::vector<PixFx>* pix);
void shakeXY(double tau, const QJsonObject& P, double k, double* dx, double* dy);

// E.rand(name, seed, n): Random(seed) の uniform(-1,1) を n 個
std::vector<double> randList(long long seed, int n);

// ---- 文字アニメーション
void textAnimState(const QString& type, const QJsonObject& P, St* st, double u, double tau, int i, int n, double k, double fps);
std::vector<int> charOrder(int n, const QString& order, long long seed = 11);

}  // namespace vn::fx
