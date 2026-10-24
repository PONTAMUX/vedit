// 修正点パネルに埋め込む「修正前の動画」(review.js の vid / tick の移植)。
// 再生ヘッドの位置に合わせて動かす: 止まっている間は位置を合わせる / 再生中はシークせず ±10% の速さで追いつく / 1.5 秒以上ずれたらシーク。
// 音は既定で出さない(音声出力を作らない = デバイスも開かない)。「修正前の音を出す」を入れた時だけ出力を作る。
#pragma once
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <QWidget>
#include <memory>

class QAudioOutput;
class QCheckBox;
class QLabel;
class QMediaPlayer;
class QVideoSink;

namespace vui {

// 合わせ方の判断(review.js tick の再生中/停止中の分岐。画面や再生部品に依存しないので単体で試せる)
struct SyncAct {
    bool seek = false;
    double seekTo = 0;
    bool setRate = false;
    double rate = 1;
    bool play = false;
    bool pause = false;
};
// want: 合わせたい位置(秒) cur: 今の位置 playing/hostRate: 編集画面側の再生状態と倍速 vidPaused/vidRate: 動画の状態 seeking: シーク中か
SyncAct decideSync(double want, double cur, bool playing, double hostRate, bool vidPaused, double vidRate, bool seeking);

class ReviewScreen : public QWidget {   // 16:9 の黒い画面に絵を中央・縦横比そのままで出す
public:
    explicit ReviewScreen(QWidget* parent = nullptr);
    void setImage(const QImage& im) { img_ = im; update(); }
    const QImage& image() const { return img_; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * 9 / 16; }
    QSize sizeHint() const override { return QSize(280, 158); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QImage img_;
};

class ReviewPlayer : public QWidget {
    Q_OBJECT
public:
    explicit ReviewPlayer(QWidget* parent = nullptr);
    ~ReviewPlayer() override;
    void setSource(const QString& path, const QString& key);   // key が同じなら何もしない(軽い版ができた時だけ作り直す)
    QString key() const { return key_; }
    void clearSource();
    void setSound(bool on);
    bool sound() const { return sound_; }
    bool audioOpen() const { return audio_ != nullptr; }
    void setCaption(const QString& s);
    QString caption() const;
    void sync(double want, bool playing, double hostRate);     // 80ms ごとに呼ぶ
    bool ready() const;
    double position() const;
    double duration() const;
    double playbackRate() const;
    bool isPlaying() const;
    int seekCount() const { return seeks_; }
    bool hasFrame() const { return !screen_->image().isNull(); }
    QImage frameImage() const { return screen_->image(); }
    QSize screenSize() const;

private:
    bool seeking() const;
    void doSeek(double sec);

    QMediaPlayer* player_ = nullptr;
    QVideoSink* sink_ = nullptr;
    QAudioOutput* audio_ = nullptr;
    ReviewScreen* screen_ = nullptr;
    QLabel* cap_ = nullptr;
    QCheckBox* snd_ = nullptr;
    QString key_;
    bool sound_ = false;
    double seekTarget_ = -1;
    QElapsedTimer seekTimer_;
    int seeks_ = 0;
};

}  // namespace vui
