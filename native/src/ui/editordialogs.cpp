// 書き出し・別名保存・仕上がり確認・音声処理・無音検出・修正点・画像編集(app.js の対応する部分の移植)
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include "editorwindow.h"
#include "reviewplayer.h"
#include "timelineview.h"
#include "widgets.h"
#include "core/timeline.h"
#include "media/videostream.h"

namespace vui {

namespace {

QLabel* lab(const QString& t, const char* cls = nullptr, bool wrap = true) {
    auto* l = new QLabel(t);
    if (cls) l->setProperty("class", cls);
    l->setWordWrap(wrap);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}
QLabel* h3(const QString& t) {
    auto* l = new QLabel(t);
    QFont f = l->font();
    f.setBold(true);
    f.setPixelSize(16);
    l->setFont(f);
    return l;
}
QPushButton* pbtn(const QString& t, const char* name = nullptr, bool primary = false) {
    auto* b = new QPushButton(t);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    if (primary) b->setObjectName("primary");
    else if (name) b->setObjectName(name);
    return b;
}
QWidget* hrow(std::initializer_list<QWidget*> l, bool right = false) {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);
    if (right) h->addStretch(1);
    for (QWidget* x : l) h->addWidget(x);
    if (!right) h->addStretch(1);
    return w;
}
QString num(const QJsonValue& v) { return v.isDouble() ? fmtNumber(v.toDouble()) : v.toString(); }
QString fmtS(double t) {
    const int s = (int)std::lround(std::max(0.0, t));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
QString fmtP(double t) {   // review.js fmt
    if (!std::isfinite(t)) return "-";
    t = std::round(std::max(0.0, t) * 100) / 100;
    const int m = (int)std::floor(t / 60);
    const double s = t - m * 60;
    return QString("%1:%2").arg(m).arg(s, 5, 'f', 2, QChar('0'));
}
struct OutOf { double o; bool missing; };
// review.js outOf: 出力上の並び P で 元素材の秒 → 出力上の秒。空白の中なら gap = {at, off}。並びに無い(カットされた)所は次の素材の頭で missing
OutOf outOf(const QJsonArray& P, double src, const QJsonObject& gap = QJsonObject()) {
    auto isGap = [](const QJsonObject& p) { return !p.value("gap").isUndefined() && !p.value("gap").isNull(); };
    if (gap.contains("at")) {
        for (const QJsonValue& pv : P) {
            const QJsonObject p = pv.toObject();
            if (isGap(p) && std::fabs(p.value("at").toDouble() - gap.value("at").toDouble()) < 1e-3)
                return {p.value("o").toDouble() + std::min(gap.value("off").toDouble(), p.value("gap").toDouble()), false};
        }
        src = gap.value("at").toDouble();
    }
    for (const QJsonValue& pv : P) {
        const QJsonObject p = pv.toObject();
        if (isGap(p)) continue;
        if (src >= p.value("s").toDouble() - 1e-6 && src < p.value("e").toDouble()) return {p.value("o").toDouble() + src - p.value("s").toDouble(), false};
    }
    for (const QJsonValue& pv : P) {
        const QJsonObject p = pv.toObject();
        if (isGap(p)) continue;
        if (p.value("s").toDouble() > src) return {p.value("o").toDouble(), true};
    }
    if (P.isEmpty()) return {0, true};
    const QJsonObject L = P.last().toObject();
    return {L.value("o").toDouble() + (isGap(L) ? L.value("gap").toDouble() : L.value("e").toDouble() - L.value("s").toDouble()), true};
}

class ClickRow : public QFrame {
public:
    std::function<void()> onClick;
    explicit ClickRow(QWidget* p = nullptr) : QFrame(p) { setCursor(Qt::PointingHandCursor); }
protected:
    void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && onClick) onClick(); }
};

}  // namespace

// ---- 小さな入出力 ----
bool EditorWindow::askYes(const QString& msg) {
    if (confirm) return confirm(msg);
    return QMessageBox::question(this, "V-edit", msg) == QMessageBox::Yes;
}
QString EditorWindow::askLine(const QString& title, const QString& init) {
    if (askText) return askText(title, init);
    bool ok = false;
    const QString s = QInputDialog::getText(this, "V-edit", title, QLineEdit::Normal, init, &ok);
    return ok ? s : QString();
}
QString EditorWindow::pickPath(const QString& kind) {
    if (pickFile) return pickFile(kind);
    QString filter = "すべてのファイル (*)";
    if (kind == "media") filter = "動画・画像 (*.mp4 *.mov *.mkv *.avi *.webm *.png *.jpg *.jpeg *.webp *.gif);;すべてのファイル (*)";
    else if (kind == "audio") filter = "音声 (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus);;すべてのファイル (*)";
    else if (kind == "vst3") filter = "VST3 (*.vst3);;すべてのファイル (*)";
    return QFileDialog::getOpenFileName(this, QString(), m_->dir(), filter);
}
QString EditorWindow::projectStatePath() const { return m_->dir() + "/" + QFileInfo(m_->path()).completeBaseName() + ".cache"; }
QString EditorWindow::sysText() const { return sysLabel_ ? sysLabel_->text() : QString(); }
bool EditorWindow::checkVisible() const { return stage_ && stage_->check() && !stage_->check()->isHidden(); }
QImage EditorWindow::checkImage() const { return stage_ && stage_->check() ? stage_->check()->image() : QImage(); }

void EditorWindow::showModal(const QList<QWidget*>& w) {
    if (playing_) emit playToggleRequested();
    modal_->setContent(w);
    modal_->open();
}
void EditorWindow::closeModal() { modal_->close(); }

void EditorWindow::jobFail(const QString& title, const QString& msg) {
    lastError = msg;
    auto* pre = new QLabel(msg);
    pre->setObjectName("fail-text");
    pre->setWordWrap(true);
    pre->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* sa = new QScrollArea;
    sa->setWidget(pre);
    sa->setWidgetResizable(true);
    sa->setMaximumHeight(260);
    auto* close = pbtn(QStringLiteral("閉じる"), "m-close");
    connect(close, &QPushButton::clicked, this, [this] { closeModal(); });
    showModal(QList<QWidget*>{h3(title), sa, close});
}

// ---- 別名で保存 ----
void EditorWindow::saveAs() {
    if (m_->dirty()) m_->save(false);   // 未保存分は元のファイルに(取り消しはヒストリーで)
    QString base = QFileInfo(m_->path()).fileName();
    base = base.toLower().endsWith(".vedit.json") ? base.left(base.size() - 11) : QFileInfo(m_->path()).completeBaseName();
    int n = 2;
    while (QFileInfo::exists(m_->dir() + QString("/%1_%2.vedit.json").arg(base).arg(n))) ++n;
    const QString def = QString("%1_%2.vedit.json").arg(base).arg(n);
    QString dst;
    if (pickSave) dst = pickSave(def, m_->dir());
    else dst = QFileDialog::getSaveFileName(this, QStringLiteral("プロジェクトを別名で保存"), m_->dir() + "/" + def, QStringLiteral("vedit プロジェクト (*.vedit.json);;JSON (*.json)"));
    if (dst.isEmpty()) return;
    if (!dst.toLower().endsWith(".json")) dst += ".vedit.json";
    saveAsAndSwitch(dst);
}

void EditorWindow::saveAsAndSwitch(const QString& dst) {
    const QString abs = QFileInfo(dst).absoluteFilePath();
    if (abs == QFileInfo(m_->path()).absoluteFilePath()) { save(); return; }
    // Python の Project.save_as と同じ結果にする(別フォルダなら相対パスを書き換え、処理済み音声のキャッシュを引き継ぐ)
    QTemporaryFile tmp(QDir::tempPath() + "/vedit_saveas_XXXXXX.json");
    tmp.setAutoRemove(true);
    if (!tmp.open()) { showToast(QStringLiteral("別名で保存: 一時ファイルを作れません"), 5000); return; }
    tmp.write(m_->projOut().dump(true).toUtf8());
    tmp.flush();
    QJsonObject a;
    a["dst"] = abs;
    a["data"] = tmp.fileName();
    const bridge::Result r = bridge::runSync(bridge::jobCmd("saveas", m_->path(), a), 60000);
    if (!r.ok) { showToast(QStringLiteral("別名で保存: ") + r.error, 5000); return; }
    QString err;
    const QString np = r.obj.value("path").toString(abs);
    if (!m_->open(np, &err)) { showToast(QStringLiteral("別名で保存: ") + err, 5000); return; }
    m_->histBase = QStringLiteral("開く");
    setWindowTitle("V-edit - " + m_->name());
    emit projectSwitched(m_->path());
    refresh();
    showToast(QStringLiteral("別名で保存しました: ") + m_->name(), 3500);
}

// ---- 書き出し ----
void EditorWindow::exportDialog() {
    if (playing_) emit playToggleRequested();
    OVal& d = m_->d();
    OVal* o = d.get("output");
    if (!o || !o->isObj()) { d.set("output", OVal::object()); o = d.get("output"); }
    const vn::MediaProps mp = vn::probeMedia(m_->resolve(d.strOr("source", "")));
    auto row = [](const QString& label, QWidget* w) {
        auto* r = new QWidget;
        auto* h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        auto* l = new QLabel(label);
        l->setFixedWidth(110);
        h->addWidget(l);
        h->addWidget(w, 1);
        return r;
    };
    auto valStr = [&](const char* key, const QString& def) {
        const OVal* v = o->get(key);
        if (!v || v->isNull()) return def;
        if (v->isNum()) return fmtNumber(v->asNum());
        if (v->isBool()) return QString(v->asBool() ? "true" : "false");
        return v->asStr();
    };
    auto mk = [&](const char* name, const char* key, const QList<QPair<QString, QString>>& opts, const QString& def) {
        auto* c = new QComboBox;
        c->setObjectName(name);
        for (const auto& p : opts) c->addItem(p.second, p.first);
        c->setCurrentIndex(c->findData(valStr(key, def)));
        return c;
    };
    auto* file = new QLineEdit(o->strOr("file", "") .isEmpty() ? QStringLiteral("out/final.mp4") : o->strOr("file", ""));
    file->setObjectName("ex-file");
    auto* browse = pbtn(QStringLiteral("参照…"), "ex-browse");
    connect(browse, &QPushButton::clicked, this, [this, file] {
        QString p;
        if (pickSave) p = pickSave(file->text(), m_->dir());
        else p = QFileDialog::getSaveFileName(this, QStringLiteral("書き出し先"), m_->resolve(file->text()), QStringLiteral("MP4 (*.mp4);;すべてのファイル (*)"));
        if (!p.isEmpty()) file->setText(m_->relPath(p));
    });
    auto* res = mk("ex-height", "height", {{"", QStringLiteral("元のまま (") + (mp.ok ? QString::number(mp.h) + "p" : QString()) + ")"}, {"2160", "2160p (4K)"}, {"1440", "1440p"}, {"1080", "1080p"}, {"720", "720p"}}, "");
    auto* fps = mk("ex-fps", "fps", {{"", QStringLiteral("元のまま (") + (mp.ok ? QString::number(mp.fps, 'f', 2) : QString()) + ")"}, {"60", "60"}, {"30", "30"}, {"29.97", "29.97"}, {"24", "24"}}, "");
    auto* codec = mk("ex-codec", "codec", {{"h264", "H.264 (互換性重視・YouTube 向け)"}, {"h265", "H.265 / HEVC (小さいファイル)"}}, "h264");
    auto* enc = mk("ex-encoder", "encoder", {{"auto", "自動 (GPU があれば GPU)"}, {"gpu", "GPU (NVENC)"}, {"cpu", "CPU (x264/x265・高品質だが遅い)"}}, "auto");
    auto* accel = mk("ex-accel", "gpu_accel", {{"true", "オン (差し込み・画像の重ね合わせを GPU で)"}, {"false", "オフ (すべて CPU)"}}, "true");
    auto* eng = mk("ex-engine", "engine", {{"ffmpeg", "ffmpeg (標準)"}, {"vedit", "自前エンジン (GPU 合成・失敗したら ffmpeg でやり直し)"}}, "ffmpeg");
    auto* q = mk("ex-quality", "quality", {{"high", "高画質"}, {"standard", "標準"}, {"small", "軽量 (小さいファイル)"}}, "high");
    auto* br = new QLineEdit;
    br->setObjectName("ex-bitrate");
    br->setPlaceholderText(QStringLiteral("空欄 = 画質優先"));
    br->setFixedWidth(110);
    if (o->get("bitrate_mbps") && o->get("bitrate_mbps")->isNum() && o->get("bitrate_mbps")->asNum() != 0) br->setText(fmtNumber(o->get("bitrate_mbps")->asNum()));
    auto* ab = mk("ex-audio", "audio_kbps", {{"128", "128 kbps"}, {"192", "192 kbps"}, {"256", "256 kbps"}, {"320", "320 kbps"}}, "192");
    auto* cpu = mk("ex-cpu", "cpu_percent", {{"25", "25% (軽い・遅い)"}, {"50", "50% (おすすめ・PC を使いながら)"}, {"75", "75%"}, {"95", "95% 放置用 (最速・書き出し中は PC が重い)"}}, "50");
    const bool hasIO = m_->markIn >= 0 && m_->markOut >= 0 && m_->markOut > m_->markIn;
    auto* rng = new QComboBox;
    rng->setObjectName("ex-range");
    rng->addItem(QStringLiteral("シーケンス全体"), "all");
    if (hasIO) rng->addItem(QStringLiteral("イン〜アウト (%1–%2)").arg(fmtT(m_->markIn), fmtT(m_->markOut)), "io");
    auto* openAfter = new QCheckBox(QStringLiteral("書き出し後にフォルダを開く"));
    openAfter->setObjectName("ex-open");
    openAfter->setChecked(prefGet("vedit.openAfter") != "0");
    openAfter->setFocusPolicy(Qt::NoFocus);
    auto fr = new QWidget;
    auto* fh = new QHBoxLayout(fr);
    fh->setContentsMargins(0, 0, 0, 0);
    fh->addWidget(file, 1);
    fh->addWidget(browse);
    auto brw = new QWidget;
    auto* bh = new QHBoxLayout(brw);
    bh->setContentsMargins(0, 0, 0, 0);
    bh->addWidget(br);
    bh->addWidget(lab(QStringLiteral("Mbps (YouTube 1080p の目安 8〜12)"), "dim", false), 1);
    auto cpw = new QWidget;
    auto* ch = new QHBoxLayout(cpw);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->addWidget(cpu, 1);
    ch->addWidget(lab(QStringLiteral("書き出しに使うコアの割合"), "dim", false));
    auto* cancel = pbtn(QStringLiteral("キャンセル"), "ex-cancel");
    auto* draft = pbtn(QStringLiteral("低画質で試し書き出し"), "ex-draft");
    auto* go = pbtn(QStringLiteral("書き出し"), nullptr, true);
    go->setObjectName("ex-go");
    go->setProperty("primary", true);
    connect(cancel, &QPushButton::clicked, this, [this] { closeModal(); });
    auto run = [=](bool dr) {
        const double ia = hasIO && rng->currentData().toString() == "io" ? m_->outTime(m_->markIn) : -1;
        const double ib = ia >= 0 ? m_->outTime(m_->markOut) : -1;
        m_->mutate([&] {
            OVal& out = *m_->d().get("output");
            const QString f = file->text().trimmed();
            out.set("file", OVal::str(f.isEmpty() ? QStringLiteral("out/final.mp4") : f));
            const std::pair<const char*, QString> nums[] = {{"height", res->currentData().toString()}, {"fps", fps->currentData().toString()}, {"audio_kbps", ab->currentData().toString()},
                                                           {"bitrate_mbps", br->text().trimmed()}, {"cpu_percent", cpu->currentData().toString()}};
            for (const auto& p : nums) {
                if (p.second.isEmpty()) out.remove(p.first);
                else { bool ok = false; const double x = p.second.toDouble(&ok); if (ok) out.set(p.first, OVal::num(x)); else out.remove(p.first); }
            }
            out.set("codec", OVal::str(codec->currentData().toString()));
            out.set("encoder", OVal::str(enc->currentData().toString()));
            out.set("quality", OVal::str(q->currentData().toString()));
            out.set("engine", OVal::str(eng->currentData().toString()));
            out.set("gpu_accel", OVal::boolean(accel->currentData().toString() == "true"));
            if (!out.get("fps") || out.get("fps")->isNull()) out.set("fps", OVal::null());
        });
        prefSet("vedit.openAfter", openAfter->isChecked() ? "1" : "0");
        QJsonObject range;
        if (ia >= 0) { range["a"] = ia; range["b"] = ib; }
        startExport(dr, range, openAfter->isChecked());
    };
    connect(draft, &QPushButton::clicked, this, [run] { run(true); });
    connect(go, &QPushButton::clicked, this, [run] { run(false); });
    showModal(QList<QWidget*>{h3(QStringLiteral("書き出し設定")), row(QStringLiteral("ファイル"), fr), row(QStringLiteral("範囲"), rng), row(QStringLiteral("形式"), codec), row(QStringLiteral("解像度"), res),
               row(QStringLiteral("フレームレート"), fps), row(QStringLiteral("エンコーダー"), enc), row(QStringLiteral("GPU 合成"), accel), row(QStringLiteral("書き出しエンジン"), eng),
               row(QStringLiteral("画質"), q), row(QStringLiteral("ビットレート"), brw), row(QStringLiteral("音声"), ab), row(QStringLiteral("CPU の使用"), cpw), openAfter,
               hrow({cancel, draft, go}, true)});
}

void EditorWindow::startExport(bool draft, const QJsonObject& range, bool openAfter) {
    if (exporting_) { showToast(QStringLiteral("書き出し中です")); return; }
    if (m_->dirty()) m_->save(false);
    exporting_ = true;
    auto* txt = lab(QStringLiteral("準備中…(音声処理が必要なら先に行います)"), nullptr);
    txt->setObjectName("export-text");
    auto* bar = new QProgressBar;
    bar->setObjectName("export-bar");
    bar->setRange(0, 1000);
    bar->setValue(0);
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    showModal(QList<QWidget*>{h3(draft ? QStringLiteral("低画質で書き出し") : QStringLiteral("書き出し")), txt, bar});
    QJsonObject a;
    a["draft"] = draft;
    if (range.contains("a")) a["range"] = QJsonArray{range.value("a"), range.value("b")};
    exportJob_ = new bridge::Job(bridge::jobCmd("render", m_->path(), a), false, this);
    QPointer<QLabel> pt(txt);
    QPointer<QProgressBar> pb(bar);
    connect(exportJob_, &bridge::Job::progress, this, [pt, pb](double p, const QString&) {
        if (pb) pb->setValue((int)std::lround(p * 1000));
        if (pt) pt->setText(QStringLiteral("書き出し中… %1%").arg((int)std::lround(p * 100)));
    });
    bridge::Job* job = exportJob_;
    connect(job, &bridge::Job::finished, this, [this, job, draft, openAfter](bool ok, const QJsonObject& r, const QString& err) {
        exporting_ = false;
        if (exportJob_ == job) exportJob_ = nullptr;
        job->deleteLater();
        if (!ok) { jobFail(QStringLiteral("書き出しに失敗"), err); return; }
        exportDone(draft, openAfter, r);
    });
    job->start();
}

void EditorWindow::exportDone(bool draft, bool openAfter, const QJsonObject& r) {
    Q_UNUSED(draft);
    lastExportResult = r;
    const QString out = r.value("output").toString();
    lastExportOutput = out;
    const QString outAbs = QFileInfo(out).isAbsolute() ? out : m_->resolve(out);
    QList<QWidget*> w;
    w << h3(QStringLiteral("完了"));
    auto* ol = lab(out, nullptr);
    ol->setObjectName("export-out");
    w << ol;
    const QJsonObject au = r.value("audio").toObject();
    QString nt = QStringLiteral("%1 / %2区間").arg(fmtT(r.value("duration").toDouble(), false), num(r.value("segments")));
    if (!au.isEmpty()) nt += QStringLiteral(" / 音量 %1 LUFS・ピーク %2 dBTP").arg(num(au.value("lufs")), num(au.value("true_peak")));
    w << lab(nt, "note");
    const QJsonArray ck = r.value("checks").toArray();
    if (!ck.isEmpty()) {
        auto* box = new QWidget;
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(2);
        bl->addWidget(lab(QStringLiteral("⚠ 確認してください(%1件)").arg(ck.size()), "notewarn"));
        int i = 0;
        for (const QJsonValue& cv : ck) {
            const QJsonObject c = cv.toObject();
            const bool hasAt = c.contains("at") && !c.value("at").isNull();
            auto* b = new QPushButton((hasAt ? fmtT(c.value("at").toDouble(), false) + " " : QString()) + c.value("msg").toString());
            b->setFlat(true);
            b->setObjectName(QString("check_%1").arg(i++));
            b->setStyleSheet("QPushButton{text-align:left;border:0;background:transparent;color:#ffb84d;padding:2px 0;}");
            b->setCursor(hasAt ? Qt::PointingHandCursor : Qt::ArrowCursor);
            if (hasAt) connect(b, &QPushButton::clicked, this, [this, c] {
                closeModal();
                seekTo(m_->srcOf(clampd(c.value("at").toDouble(), 0, m_->totalOut()), true));
            });
            bl->addWidget(b);
        }
        auto* sa = new QScrollArea;
        sa->setWidget(box);
        sa->setWidgetResizable(true);
        sa->setMaximumHeight(180);
        w << sa;
    } else {
        w << lab(QStringLiteral("✔ チェック: 効果音の抜け・重なり・音量 問題なし"), "note");
    }
    auto* open = pbtn(QStringLiteral("フォルダを開く"), nullptr, true);
    open->setObjectName("export-open");
    auto* close = pbtn(QStringLiteral("閉じる"), "export-close");
    connect(open, &QPushButton::clicked, this, [this, outAbs] { bridge::launch(bridge::explorerCmd(outAbs)); closeModal(); });
    connect(close, &QPushButton::clicked, this, [this] { closeModal(); });
    w << hrow({open, close});
    showModal(w);
    if (openAfter) bridge::launch(bridge::explorerCmd(outAbs));
    m_->checkExternal();
}

// ---- 仕上がり確認(実際の描画で今のコマを出す) ----
void EditorWindow::checkFrame() {
    if (frameJob_ && frameJob_->running()) return;
    if (m_->dirty()) m_->save(false);
    if (playing_) emit playToggleRequested();
    showToast(QStringLiteral("実際の描画で確認中…"), 8000);
    QJsonObject a;
    a["t"] = m_->tout();
    frameJob_ = new bridge::Job(bridge::jobCmd("frame", m_->path(), a), false, this);
    bridge::Job* job = frameJob_;
    connect(job, &bridge::Job::finished, this, [this, job](bool ok, const QJsonObject& r, const QString& err) {
        if (frameJob_ == job) frameJob_ = nullptr;
        job->deleteLater();
        if (!ok) { lastError = err; showToast(QStringLiteral("失敗: ") + err, 6000); return; }
        const QString out = r.value("output").toString();
        QImage im(QFileInfo(out).isAbsolute() ? out : m_->resolve(out));
        if (im.isNull()) { showToast(QStringLiteral("失敗: 画像を読めません"), 6000); return; }
        stage_->check()->setImage(im);
        stage_->check()->show();
        stage_->check()->raise();
        toast_->hide();
    });
    job->start();
}

// ---- 音声処理(vaudio.exe --process) ----
void EditorWindow::runAudio() {
    if (audioJob_ && audioJob_->running()) return;
    if (m_->dirty()) m_->save(false);
    audioJob_ = new bridge::Job(bridge::audioCmd(m_->path(), true), true, this);
    bridge::Job* job = audioJob_;
    if (auto* b = leftBody_ ? leftBody_->findChild<QPushButton*>("b-audio") : nullptr) { b->setEnabled(false); b->setText(QStringLiteral("処理中…")); }
    if (bRaw_) { bRaw_->setEnabled(false); bRaw_->setText(QStringLiteral("⏳ 音声処理中…(終わるまで元の音)")); bRaw_->show(); }
    connect(job, &bridge::Job::progress, this, [this](double p, const QString&) {
        if (bRaw_) bRaw_->setText(QStringLiteral("⏳ 音声処理中… %1%(終わるまで元の音)").arg((int)std::lround(p * 100)));
    });
    connect(job, &bridge::Job::finished, this, [this, job](bool ok, const QJsonObject& r, const QString& err) {
        if (audioJob_ == job) audioJob_ = nullptr;
        job->deleteLater();
        if (bRaw_) bRaw_->setEnabled(true);
        if (ok) {
            const QJsonValue cl = r.value("clicks_repaired");
            showToast(QStringLiteral("音声処理が完了(クリック修復 %1件)").arg(cl.isDouble() ? fmtNumber(cl.toDouble()) : QStringLiteral("0")));
            m_->checkExternal();
        } else {
            lastError = err;
            showToast(QStringLiteral("失敗: ") + err, 6000);
        }
        updAudioWarn();
        fillLeft();
        emit processedAudioChanged(useProcessed());
    });
    job->start();
}

// ---- 無音検出 ----
void EditorWindow::runSilence() {
    if (silBusy_) return;
    if (m_->dirty()) m_->save(false);
    silBusy_ = true;
    fillLeft();
    QJsonObject a;
    a["min"] = sil_.min;
    a["noise"] = sil_.noise;
    a["keep_before"] = sil_.kb;
    a["keep_after"] = sil_.ka;
    silJob_ = new bridge::Job(bridge::jobCmd("silence", m_->path(), a), false, this);
    bridge::Job* job = silJob_;
    connect(job, &bridge::Job::finished, this, [this, job](bool ok, const QJsonObject& r, const QString& err) {
        if (silJob_ == job) silJob_ = nullptr;
        job->deleteLater();
        silBusy_ = false;
        if (ok) { silRes_ = r; silHave_ = true; }
        else { lastError = err; showToast(QStringLiteral("失敗: ") + err, 6000); }
        fillLeft();
    });
    job->start();
}

// ---- 波形(wave.json が無ければ裏で作る) ----
void EditorWindow::ensureWave() {
    if (!waveJobOn_ || waveAsked_) return;
    waveAsked_ = true;
    if (QFileInfo::exists(projectStatePath() + "/wave.json")) return;
    if (!QFileInfo::exists(m_->resolve(m_->d().strOr("source", "")))) return;
    waveJob_ = new bridge::Job(bridge::jobCmd("wave", m_->path(), QJsonObject()), false, this);
    bridge::Job* job = waveJob_;
    connect(job, &bridge::Job::finished, this, [this, job](bool ok, const QJsonObject&, const QString&) {
        if (waveJob_ == job) waveJob_ = nullptr;
        job->deleteLater();
        if (ok) tl_->reloadWave();
    });
    job->start();
}

// ---- 画像編集 ----
void EditorWindow::watchImage(const QString& abs) {
    if (!imgWatch_) {
        imgWatch_ = new QFileSystemWatcher(this);
        connect(imgWatch_, &QFileSystemWatcher::fileChanged, this, [this](const QString& f) {
            if (QFileInfo::exists(f)) imgWatch_->addPath(f);   // 置き換え保存で外れた監視を付け直す
            m_->invalidateCache();
            emit contentChanged();
            tl_->update();
        });
    }
    if (QFileInfo::exists(abs) && !imgWatch_->files().contains(abs)) imgWatch_->addPath(abs);
}

void EditorWindow::imageNew(const QString& kind) {
    if (kind == "frame") {
        if (m_->dirty()) m_->save(false);
        if (playing_) emit playToggleRequested();
        showToast(QStringLiteral("今の画面を原寸で書き出し中…"), 6000);
    }
    QJsonObject a;
    a["act"] = "new";
    a["kind"] = kind;
    if (kind == "frame") a["t"] = m_->tout();
    auto* job = new bridge::Job(bridge::jobCmd("vimg", m_->path(), a), false, this);
    connect(job, &bridge::Job::finished, this, [this, job](bool ok, const QJsonObject& r, const QString& err) {
        job->deleteLater();
        if (!ok) { lastError = err; showToast(QStringLiteral("画像編集: ") + err, 5000); return; }
        const QString png = r.value("png").toString(), path = r.value("path").toString();
        OVal it = OVal::object();
        it.set("file", OVal::str(png));
        const double t = m_->tcur();
        it.set("start", OVal::num(toFixedNum(t, 2)));
        it.set("end", OVal::num(toFixedNum(std::min(m_->dur(), t + 3), 2)));
        it.set("pos", OVal::str("full"));
        if (m_->vTarget > 2) it.set("track", OVal::num(m_->vTarget));
        m_->addItem("overlays", it);
        watchImage(m_->resolve(png));
        const bridge::Cmd c = bridge::vimgOpenCmd(m_->resolve(path));
        lastImageCmd = c.line();
        if (!bridge::launch(c)) showToast(QStringLiteral("画像編集: 起動できませんでした"), 5000);
    });
    job->start();
}

void EditorWindow::imageEditSel() {
    static const QRegularExpression re(R"(\.(png|jpe?g|webp|vimg\.json)$)", QRegularExpression::CaseInsensitiveOption);
    OVal* ovl = m_->d().get("overlays");
    OVal* it = (m_->selKind == "overlays" && m_->selI >= 0 && ovl && m_->selI < ovl->size()) ? &ovl->at(m_->selI) : nullptr;
    if (!it || !re.match(it->strOr("file", "")).hasMatch()) { showToast(QStringLiteral("差し込み画像(PNG/JPG/WebP)を選択してください")); return; }
    const QString file = it->strOr("file", "");
    const int idx = m_->selI;
    QJsonObject a;
    a["act"] = "open";
    a["path"] = file;
    auto* job = new bridge::Job(bridge::jobCmd("vimg", m_->path(), a), false, this);
    connect(job, &bridge::Job::finished, this, [this, job, idx, file](bool ok, const QJsonObject& r, const QString& err) {
        job->deleteLater();
        if (!ok) { lastError = err; showToast(QStringLiteral("画像編集: ") + err, 5000); return; }
        const QString png = r.value("png").toString(), path = r.value("path").toString();
        if (png != file) {   // JPG 等は同名の PNG(画像編集の書き出し)に差し替え
            m_->mutate([&] {
                OVal* l = m_->d().get("overlays");
                if (l && idx < l->size()) l->at(idx).set("file", OVal::str(png));
            });
        }
        watchImage(m_->resolve(png));
        const bridge::Cmd c = bridge::vimgOpenCmd(m_->resolve(path));
        lastImageCmd = c.line();
        if (!bridge::launch(c)) showToast(QStringLiteral("画像編集: 起動できませんでした"), 5000);
    });
    job->start();
}

void EditorWindow::imageMenu(const QPoint& global) {
    std::vector<MenuItem> items;
    items.push_back({QStringLiteral("新規画像を作って差し込み(V2)に置く → 画像編集で開く"), [this] { imageNew("image"); }, false});
    items.push_back({QStringLiteral("新規テロップ画像を作る(テロップのスタイルで文字入り)"), [this] { imageNew("telop"); }, false});
    items.push_back({QStringLiteral("今の画面を下敷きにして画像を作る(下敷きは書き出さない)"), [this] { imageNew("frame"); }, false});
    items.push_back({QStringLiteral("選択中の差し込み画像を編集"), [this] { imageEditSel(); }, false});
    items.push_back({QStringLiteral("画像編集(vimg)を起動(単独の新しい窓・プロジェクトに紐づかない)"), [this] {
        const bridge::Cmd c = bridge::vimgOpenCmd(QString());
        lastImageCmd = c.line();
        if (bridge::launch(c)) showToast(QStringLiteral("vimg を起動しています…"), 2500);
        else showToast(QStringLiteral("vimg: 起動できませんでした"), 5000);
    }, false});
    if (tl_->menuHandler) { tl_->menuHandler(global, items); return; }
    QMenu menu(this);
    for (const MenuItem& mi : items) { auto fn = mi.fn; menu.addAction(mi.label, this, [fn] { fn(); }); }
    menu.exec(global);
}

// ---- 修正点(review.js) ----
bool EditorWindow::reviewShown() const { return review_ && !review_->isHidden(); }

void EditorWindow::reviewToggle() {
    if (rvPopped_) { if (rvWin_) { rvWin_->raise(); rvWin_->activateWindow(); } return; }   // 別窓に出している間はそちらを前に出す
    const bool show = !reviewShown();
    review_->setVisible(show);
    prefSet("vedit.review", show ? "1" : "0");
    updateButtons();
    if (show) { reviewDraw(); reviewRefresh(); }
    else if (rvPlayer_) rvPlayer_->clearSource();
}

// 別窓: 右の列そのものを別のウィンドウへ移す(同じ部品なので中身・動画・状態はそのまま)。大きくして修正前の映像を見られる
void EditorWindow::reviewPopout() {
    if (rvPopped_ || !review_) return;
    rvPopped_ = true;
    prefSet("vedit.review", "0");
    if (!rvWin_) {
        rvWin_ = new QWidget(this, Qt::Window);
        rvWin_->setObjectName("rv-window");
        rvWin_->setWindowTitle(QStringLiteral("修正点 - vedit"));
        rvWin_->setAttribute(Qt::WA_ShowWithoutActivating);   // 編集画面からフォーカスを奪わない
        auto* l = new QVBoxLayout(rvWin_);
        l->setContentsMargins(0, 0, 0, 0);
        rvWin_->installEventFilter(this);
        rvWin_->resize(760, 900);
    }
    mainLay_->removeWidget(review_);
    review_->setParent(rvWin_);
    review_->setMinimumWidth(0);
    review_->setMaximumWidth(QWIDGETSIZE_MAX);
    rvWin_->layout()->addWidget(review_);
    review_->show();
    rvWin_->show();
    updateButtons();
    reviewDraw();
    reviewRefresh();
}

void EditorWindow::reviewDock() {
    if (!rvPopped_) return;
    rvPopped_ = false;
    rvWin_->layout()->removeWidget(review_);
    review_->setParent(centralWidget());
    review_->setFixedWidth(300);
    mainLay_->addWidget(review_);
    review_->show();
    rvWin_->hide();
    prefSet("vedit.review", "1");
    updateButtons();
    reviewDraw();
    reviewRefresh();
}

// 再生ヘッドに合わせて修正前の動画を動かす(review.js tick)。位置は元素材の秒 tcur で対応づける(カットを足して後ろがずれても合う)
void EditorWindow::reviewTick() {
    if (dying_) return;
    if (!rvPlayer_ || !reviewShown()) return;
    const QJsonObject D = reviewState_;
    if (!D.value("exists").toBool()) return;
    const double src = m_->tcur();
    const OutOf b = outOf(D.value("base").toObject().value("pieces").toArray(), src);
    const OutOf c = outOf(D.value("cur").toObject().value("pieces").toArray(), src);
    rvPlayer_->setCaption(QStringLiteral("修正前 %1%2  /  今 %3").arg(fmtP(b.o), b.missing ? QStringLiteral("(修正前に無い所)") : QString(), fmtP(c.o)));
    rvPlayer_->sync(b.o, playing_ && shuttle_ >= 0, rate_);
}

void EditorWindow::reviewRefresh() {
    QJsonObject op;
    op["op"] = "state";
    reviewOp(op);
}

void EditorWindow::reviewOp(const QJsonObject& opIn) {
    QJsonObject a = opIn;
    if (reviewRound_ >= 0 && !a.contains("r")) a["r"] = reviewRound_;
    const QString op = a.value("op").toString("state");
    auto* job = new bridge::Job(bridge::jobCmd("review", m_->path(), a), false, this);
    ++reviewPending_;
    connect(job, &bridge::Job::finished, this, [this, job, op](bool ok, const QJsonObject& r, const QString& err) {
        --reviewPending_;
        job->deleteLater();
        if (!ok) {
            if (op == "state") reviewState_ = QJsonObject{{"error", err}};
            else { lastError = err; showToast(QStringLiteral("修正点: ") + err, 5000); }
            reviewDraw();
            return;
        }
        if (op == "export") {
            const QString tx = r.value("text").toString();
            copyToClipboard(tx);
            lastExportResult = r;
            return;
        }
        if (op == "start" || op == "next") reviewRound_ = -1;
        reviewState_ = r;
        reviewDraw();
    });
    job->start();
}

void EditorWindow::reviewDraw() {
    if (dying_) return;
    if (!review_ || !reviewLay_) return;
    if (!rvPlayer_) rvPlayer_ = new ReviewPlayer;
    if (rvPlayer_->parentWidget()) { rvPlayer_->hide(); rvPlayer_->setParent(nullptr); }   // 動画は作り直しをまたいで使い回す(消さない)
    while (QLayoutItem* it = reviewLay_->takeAt(0)) {
        if (QWidget* w = it->widget()) { w->hide(); w->deleteLater(); }
        delete it;
    }
    using namespace std;
    const QJsonObject D = reviewState_;
    if (!D.value("exists").toBool()) rvPlayer_->clearSource();
    const QJsonArray RS = D.value("rounds").toArray();
    const int last = RS.size() - 1;
    auto* head = new QWidget;
    auto* hh = new QHBoxLayout(head);
    hh->setContentsMargins(10, 6, 10, 6);
    hh->setSpacing(6);
    if (RS.size() > 1) {
        auto* sel = new QComboBox;
        sel->setObjectName("rv-round");
        sel->setToolTip(QStringLiteral("どの回の修正点を見るか(前回の修正 / 今回の修正)"));
        for (const QJsonValue& xv : RS) {
            const QJsonObject x = xv.toObject();
            const int i = x.value("i").toInt();
            sel->addItem(QStringLiteral("修正点%1%2  %3件").arg(i + 1).arg(i == last ? QStringLiteral("(今回)") : i == last - 1 ? QStringLiteral("(前回)") : QString()).arg(x.value("n").toInt()), i);
        }
        sel->setCurrentIndex(std::clamp<int>(D.value("round").toInt(), 0, (int)RS.size() - 1));
        connect(sel, &QComboBox::activated, this, [this, sel, last](int k) {
            const int v = sel->itemData(k).toInt();
            reviewRound_ = v == last ? -1 : v;
            reviewRefresh();
        });
        hh->addWidget(sel);
    } else {
        auto* t = new QLabel(QStringLiteral("<b>修正点</b>"));
        t->setObjectName("rv-title");
        hh->addWidget(t);
    }
    hh->addStretch(1);
    if (!rvPopped_) {
        auto* pop = pbtn(QStringLiteral("⧉ 別窓"), "rv-pop");
        pop->setToolTip(QStringLiteral("別の窓に出す(大きくして修正前の映像を見られる)"));
        connect(pop, &QPushButton::clicked, this, [this] { reviewPopout(); });
        auto* x = pbtn(QStringLiteral("×"), "rv-close");
        x->setToolTip(QStringLiteral("閉じる"));
        connect(x, &QPushButton::clicked, this, [this] { reviewToggle(); });
        hh->addWidget(pop);
        hh->addWidget(x);
    } else {
        auto* dock = pbtn(QStringLiteral("⇤ 右に戻す"), "rv-dock");
        dock->setToolTip(QStringLiteral("編集画面の右に戻す"));
        connect(dock, &QPushButton::clicked, this, [this] { reviewDock(); });
        hh->addWidget(dock);
    }
    reviewLay_->addWidget(head);

    auto* sa = new QScrollArea;
    sa->setObjectName("rv-body");
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* body = new QWidget;
    auto* v = new QVBoxLayout(body);
    v->setContentsMargins(10, 4, 10, 10);
    v->setSpacing(6);
    sa->setWidget(body);
    reviewLay_->addWidget(sa, 1);
    if (D.isEmpty()) { v->addWidget(lab(QStringLiteral("読み込み中…"), "note")); v->addStretch(1); return; }
    if (D.contains("error")) { v->addWidget(lab(QStringLiteral("読めません: ") + D.value("error").toString(), "notewarn")); v->addStretch(1); return; }
    if (!D.value("exists").toBool()) {
        v->addWidget(lab(QStringLiteral("修正前の控えがまだありません。先方に見せた状態のうちに控えておくと、修正点の時刻(先方が見た動画の時刻)で正しい所へ飛べ、修正前の映像と並べて見られます。"), "note"));
        v->addWidget(lab(QStringLiteral("修正前の動画: ") + (D.value("video_guess").toString().isEmpty() ? QStringLiteral("(見つかりません。後で指定できます)") : D.value("video_guess").toString()), "note"));
        auto* b = pbtn(QStringLiteral("今の状態を修正前として控える"), nullptr, true);
        b->setObjectName("rv-start");
        connect(b, &QPushButton::clicked, this, [this] { reviewOp(QJsonObject{{"op", "start"}}); });
        v->addWidget(b);
        v->addStretch(1);
        return;
    }
    // 修正前の映像(軽い版 = 720p・キーフレーム0.5秒おき・音入り ができたらそれ。できるまでは元の mp4 を出し、裏で作らせる)
    const QString vid = D.value("video").toString();
    if (!vid.isEmpty()) {
        const bool pv = D.value("video_pv").toBool() && QFileInfo::exists(D.value("pv_path").toString());
        rvPlayer_->setSource(pv ? D.value("pv_path").toString() : D.value("video_abs").toString(), vid + (pv ? "|pv" : ""));
        v->addWidget(rvPlayer_);
        rvPlayer_->show();
        if (!pv && !pvAsked_.contains(vid)) { pvAsked_.insert(vid); reviewOp(QJsonObject{{"op", "proxy"}}); }
        reviewTick();
    } else {
        rvPlayer_->clearSource();
        v->addWidget(lab(QStringLiteral("修正前の動画が指定されていません(書き出した mp4 を project と同じフォルダに置いて「控え直す」か、vedit review … start --video)。"), "note"));
    }
    const QJsonArray P = D.value("points").toArray();
    int done = 0;
    for (const QJsonValue& pv : P) if (pv.toObject().value("done").toBool()) ++done;
    v->addWidget(lab(QStringLiteral("%1 %2件(済 %3)").arg(RS.size() > 1 ? QStringLiteral("修正点%1").arg(D.value("round").toInt() + 1) : QStringLiteral("修正点")).arg(P.size()).arg(done), "h4"));
    const QJsonArray curP = D.value("cur").toObject().value("pieces").toArray();
    int pi = 0;
    for (const QJsonValue& pv : P) {
        const QJsonObject p = pv.toObject();
        const OutOf now = outOf(curP, p.value("src").toDouble());
        auto* row = new ClickRow;
        row->setObjectName(QString("rv-pt_%1").arg(pi));
        row->setToolTip(QStringLiteral("クリックでその所へ(元素材 %1)").arg(fmtP(p.value("src").toDouble())));
        auto* rh = new QHBoxLayout(row);
        rh->setContentsMargins(2, 2, 2, 2);
        rh->setSpacing(6);
        auto* cb = new QCheckBox;
        cb->setObjectName(QString("rv-cb_%1").arg(pi));
        cb->setChecked(p.value("done").toBool());
        cb->setToolTip(QStringLiteral("済みにする"));
        cb->setFocusPolicy(Qt::NoFocus);
        const int pidx = p.value("i").toInt();
        connect(cb, &QCheckBox::clicked, this, [this, pidx](bool on) { reviewOp(QJsonObject{{"op", "update"}, {"i", pidx}, {"done", on}}); });
        rh->addWidget(cb);
        auto* tl = new QLabel(fmtS(now.o));
        tl->setObjectName(QString("rv-t_%1").arg(pi));
        tl->setStyleSheet("color:#ffc233;");
        rh->addWidget(tl);
        const QString nowTxt = now.missing ? QStringLiteral("カット済") : fmtS(now.o) == fmtS(p.value("t").toDouble()) ? QString() : QStringLiteral("(先方 %1)").arg(fmtS(p.value("t").toDouble()));
        auto* nl = new QLabel(nowTxt);
        nl->setObjectName(QString("rv-now_%1").arg(pi));
        nl->setProperty("class", "dim");
        rh->addWidget(nl);
        auto* tx = new QLabel(p.value("text").toString());
        tx->setWordWrap(true);
        rh->addWidget(tx, 1);
        auto* del = pbtn(QStringLiteral("×"), nullptr);
        del->setObjectName(QString("rv-del_%1").arg(pi));
        const QString ptxt = p.value("text").toString();
        const double pt = p.value("t").toDouble();
        connect(del, &QPushButton::clicked, this, [this, pidx, ptxt, pt] {
            if (askYes(QStringLiteral("「%1 %2」を消しますか?").arg(fmtS(pt), ptxt))) reviewOp(QJsonObject{{"op", "delete"}, {"i", pidx}});
        });
        rh->addWidget(del);
        const double src = p.value("src").toDouble();
        const QJsonObject gap = p.value("gap").toObject();
        row->onClick = [this, src, gap] { seekTo(gap.contains("at") ? gap.value("at").toDouble() : src); };
        v->addWidget(row);
        ++pi;
    }
    if (!P.isEmpty()) {
        auto* cp = pbtn(QStringLiteral("📋 vreview 用にコピー"), "rv-copy");
        cp->setToolTip(QStringLiteral("vreview の管理ページに貼る文章(1行1点・時刻は今の動画。カットで消えた点は継ぎ目に[カット済])をクリップボードへ"));
        QPointer<QPushButton> pcp(cp);
        connect(cp, &QPushButton::clicked, this, [this, pcp] {
            QJsonObject a{{"op", "export"}};
            if (reviewRound_ >= 0) a["r"] = reviewRound_;
            auto* job = new bridge::Job(bridge::jobCmd("review", m_->path(), a), false, this);
            ++reviewPending_;
            connect(job, &bridge::Job::finished, this, [this, job, pcp](bool ok, const QJsonObject& r, const QString& err) {
                --reviewPending_;
                job->deleteLater();
                if (!ok) { showToast(QStringLiteral("修正点の出力: ") + err, 5000); return; }
                const QString tx = r.value("text").toString();
                copyToClipboard(tx);
                lastExportResult = r;
                if (pcp) {
                    pcp->setText(QStringLiteral("コピーしました(%1件)").arg(tx.trimmed().isEmpty() ? 0 : tx.trimmed().split('\n').size()));
                    QTimer::singleShot(2000, pcp, [pcp] { if (pcp) pcp->setText(QStringLiteral("📋 vreview 用にコピー")); });
                }
            });
            job->start();
        });
        v->addWidget(cp);
    }
    auto* addT = pbtn(QStringLiteral("▸ 修正点を足す(先方の文をそのまま貼れます)"), "rv-addtoggle");
    auto* box = new QWidget;
    auto* bl = new QVBoxLayout(box);
    bl->setContentsMargins(0, 0, 0, 0);
    auto* ta = new QPlainTextEdit;
    ta->setObjectName("rv-addtext");
    ta->setPlaceholderText(QStringLiteral("0:07 画像\n3:24 2回言ってるから1回目カット"));
    ta->setFixedHeight(64);
    auto* addB = pbtn(QStringLiteral("足す"), "rv-add");
    bl->addWidget(ta);
    bl->addWidget(hrow({addB}));
    box->hide();
    connect(addT, &QPushButton::clicked, this, [box] { box->setVisible(box->isHidden()); });
    connect(addB, &QPushButton::clicked, this, [this, ta] { const QString t = ta->toPlainText(); if (!t.trimmed().isEmpty()) reviewOp(QJsonObject{{"op", "add"}, {"text", t}}); });
    v->addWidget(addT);
    v->addWidget(box);
    const QJsonArray C = D.value("changes").toArray();
    v->addWidget(lab(QStringLiteral("修正前から変えた所 %1件").arg(C.size()), "h4"));
    if (C.isEmpty()) v->addWidget(lab(QStringLiteral("まだありません"), "note"));
    int ci = 0;
    for (const QJsonValue& cv : C) {
        const QJsonObject c = cv.toObject();
        auto* row = new ClickRow;
        row->setObjectName(QString("rv-chg_%1").arg(ci++));
        auto* rh = new QHBoxLayout(row);
        rh->setContentsMargins(2, 2, 2, 2);
        const bool hasSrc = c.contains("src") && !c.value("src").isNull();
        auto* t = new QLabel(hasSrc ? fmtS(outOf(curP, c.value("src").toDouble()).o) : QString());
        t->setStyleSheet("color:#ffc233;");
        rh->addWidget(t);
        rh->addWidget(lab(c.value("what").toString(), "dim", false));
        rh->addWidget(lab(c.value("label").toString(), nullptr), 1);
        if (hasSrc) { const double s = c.value("src").toDouble(); row->onClick = [this, s] { seekTo(s); }; }
        v->addWidget(row);
    }
    v->addWidget(lab(QStringLiteral("控え: %1  修正前 %2 → 今 %3").arg(D.value("created").toString(), fmtP(D.value("base").toObject().value("total").toDouble()), fmtP(D.value("cur").toObject().value("total").toDouble())), "note"));
    auto* nextB = pbtn(QStringLiteral("次の回を始める"), "rv-next");
    nextB->setToolTip(QStringLiteral("今の状態(先方に見せ直す版)を修正前として控え、次の回の修正点を始める(前の回の修正点は残り、上のプルダウンで見られます)"));
    auto* redoB = pbtn(QStringLiteral("控え直す"), "rv-redo");
    redoB->setToolTip(QStringLiteral("今の状態で修正前を作り直す(すべての回の修正点が消えます)"));
    const int nr = RS.size();
    connect(nextB, &QPushButton::clicked, this, [this, nr] {
        if (askYes(QStringLiteral("今の状態を修正前として控え、修正点%1 を始めます。前の回の修正点は残ります。よいですか?").arg(nr + 1))) reviewOp(QJsonObject{{"op", "next"}});
    });
    connect(redoB, &QPushButton::clicked, this, [this] {
        if (askYes(QStringLiteral("今の状態を修正前として控え直します。すべての回の修正点が消えます。よいですか?"))) reviewOp(QJsonObject{{"op", "start"}, {"force", true}});
    });
    v->addWidget(hrow({nextB, redoB}));
    v->addStretch(1);
}

}  // namespace vui
