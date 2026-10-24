// 描画: 1コマ分の層(V1 本編 + 差し込み)を W×H の FBO に重ねる。YUV→RGB はシェーダ。
// QOpenGLWidget の中でも、窓なし(QOffscreenSurface)でも同じものを使う。
#pragma once
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions_3_3_Core>
#include <QSet>
#include <atomic>
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <vector>
#include "core/ass.h"
#include "core/fxcore.h"
#include "core/timeline.h"
#include "media/subrender.h"
#include "media/videostream.h"

namespace vn {

// 画素エフェクト用のパスに渡す値(renderer_post.cpp)
struct PostArgs {
    float a[4] = {}, b[4] = {}, c[3] = {}, d[3] = {};
    int nm = 0;
    float mk[4][4] = {}, mk2[4][4] = {};
    int ng = 0;
    float g[128][3] = {};
    int np = 0;
    float poly[64][2] = {};   // 多角形マスクの頂点(全マスク分を詰める)
};

class Renderer : protected QOpenGLFunctions_3_3_Core {
public:
    Renderer();
    ~Renderer();   // GL コンテキストを current にして破棄すること

    bool load(const QString& projectPath, QString* err);   // GL 不要
    // 編集中の内容を反映する(保存しない・デコーダやキャッシュは使い回し、変わった所だけ作り直す)。GL コンテキストが current の状態で。
    // 失敗(JSON が不正など)の時は今の状態のまま false。
    bool reload(const QByteArray& json, QString* err);
    // 「編集中の内容」の取り出し口。invalidate() した後の次の render() の頭で、これを呼んで reload する(1コマの間に何度編集しても1回)
    void setLiveSource(std::function<QByteArray()> src) { live_ = std::move(src); }
    void invalidate() { dirty_ = true; }
    bool applyLive();                 // invalidate 済みなら今すぐ reload する(render() も頭で呼ぶ)。GL が current の時だけ。反映したら true
    bool hasPending() const { return dirty_.load(); }
    void setOnReloaded(std::function<void(bool audioChanged)> f) { onReloaded_ = std::move(f); }
    void setProjectPath(const QString& p) { pathOverride_ = p; }
    // プレビュー用の軽い映像(proxy.mp4、キーフレーム0.5秒おき)。src が今の元素材と同じ間だけ本編のデコードに使う(GUI だけ。--frame は元動画)
    void setPreviewSource(const QString& src, const QString& proxy) { pvFor_ = src; pvPath_ = proxy; dirty_ = true; }
    const QString& sourcePath() const { return srcPath_; }   // 別名で保存した後など、相対パスの基準が変わる時
    double lastReloadMs() const { return reloadMs_; }
    int reloadCount() const { return reloads_; }
    bool initGL();                                          // コンテキストが current の状態で
    void releaseGL();

    // 出力時刻 T(コマの時刻)を FBO に描く。wait=true なら全部そろうまで待つ(静止画用)
    void render(double T, bool wait);
    QImage grab();                                          // FBO → 画像(上が先頭)
    void blit(unsigned dstFbo, int x, int y, int w, int h); // FBO を拡大縮小して dst へ

    Timeline& timeline() { return tl_; }
    const Project& project() const { return proj_; }
    int W() const { return tl_.W; }
    int H() const { return tl_.H; }
    int staleCount() const { return stale_; }               // 間に合わず古い絵(または無し)で出したコマ数
    double lastFrameLag() const { return lag_; }
    bool decodersWarm() const { return warmDone_.load(); }   // 差し込み動画の NVDEC を先に作り終えた(prober スレッドが load 直後にやる)
    bool waitDecodersWarm(int ms);
    void setPrewarm(bool on) { prewarm_ = on; }            // 止まっている絵の描画でも先読みを始める(再生前の仕込み)
    int interopFrames() const { return interop_; }          // GPU の中だけで渡したコマ数
    unsigned frameTex() const { return fbo_ ? fbo_->texture() : 0; }   // 描いた絵のテクスチャ(外部 project の comp が使う)

private:
    struct GpuVideo {
        unsigned y = 0, uv = 0;
        int w = 0, h = 0;
        VFrame::Kind kind = VFrame::NV12;
        uint64_t id = 0;
        int cs = 0;
        bool full = false;
        double pts = -1;
        cuda::GlTarget cu;   // CUDA に登録した y・uv テクスチャ(interop 用)
    };
    struct ImgTex { unsigned tex = 0; int w = 0, h = 0; uint64_t used = 0; float bb[4] = {0, 0, 1, 1}; };   // bb: 不透明部分の外枠(0..1)
    struct SubTex { unsigned tex = 0; int w = 0, h = 0, x = 0, y = 0; uint64_t used = 0; };
    struct Slot { std::unique_ptr<VideoStream> s; int piece = -1; };
    struct OvStream { std::unique_ptr<VideoStream> s; GpuVideo gpu; };

    void drawBase(const Timeline::V1State& v, bool wait, double T);
    void drawLayer(int li, double T, bool wait);
    // matte / parent / blend(fx.py の Elem.render 後半)。ov 番号ごとの指定
    struct Compose { bool hasMatte = false; int matteOv = -1; QString matteMode; std::vector<int> parents; int blend = 0; bool hidden = false; };
    std::map<int, Compose> comp_;
    bool compBuilt_ = false;
    void buildCompose();
    int layerOfOv(int ov) const;
    int renderComp(int li, double T, bool wait);                                 // comp の中の層を重ねた絵(予約キャンバス)。-1 なら無し
    int compBBLi_ = -1;                                                          // 直近の comp の中身の外枠(画面 px。readback で求める)
    double compBB_[4] = {};
    int compCv_ = -1;                                                            // いま描いている comp の中身(drawContent が貼る)
    int layerToCanvas(int li, double T, bool wait, double* alpha, bool force);   // -1=何も出ない、-2=直接描いた(force=false の素の層)
    int stashCanvas(int r);                                                      // キャンバスを予約キャンバスへ写す(入れ子の描画で潰されないように)
    int applyCompose(int li, int r, double T, bool wait);                        // matte → parent → blend。戻り値: キャンバス番号(-1 なら出さない)
    bool parentAffine(int li, double T, float A[4], float D[2]);
    void clearXform();
    void restoreXform();
    bool clipOn_ = false;                      // wipe の切り抜き(動かす前の外枠に対する比)
    double clip_[4] = {}, clipFe_ = 0, clipBox_[4] = {};
    float xfM_[4] = {1, 0, 0, 1}, xfP_[2] = {0, 0}, xfD_[2] = {0, 0};   // setupFx が入れた動きの行列(マスク後に掛け直す用)
    static constexpr int kTopTrack = 999;   // fx.py TOP_TRACK: トランジションは track 999 に重ねる
    void drawTrans(double T);
    void drawSrcTrans(double T, bool wait);
    void prefetchTrans(double T);
    GpuVideo* transFrame(int side, int idx, const Timeline::Trans& t, double tau, bool wait, bool* black);
    void drawWipe(const QJsonObject& P, double tau);
    void drawIris(const QJsonObject& P, double tau);
    void drawGen(int li, double T, double alpha, bool wait);
    QImage makeGenImage(int li, double tau) const;
    QJsonObject genParams(int li, double tau, bool* animated) const;
    void requestGen(const QString& key, int li, double tau);
    void prefetchGen(int li, double T);
    unsigned genTex(const QString& key, int li, double tau, bool wait);
    void drawSolid(const int rgb[3], double x, double y, double w, double h, double alpha);
    double setupFx(int li, double T, double bx, double by, double bw, double bh);
    void drawQuad(int mode, double x, double y, double w, double h, double u0, double v0, double u1, double v1, double alpha,
                  unsigned t0 = 0, unsigned t1 = 0, const GpuVideo* gv = nullptr);
    void drawVideo(const GpuVideo& g, double x, double y, double w, double h, double u0, double v0, double u1, double v1, double alpha);
    void upload(GpuVideo& g, const VFrame& f);
    bool uploadDev(GpuVideo& g, const VFrame& f);
    void freeVideo(GpuVideo& g);
    VideoStream* baseFor(int piece, double startT, double ahead);
    double pieceStartSrc(int piece) const;
    OvStream* overlayStream(int li, double T);
    unsigned imageTex(int li, bool wait, bool allowLoad = true);
    bool texBBox(unsigned tex, float bb[4]) const;
    void requestImage(const QString& key, const QString& path, int tw, int th);
    unsigned uploadImage(const QString& key, const QImage& im);
    // 動きを付けたテロップ(gen="telop"。renderer_telop.cpp)。絵は専用の SubRender(libass)で変種ごとに描き、文字ごとの動きは CPU で重ねる
    struct TelopLayer {
        int spec = -1;
        bool animator = false;
        std::vector<std::pair<bool, double>> typer;   // typewriter: (in?, 1文字の秒)
        std::vector<int> cuts;                        // 空白でない文字の番号
        std::vector<QJsonObject> anims;               // animator(既定値を足したもの)
        int slot0 = 0;
        std::mutex mu;
        std::atomic<bool> ready{false};
        std::vector<SubRender::Img> var;              // 変種の絵(typewriter / 通常)
        struct Ch { bool ok = false; QImage o, f; int x0 = 0, y0 = 0; };
        std::vector<Ch> chars;                        // animator: 文字ごとの縁・文字(外枠で切ったもの。premultiplied)
        bool hasBB = false;
        int bb[4] = {0, 0, 0, 0};                     // 全部出した時の外枠(anchor の基準)
    };
    std::map<int, std::shared_ptr<TelopLayer>> telop_;
    std::map<int, std::unique_ptr<Renderer>> subr_;   // 外部 project の comp(gen="subproj")ごとの描画器
    void prepSubProj(double T, bool wait);
    std::unique_ptr<SubRender> telopSubs_;
    void initTelops();
    bool telopEnsure(int li);
    QImage telopImage(int li, double tau);
    long long telopFk(int li, double tau) const;
    bool telopBox(int li, bool wait, double* bx, double* by, double* bw, double* bh);
    void drawSubs(double T, bool wait);
    void prefetchSubs(double T);
    void prefetch(double T, double horizon = 2.5);
    void trimImages();

    // 画素エフェクト(renderer_post.cpp)
    struct Canvas { unsigned fbo = 0, tex = 0; };
    struct V1Fx { double a = 0, b = 0; QString type; QJsonObject P; };
    bool postInit();
    void postRelease();
    void bindCanvas(int i, bool clear);
    void copyToCanvas(int i);
    void compositeCanvas(int i, double alpha);
    void postRun(int mode, int dst, int a, int b, const PostArgs& A);
    int blurIdx(int src, double sigma, int keep = -1);
    int fxPass(const fx::PixFx& f, int cur);
    int chainFx(int cur, const std::vector<fx::PixFx>& list, double blurSigma, const QJsonArray& masks, double maskOpacity);
    void initV1Fx();
    void drawV1Fx(double T);
    void drawContent(int li, double T, double alpha, bool wait, const Timeline::LayerState& st);
    static constexpr int kGenCv = 8;      // 順に使い回すキャンバス(0..7)。8 以降は reserveCanvas で確保する(必要になるまで作らない)
    Canvas cv_[12];
    int clipPass(int cur);
    void ensureCanvas(int i, bool f16 = false);
    int reserveCanvas(bool f16 = false);
    void releaseCanvas(int i);
    int freeIdx(std::initializer_list<int> used);
    bool cvBusy_[12] = {};
    unsigned cvF16_ = 0;
    void fxBox(int li, bool wait, const Timeline::LayerState& st, double* bx, double* by, double* bw, double* bh);
    int canvasPass(int li, double T, bool wait, const Timeline::LayerState& st, const QJsonArray& masks, double maskOp,
                   const std::vector<fx::PixFx>& list, double blurSigma);
    int layerCanvas(int li, double T, bool wait, const Timeline::LayerState& st, const QJsonArray& masks, double maskOp);
    unsigned postProg_ = 0;
    std::map<QString, int> pu_;
    std::vector<V1Fx> v1fx_;
    bool postOn_ = false;                 // setupFx が決める: この層は画素エフェクトを通す
    std::vector<fx::PixFx> postFx_;
    double postBlur_ = 0;

    void startProber();
    void stopProber(bool wait = true);   // wait=false: 止めるよう言うだけで待たない(終わったものは次に片付ける)
    Timeline tl_;
    Project proj_;
    struct Prober { vn::Thread th; std::shared_ptr<std::atomic<bool>> quit, done; };
    std::vector<Prober> probers_;    // 末尾が今の分。前の分は止まるのを待たずに残し、終わったら join する
    std::atomic<double> playhead_{0};
    std::atomic<bool> warmDone_{false}, warmStarted_{false};
    QString srcPath_, pvFor_, pvPath_;
    std::string openedDecode_;   // 今のデコーダが開いているファイル(プロキシの切り替え判定用)
    std::string decodePath() const { return (!pvPath_.isEmpty() && pvFor_ == srcPath_ ? pvPath_ : srcPath_).toUtf8().toStdString(); }
    unsigned prog_ = 0, vao_ = 0;
    int uRect_ = -1, uCanvas_ = -1, uUV_ = -1, uMode_ = -1, uAlpha_ = -1, uYoff_ = -1, uYscale_ = -1, uCoff_ = -1,
        uCscale_ = -1, uKr_ = -1, uKb_ = -1, uXA_ = -1, uXP_ = -1, uXD_ = -1, uNOps_ = -1, uOpA_ = -1, uOpB_ = -1, uOpC_ = -1, uSolid_ = -1, uSolid2_ = -1, uTr_ = -1, uCv_ = -1;
    std::map<int, QString> lastGen_;
    OvStream tsrc_[2];       // 前後の映像を使うトランジション用(A=前 / B=後)
    int tsIdx_[2] = {-1, -1};
    std::map<int, fx::ElemFx> efx_;
    std::unique_ptr<QOpenGLFramebufferObject> fbo_;
    GpuVideo base_;
    std::vector<Slot> slots_;
    std::map<int, OvStream> ovs_;
    std::map<QString, ImgTex> imgs_;
    SubPlan subPlan_;
    std::unique_ptr<SubRender> subs_;
    std::map<QString, SubTex> subTex_;
    QMutex imgMu_;
    QHash<QString, QImage> ready_;
    QSet<QString> pending_, failed_;
    uint64_t useCounter_ = 0;
    int curPiece_ = 0;
    int stale_ = 0;
    int interop_ = 0;
    bool prewarm_ = false;
    double lag_ = 0;
    bool glOk_ = false;
    std::function<QByteArray()> live_;
    std::function<void(bool)> onReloaded_;
    std::atomic<bool> dirty_{false};
    std::atomic<int> inflight_{0};    // thread pool に出した絵の読み込み・生成の数
    // 生成(makeGenImage は proj_ / tl_ を読む)の世代。reload で進め、古い世代の待ちは走らずに捨てる。走っている分だけ reload が待つ
    int genEpoch_ = 0;                // imgMu_ で守る
    QSet<QString> genPending_;        // imgMu_ で守る。pending_ のうち生成の分
    std::atomic<int> genRunning_{0};
    double reloadMs_ = 0;
    int reloads_ = 0;
    QString telopSig_;
    QString pathOverride_;
};

}  // namespace vn
