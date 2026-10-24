// 編集画面の小さな部品: 縦スクロール(#vscroll 14px)・オーディオメーター(#meter 68px)・モーダル・コマ確認の重ね表示
#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QTimer>
#include <QWidget>
#include <functional>

class QVBoxLayout;
class QPushButton;

namespace vui {

class TimelineView;

class VScrollBar : public QWidget {
    Q_OBJECT
public:
    explicit VScrollBar(TimelineView* tl, QWidget* parent = nullptr);
    QRect thumbRect() const;
    bool off() const;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override { hover_ = false; update(); }
    void enterEvent(QEnterEvent*) override { hover_ = true; update(); }
private:
    TimelineView* tl_;
    bool drag_ = false, hover_ = false;
    int y0_ = 0, s0_ = 0;
};

// Premiere 風の縦型 L/R メーター(meter.js の移植)。入力はピーク(dBFS)と平均パワー。落下・ピークホールド・クリップランプは meter.js と同じ定数
class MeterWidget : public QWidget {
    Q_OBJECT
public:
    explicit MeterWidget(QWidget* parent = nullptr);
    // 再生中に毎フレーム呼ばれる。L/R のピーク(dBFS)と平均パワー(リニア)を返す。false = 入力なし
    std::function<bool(double pk[2], double pw[2])> source;
    void setPlaying(bool on);
    void reset();                        // クリックでピークホールドとクリップを消す
    void step(double dt, const double pk[2], const double pw[2]);   // テスト用に直接進められる
    double hold(int c) const { return hold_[c]; }
    double level(int c) const { return lv_[c]; }
    bool clip(int c) const { return clip_[c]; }
    QString peakText() const;
    QString rmsText() const;
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override { reset(); }
private:
    void frame();
    double yOf(double d, int top, int bot) const;
    double lv_[2], hold_[2], pw_[2] = {0, 0};
    qint64 holdAt_[2] = {0, 0};
    bool clip_[2] = {false, false};
    bool playing_ = false;
    QTimer timer_;
    QElapsedTimer clock_;
    qint64 last_ = 0;
};

// モーダル(#modal: 画面全体を暗くして中央に箱)
class ModalOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ModalOverlay(QWidget* parent);
    QVBoxLayout* box() { return lay_; }
    void setContent(const QList<QWidget*>& w);
    void open();
    void close();
    QWidget* boxWidget() { return frame_; }
protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;
private:
    QWidget* frame_;
    QVBoxLayout* lay_;
    QList<QWidget*> cur_;
};

// モニターの上に重ねる仕上がり確認の画像(#checkimg)。右上の × で閉じる
class CheckOverlay : public QWidget {
    Q_OBJECT
public:
    explicit CheckOverlay(QWidget* parent);
    void setImage(const QImage& im);
    const QImage& image() const { return img_; }
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
private:
    QImage img_;
    QPushButton* x_;
};

// モニターの枠(黒い画面を 16:9 などの比で中央に置く。まわり 8px)。中身は本番では PreviewWidget、テストでは静止画
class StageHost : public QWidget {
    Q_OBJECT
public:
    explicit StageHost(QWidget* p);
    void setContent(QWidget* w);
    void setImage(const QImage& im);
    void setAspect(double w, double h);
    QRect stageRect() const;
    CheckOverlay* check() const { return check_; }
    QImage grabStage();                  // 画面に出ているモニターの絵(中身の QWidget があればそれ、無ければ静止画)
protected:
    void resizeEvent(QResizeEvent*) override { relayout(); }
    void paintEvent(QPaintEvent*) override;
private:
    void relayout();
    QWidget* content_ = nullptr;
    CheckOverlay* check_ = nullptr;
    QImage img_;
    double aw_ = 16, ah_ = 9;
};

}  // namespace vui
