// タイムライン(app.js の renderTimeline を QPainter で描く)。左のラベル列(118px)・ルーラー(36px)・各トラック・再生ヘッド・波形
// 第2部: ドラッグ移動/トリム・スナップ・範囲選択・右クリックメニュー・トラックの並べ替えと高さ・縦スクロール
#pragma once
#include <QPixmap>
#include <QWidget>
#include <QSet>
#include <cmath>
#include <functional>
#include <map>
#include <vector>
#include "editmodel.h"

class QScrollBar;
class QTimer;

namespace vui {

struct MenuItem { QString label; std::function<void()> fn; bool sep = false; };

class TimelineView : public QWidget {
    Q_OBJECT
public:
    static constexpr int kLabelW = 118, kRuler = 36;
    explicit TimelineView(EditModel* m, QWidget* parent = nullptr);
    double zoom() const { return zoom_; }
    void setZoom(double z, int anchorPx = -1);
    void zoomFit(int reservePx = 0);        // 全体が収まる拡大率(reservePx = 右端のメーター分など)
    void ensureVisible(double srcT);        // 再生ヘッドが画面外なら寄せる
    int contentWidth() const;               // ラベル列を除いた表示幅
    QString laneAtY(int y) const;           // y のトラック id(ルーラーは "ruler")
    double timeAtX(int x) const;            // 画面x → 元素材の時刻
    int hScroll() const;
    void setHScroll(int v);
    // 縦スクロール
    int vOff() const { return vOff_; }
    int vMax() const;
    int viewHeight() const;                 // トラックを出す高さ(横スクロールバーを除く)
    int fullHeight() const;
    void setVOff(int v);
    double vz() const { return vz_; }
    double lz(const QString& id) const { auto it = lz_.find(id); return it == lz_.end() ? 1.0 : it->second; }
    void setVz(double v) { vz_ = v; clampV(); update(); emit vChanged(); }
    void setLz(const QString& id, double v) { if (std::fabs(v - 1) < 0.05) lz_.erase(id); else lz_[id] = v; clampV(); update(); emit vChanged(); }
    void resetHeights() { vz_ = 1; lz_.clear(); clampV(); update(); emit vChanged(); }
    int laneHeight(const QString& id) const;
    int laneTop(const QString& id) const;
    void reloadWave() { waveTried_ = false; update(); }
    double levelAt(double srcT);            // 波形(wave.json)から srcT 付近のピーク(0..1)。波形が無ければ -1
    bool seekSnap() const { return seekSnap_; }
    void setSeekSnap(bool v);
    // 画面座標 → 内容座標と当たり判定(テスト用)
    QPoint screenOf(double srcT, const QString& lane) const;   // その時刻・トラックの中央の画面座標
    // メニュー: 既定は QMenu で出す。テストは差し替える
    std::function<void(const QPoint&, const std::vector<MenuItem>&)> menuHandler;
    std::function<bool(const QString&)> confirm;                 // 削除の確認(既定は QMessageBox)
    std::function<QString(const QString&, const QString&)> askText;   // トラック名の入力(既定は QInputDialog)
    std::vector<MenuItem> lastMenu;

signals:
    void scrubStarted();                    // ルーラー/再生ヘッドをつまんだ
    void seekRequested(double srcT);        // ルーラー/再生ヘッドのドラッグ
    void seekOutRequested(double outT);     // 同じく、空白の中(表示上の秒)
    void splitRequested(double srcT);       // 分割ツールのクリック
    void zoomChanged(double zoom);
    void vChanged();
    void imageEditRequested();              // 選択中の差し込み画像を画像編集で開く
    void photoshopRequested(const QString& file);
    void bgmPanelRequested();
    void liveEdit();                        // ドラッグ中の見た目の更新(プレビューの先読みなど)

public:
    // 描き直し。再生ヘッド以外の絵(ためておいた分)も描き直す。再生ヘッドだけ動いた時は updatePlayhead
    using QWidget::update;
    void update() { baseOk_ = false; QWidget::update(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private:
    struct El {
        enum T { Seg, Gap, Mute, Slip, Cut, Clip, Bgm } t;
        QRectF r;                   // 内容座標(スクロール前、y はトラック内ではなく全体)
        QString lane, kind, label;
        int idx = -1;
        double a = 0, b = 0;
        QChar trk;
        bool alpha = false, ghost = false, sel = false, seam = false, ingap = false;
        QColor col;
    };
    enum Drag { None, Seek, ClipD, GapItemD, MarqueeD, SegMoveD, SegTrimD, SlipD, CutRangeD, LabelD, LaneResizeD, Pending };
    struct CD {
        QString kind; int i = -1; QChar mode;
        double x0 = 0, y0 = 0, s0 = 0, e0 = 0, in0 = 0, gd = 0;
        bool moved = false;
        std::vector<double> snapPts;
        struct G { QString k; int j; double a, b; };
        std::vector<G> group;
    };
    double X(double t) { return (m_->collapsed ? m_->outTime(t) : t) * zoom_; }
    double playX();
    void build();                   // 画面に出す要素の一覧を作る
    void loadWave();
    void drawWave(QPainter& p, int top, int h);
    void drawClip(QPainter& p, const El& e, double sx, const QRect& area, double op);
    void drawRuler(QPainter& p);
    void drawLabels(QPainter& p);
    void drawLabelsRaw(QPainter& p);
    QPixmap lblPix_;                         // 左のトラック名の絵(絵文字の文字描きが 1 回 10ms ほどかかるのでためる)
    QString lblSig_;
    struct LblBtn { QRect r; QString lane; int what; };   // what: 0=ロック 1=目/M
    void updateBar();
    void clampV();
    int hitEl(const QPoint& pos) const;
    int nearEdge(const QPoint& pos, QChar* mode) const;   // 空きでも端から6px以内ならそのクリップ
    QChar edgeMode(const El& e, int px) const;
    void clipDown(QMouseEvent* ev, int ei, QChar mode);
    void startClipDrag(const El& e, QChar mode, const QPoint& pos);
    void moveClipDrag(const QPoint& pos);
    void startGapItemDrag(const El& e, QChar mode, const QPoint& pos);
    void moveGapItemDrag(const QPoint& pos);
    void startMarquee(const QPoint& pos, bool add);
    void moveMarquee(const QPoint& pos);
    void startSegMove(const El& e, const QPoint& pos);
    void moveSegMove(const QPoint& pos);
    void startSegTrim(const El& e, QChar side, const QPoint& pos);
    void moveSegTrim(const QPoint& pos);
    void startSlip(const El& e, const QPoint& pos);
    void moveSlip(const QPoint& pos);
    void seekAt(const QPoint& pos, bool shift);
    double snapT(double t);
    void showMenu(const QPoint& global, const std::vector<MenuItem>& items);
    void clipMenu(const QPoint& global);
    void trackMenu(const QPoint& global, const QString& lane);
    void rightClick(QMouseEvent* ev);
    bool selOnSeg(double a, double b, QChar trk) const;
    EditModel* m_;
    double zoom_ = 20;
    QScrollBar* hbar_;
    int vOff_ = 0;
    double vz_ = 1;
    std::map<QString, double> lz_;
    std::vector<LaneDef> lanes_;
    QPixmap base_;                           // 再生ヘッド以外の絵(Qt の QPainter 文書: 重い描画は QPixmap にためる)
    bool baseOk_ = false;
    int baseSc_ = -1, baseV_ = -1;
    int phX_ = -100000;                      // 最後に描いた再生ヘッドの x
    void paintBase(QPainter& p);
    void updatePlayhead();
    int pin_ = 0;                            // >0 の間は lanes_ を使う(build/paintEvent の中)
    std::vector<LaneDef> lanesNow() const;
    std::vector<El> els_;
    std::vector<double> splitsV_;                    // V1/A1 の分割線(内容座標 x)
    std::vector<std::pair<QString, double>> clipSplits_;
    std::vector<LblBtn> lbl_;
    std::vector<LblBtn> lblCache_;
    Drag drag_ = None;
    bool seekSnap_ = false;
    bool waveTried_ = false;
    int waveRate_ = 0;
    std::vector<unsigned char> peaks_;
    QColor waveCol_;
    QString waveFor_;
    // ドラッグの状態
    CD cd_;
    QPoint press_;
    double pressSrcT_ = 0;
    // 範囲選択
    QRectF mq_;                                      // 内容座標
    QSet<QString> mqBase_;
    std::vector<SegSel> mqSeg_;
    std::vector<double> mqGap_;
    bool mqBgm_ = false;
    QStringList mqTrk_;
    // セグメント移動
    double sgA_ = 0, sgB_ = 0;
    OVal sgBaseCuts_, sgBaseGaps_;
    bool sgMoved_ = false, sgHave_ = false;
    // トリム
    QChar stSide_;
    double stFixed_ = 0, stEdge_ = 0;
    int stCut_ = -2;
    bool stMoved_ = false;
    // スリップ
    int slIdx_ = -1;
    double slD0_ = 0;
    bool slMoved_ = false;
    // 範囲カット
    double ctA_ = 0, ctB_ = 0;
    // ラベルのドラッグ
    QString lbLane_;
    int lbVn_ = 0, lbSlot_ = -1, lbY0_ = 0, lbCurY_ = 0;
    bool lbOn_ = false;
    int rzH0_ = 0;
    QString rzLane_;
    QTimer* autoTimer_ = nullptr;
    int lastX_ = 0;
    bool lastShift_ = false;
};

}  // namespace vui
