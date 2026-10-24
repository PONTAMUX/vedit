#include "reviewplayer.h"
#include <QAudioOutput>
#include <QCheckBox>
#include <QLabel>
#include <QMediaPlayer>
#include <QPainter>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoFrame>
#include <QVideoSink>
#include <algorithm>
#include <cmath>

namespace vui {

SyncAct decideSync(double want, double cur, bool playing, double hostRate, bool vidPaused, double vidRate, bool seeking) {
    SyncAct a;
    if (playing && hostRate > 0) {
        // 再生中はシークしない(シークのたびにデコードし直して重く、本編の再生まで引っかかる)。小さいずれは速さを少し変えて追いつき、
        // 大きく飛んだ時(修正で変わった所・本編のシーク)だけシークする
        const double d = want - cur;
        if (std::fabs(d) > 1.5 && !seeking) { a.seek = true; a.seekTo = want; a.setRate = true; a.rate = hostRate; }
        else {
            const double r = hostRate * (1 + std::max(-0.1, std::min(0.1, d * 0.5)));
            if (std::fabs(vidRate - r) > 0.01) { a.setRate = true; a.rate = r; }
        }
        if (vidPaused) a.play = true;
    } else {
        if (!vidPaused) a.pause = true;
        if (!seeking && std::fabs(cur - want) > 0.03) { a.seek = true; a.seekTo = want; }
    }
    return a;
}

ReviewScreen::ReviewScreen(QWidget* parent) : QWidget(parent) {
    setObjectName("rv-video");
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void ReviewScreen::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (img_.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QSize s = img_.size().scaled(size(), Qt::KeepAspectRatio);
    p.drawImage(QRect(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s), img_);
}

ReviewPlayer::ReviewPlayer(QWidget* parent) : QWidget(parent) {
    setObjectName("rv-vid");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    screen_ = new ReviewScreen;
    v->addWidget(screen_);
    cap_ = new QLabel;
    cap_->setObjectName("rv-cap");
    cap_->setStyleSheet("font: 11px Consolas, 'Cascadia Mono', monospace; color: #8d8d99; padding: 3px 6px; background: #111;");
    v->addWidget(cap_);
    snd_ = new QCheckBox(QStringLiteral("修正前の音を出す"));
    snd_->setObjectName("rv-snd");
    snd_->setToolTip(QStringLiteral("修正前の動画の音を出す(本編の音は 🔊 で消せます)"));
    snd_->setFocusPolicy(Qt::NoFocus);
    snd_->setStyleSheet("margin: 4px 0; font-size: 12px;");
    v->addWidget(snd_);
    connect(snd_, &QCheckBox::toggled, this, [this](bool on) { setSound(on); });

    player_ = new QMediaPlayer(this);   // 音声出力は付けない(= 音のデバイスを開かない)
    sink_ = new QVideoSink(this);
    player_->setVideoSink(sink_);
    connect(sink_, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& f) {
        if (!f.isValid()) return;
        const QImage im = f.toImage();
        if (!im.isNull()) screen_->setImage(im);
    });
    connect(player_, &QMediaPlayer::positionChanged, this, [this](qint64 ms) {
        if (seekTarget_ >= 0 && std::fabs(ms / 1000.0 - seekTarget_) < 0.3) seekTarget_ = -1;
    });
}

ReviewPlayer::~ReviewPlayer() {
    player_->stop();
    player_->setVideoSink(nullptr);
    player_->setAudioOutput(nullptr);
}

void ReviewPlayer::setSource(const QString& path, const QString& key) {
    if (key == key_) return;
    key_ = key;
    seekTarget_ = -1;
    screen_->setImage(QImage());
    player_->setSource(QUrl::fromLocalFile(path));
}

void ReviewPlayer::clearSource() {
    if (key_.isEmpty()) return;
    key_.clear();
    player_->stop();
    player_->setSource(QUrl());
    screen_->setImage(QImage());
}

void ReviewPlayer::setSound(bool on) {
    sound_ = on;
    if (snd_->isChecked() != on) { QSignalBlocker b(snd_); snd_->setChecked(on); }
    if (on && !audio_) {
        audio_ = new QAudioOutput(this);   // ここで初めて音のデバイスを開く(利用者が音を出すと決めた時だけ)
        player_->setAudioOutput(audio_);
    } else if (!on && audio_) {
        player_->setAudioOutput(nullptr);
        delete audio_;
        audio_ = nullptr;
    }
}

void ReviewPlayer::setCaption(const QString& s) { if (cap_->text() != s) cap_->setText(s); }
QString ReviewPlayer::caption() const { return cap_->text(); }

bool ReviewPlayer::ready() const {
    switch (player_->mediaStatus()) {
    case QMediaPlayer::LoadedMedia: case QMediaPlayer::BufferingMedia: case QMediaPlayer::BufferedMedia:
    case QMediaPlayer::StalledMedia: case QMediaPlayer::EndOfMedia: return true;
    default: return false;
    }
}
double ReviewPlayer::position() const { return player_->position() / 1000.0; }
double ReviewPlayer::duration() const { return player_->duration() / 1000.0; }
double ReviewPlayer::playbackRate() const { return player_->playbackRate(); }
bool ReviewPlayer::isPlaying() const { return player_->playbackState() == QMediaPlayer::PlayingState; }
QSize ReviewPlayer::screenSize() const { return screen_->size(); }

bool ReviewPlayer::seeking() const {
    return seekTarget_ >= 0 && seekTimer_.isValid() && seekTimer_.elapsed() < 600;
}
void ReviewPlayer::doSeek(double sec) {
    seekTarget_ = sec;
    seekTimer_.restart();
    ++seeks_;
    player_->setPosition((qint64)std::llround(std::max(0.0, sec) * 1000));
}

void ReviewPlayer::sync(double want, bool playing, double hostRate) {
    if (key_.isEmpty() || !ready()) return;
    const double dur = duration();
    want = std::max(0.0, dur > 0 ? std::min(want, dur - 0.05) : want);
    const SyncAct a = decideSync(want, position(), playing, hostRate, !isPlaying(), playbackRate(), seeking());
    if (a.seek) doSeek(a.seekTo);
    if (a.setRate) player_->setPlaybackRate(a.rate);
    if (a.play) player_->play();
    if (a.pause || (!a.play && player_->playbackState() == QMediaPlayer::StoppedState)) player_->pause();   // 止まっている間も1コマ目を出すため、停止でなく一時停止にする
}

}  // namespace vui
