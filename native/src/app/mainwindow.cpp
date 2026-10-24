#include "mainwindow.h"
#include "ui/editmodel.h"
#include "ui/editorwindow.h"
#include "ui/pybridge.h"
#include <QCloseEvent>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <cmath>

PreviewWidget::PreviewWidget(vn::Renderer* r, vn::AudioPlayer* ap, QWidget* parent) : QOpenGLWidget(parent), r_(r), ap_(ap) {
    setMinimumSize(640, 360);
    r_->setOnReloaded([this](bool audioChanged) { onReloaded(audioChanged); });
}

PreviewWidget::~PreviewWidget() {
    makeCurrent();
    r_->releaseGL();
    doneCurrent();
}

void PreviewWidget::initializeGL() {
    r_->initGL();
    r_->setPrewarm(true);
}

void PreviewWidget::setTime(double t) {
    t_ = std::min(std::max(0.0, t), r_->timeline().total);
    if (playing_) {
        playStart_ = t_; clock_.restart();
        if (ap_) ap_->seek(t_);   // 音は尾をフェードアウトして飛ぶ
    }
    update();
}

void PreviewWidget::setPlaying(bool on) {
    if (on == playing_) return;
    if (on && t_ >= r_->timeline().total - 1.0 / r_->timeline().fps) t_ = 0;
    if (on && audioStale_ && ap_) {   // 音に関わる編集の後の最初の再生: ミキサーを新しい内容で作り直す
        QString err;
        if (!ap_->reinit(r_->project(), r_->timeline(), &err)) qWarning("audio reinit: %s", qPrintable(err));
        audioStale_ = false;
    }
    playing_ = on;
    if (on) { playStart_ = t_; clock_.restart(); }
    if (ap_) {
        if (on) {
            if (!apOpen_) apOpen_ = ap_->open(vn::AudioPlayer::Sink::Device);   // 初回の再生で出力を開く
            if (apOpen_) ap_->play(t_);
        } else {
            ap_->pause();
        }
    }
    emit playingChanged(on);
    update();
}

void PreviewWidget::setVolume(double gain, bool muted) {
    if (ap_) ap_->setVolume(gain, muted);
}

void PreviewWidget::setProcessed(bool on) {
    if (ap_) ap_->setProcessed(on);
}

void PreviewWidget::setRate(double rate) {
    if (playing_) { playStart_ = t_; clock_.restart(); }
    rate_ = rate;
    if (ap_) ap_->setRate(rate);
}

void PreviewWidget::markDirty() {
    r_->invalidate();
    update();
}

// Renderer が新しい内容を取り込んだ(paintGL / render の頭)。再生中なら止める(音が古いミキサーのままなので)
void PreviewWidget::onReloaded(bool audioChanged) {
    if (audioChanged) audioStale_ = true;
    t_ = std::min(t_, r_->timeline().total);
    if (playing_ && audioChanged) setPlaying(false);
}

bool PreviewWidget::audioLevel(double pk[2], double rms[2]) {
    if (!ap_ || !apOpen_) return false;
    const auto L = ap_->takeLevels();
    if (!L.valid) return false;
    for (int c = 0; c < 2; ++c) { pk[c] = L.peak[c]; rms[c] = L.rms[c]; }
    return true;
}

void PreviewWidget::paintGL() {
    r_->applyLive();   // 編集があれば、この描画から反映(時刻の計算の前に)
    auto& tl = r_->timeline();
    if (playing_) {
        if (ap_ && apOpen_) {
            if (ap_->streaming()) t_ = std::max(t_, ap_->clock());   // 音声の位置が親。バッファ待ちの間は映像も止める
            else if (ap_->finished()) t_ = tl.total;
        } else {
            t_ = playStart_ + clock_.nsecsElapsed() * 1e-9 * rate_;
        }
        if (t_ >= tl.total) { t_ = tl.total; playing_ = false; emit playingChanged(false); }
    }
    const double last = std::max(0.0, tl.total - 1.0 / tl.fps);
    const double T = std::min(last, std::floor(t_ * tl.fps + 1e-6) / tl.fps);
    r_->render(T, !playing_);
    auto* f = context()->functions();
    const double dpr = devicePixelRatioF();
    const int dw = (int)(width() * dpr), dh = (int)(height() * dpr);
    f->glViewport(0, 0, dw, dh);
    f->glClearColor(0, 0, 0, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);
    const double s = std::min((double)dw / r_->W(), (double)dh / r_->H());
    const int w = (int)(r_->W() * s), h = (int)(r_->H() * s);
    r_->blit(defaultFramebufferObject(), (dw - w) / 2, (dh - h) / 2, w, h);
    emit timeChanged(t_);
    if (playing_) update();   // vsync に合わせて次のコマへ
}

MainWindow::MainWindow(vn::Renderer* r, const QString& title) : r_(r) {
    QString aerr;
    vn::AudioPlayer* ap = audio_.init(r->project(), r->timeline(), &aerr) ? &audio_ : nullptr;
    model_ = new vui::EditModel(this);
    QString err;
    if (!model_->open(title, &err)) qWarning("open: %s", qPrintable(err));
    model_->startAutosave(false);
    model_->setBackupMinutes(vui::prefGet("vedit.backupMin", "5").toInt());   // 写しを残す間隔(設定タブで変える。自動保存の代わりの戻れる対策)
    model_->startWatch(2000);
    ed_ = new vui::EditorWindow(model_);
    ed_->setWindowFlags(Qt::Widget);
    setCentralWidget(ed_);
    view_ = new PreviewWidget(r, ap, this);
    ed_->setMonitor(view_);
    ed_->setStageAspect(r->W(), r->H());
    connect(ed_, &vui::EditorWindow::previewSeek, view_, &PreviewWidget::setTime);
    connect(ed_, &vui::EditorWindow::playToggleRequested, view_, [this] { view_->setPlaying(!view_->playing()); });
    connect(ed_, &vui::EditorWindow::volumeChanged, view_, &PreviewWidget::setVolume);
    connect(ed_, &vui::EditorWindow::rateRequested, view_, &PreviewWidget::setRate);
    connect(ed_, &vui::EditorWindow::processedAudioChanged, view_, &PreviewWidget::setProcessed);
    view_->setVolume(ed_->muted() ? 0.0 : ed_->volume() * ed_->volume(), ed_->muted());
    connect(view_, &PreviewWidget::timeChanged, ed_, [this](double t) { if (view_->playing()) ed_->onPreviewTime(t); });
    connect(view_, &PreviewWidget::playingChanged, ed_, &vui::EditorWindow::onPreviewPlaying);
    // 編集内容は待たずに、次の描画で反映する(保存は EditModel の自動保存に任せる)
    r_->setLiveSource([this] { return model_->serialize(); });
    connect(ed_, &vui::EditorWindow::contentChanged, view_, &PreviewWidget::markDirty);
    connect(ed_, &vui::EditorWindow::projectSwitched, this, [this](const QString&) {
        setWindowTitle("V-edit - " + model_->name());
        r_->setProjectPath(model_->path());
        view_->markDirty();
        startProxy();
    });
    ed_->levelSource = [this](double pk[2], double rms[2]) { return view_->audioLevel(pk, rms); };
    setWindowTitle("V-edit - " + model_->name());
    resize(1400, 860);
    QTimer::singleShot(800, this, [this] { startProxy(); });
}

// 元動画のキーフレームが数秒おき(OBS の収録など)だと、シークのたびに手前のキーフレームから数百コマ読み直してカクつく。
// ブラウザ版と同じプロキシ(proxy.mp4)を裏で用意し、できたらプレビューの本編をそれに切り替える(書き出し・--frame は元動画)
void MainWindow::startProxy() {
    if (proxyJob_) return;
    const QString src = r_->sourcePath();
    if (src.isEmpty() || !QFileInfo::exists(src)) return;
    proxyJob_ = new vui::bridge::Job(vui::bridge::jobCmd("proxy", model_->path(), QJsonObject()), false, this);
    auto* job = proxyJob_;
    connect(job, &vui::bridge::Job::finished, this, [this, job, src](bool ok, const QJsonObject& res, const QString& err) {
        if (proxyJob_ == job) proxyJob_ = nullptr;
        job->deleteLater();
        const QString pv = res.value("path").toString();
        if (!ok || pv.isEmpty() || !QFileInfo::exists(pv)) { if (!ok) qWarning("proxy: %s", qPrintable(err)); return; }
        r_->setPreviewSource(src, pv);
        view_->markDirty();
    });
    job->start();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!ed_->confirmClose()) { e->ignore(); return; }
    QMainWindow::closeEvent(e);
}
