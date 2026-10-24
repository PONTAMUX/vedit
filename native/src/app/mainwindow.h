// 最小の窓: プレビュー + 時刻スライダー + 再生/停止(スペース)。再生中は音声が時計の親(映像は音声の位置に合わせる)
#pragma once
#include <QElapsedTimer>
#include <QMainWindow>
#include <QOpenGLWidget>
#include <QTimer>
#include "media/audioplayer.h"
#include "renderer.h"

namespace vui::bridge { class Job; }


class PreviewWidget : public QOpenGLWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(vn::Renderer* r, vn::AudioPlayer* ap, QWidget* parent = nullptr);
    ~PreviewWidget() override;
    double time() const { return t_; }
    bool playing() const { return playing_; }
public slots:
    void setTime(double t);
    void setPlaying(bool on);
    void setVolume(double gain, bool muted);   // G4: 音量・ミュート(2乗済みの係数)
    void setProcessed(bool on);                // G4: 処理後/生の音の切替(c-proc)
    void setRate(double rate);                 // G4: 倍速(L を重ねた時)
    void markDirty();                  // 編集があった: 次の描画で Renderer が新しい内容を取り込む(Renderer::setLiveSource)
    bool audioStale() const { return audioStale_; }
    bool audioLevel(double pk[2], double rms[2]);   // メーター用: AudioPlayer が出している音の L/R の peak / RMS(線形)
signals:
    void timeChanged(double t);
    void playingChanged(bool on);

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    vn::Renderer* r_;
    vn::AudioPlayer* ap_;   // 音声(無ければ nullptr → 壁時計)
    bool apOpen_ = false;
    bool audioStale_ = false;          // 音に関わる編集があった(次の再生の前に AudioPlayer を作り直す)
    void onReloaded(bool audioChanged);
    double t_ = 0, playStart_ = 0, rate_ = 1;
    bool playing_ = false;
    QElapsedTimer clock_;
};

namespace vui { class EditModel; class EditorWindow; }

// 編集画面(src/ui の EditorWindow)に、プレビュー(PreviewWidget)を差し込む。プレビューは出力時刻、編集は元素材の時刻で持つ
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(vn::Renderer* r, const QString& title);   // title = プロジェクトのパス
protected:
    void closeEvent(QCloseEvent* e) override;   // 中の編集画面には閉じる知らせが届かないので、ここで未保存を聞く
private:
    vn::Renderer* r_;
    vn::AudioPlayer audio_;
    PreviewWidget* view_;
    vui::EditModel* model_;
    vui::EditorWindow* ed_;
    vui::bridge::Job* proxyJob_ = nullptr;
    void startProxy();
};
