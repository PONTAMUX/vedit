// vaudio: Qt GUI。用語・項目は vedit/web/app.js の音声タブ(STEP_NAME / FX)と同じ。
#include "gui.h"

#include <windows.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>
#include <atomic>
#include <cmath>
#include <vector>

#include "engine.h"
#include "media.h"
#include "vst3host.h"

using namespace vaudio;

namespace {

struct FxParam { const char* key; const char* label; double def; const char* unit; double step; };
struct FxDef { const char* name; const char* title; std::vector<FxParam> params; const char* note; };

const std::vector<FxDef>& fxDefs() {
    static const std::vector<FxDef> v = {
        {"HighpassFilter", "ローカット(ハイパス)", {{"cutoff_frequency_hz", "周波数", 80, "Hz", 5}}, "低いゴロゴロ・吹かれを消す"},
        {"NoiseGate", "ノイズゲート", {{"threshold_db", "しきい値", -45, "dB", 1}, {"ratio", "比率", 4, ":1", 0.5}, {"release_ms", "リリース", 150, "ms", 10}}, "しきい値より小さい音(無音部の雑音)を下げる"},
        {"Compressor", "コンプレッサー", {{"threshold_db", "しきい値", -18, "dB", 1}, {"ratio", "比率", 3, ":1", 0.5}, {"attack_ms", "アタック", 5, "ms", 1}, {"release_ms", "リリース", 120, "ms", 10}}, "声の大小をならす"},
        {"LowShelfFilter", "EQ 低域", {{"cutoff_frequency_hz", "周波数", 200, "Hz", 10}, {"gain_db", "増減", -3, "dB", 0.5}}, ""},
        {"PeakFilter", "EQ 中域(ピーク)", {{"cutoff_frequency_hz", "周波数", 3000, "Hz", 50}, {"gain_db", "増減", 2, "dB", 0.5}, {"q", "Q(幅)", 1, "", 0.1}}, ""},
        {"HighShelfFilter", "EQ 高域", {{"cutoff_frequency_hz", "周波数", 8000, "Hz", 100}, {"gain_db", "増減", 2, "dB", 0.5}}, ""},
        {"Limiter", "リミッター", {{"threshold_db", "しきい値", -1, "dB", 0.5}, {"release_ms", "リリース", 100, "ms", 10}}, "これ以上大きくしない(音割れ防止)"},
    };
    return v;
}
const FxDef* findFx(const std::string& n) {
    for (auto& f : fxDefs()) if (n == f.name) return &f;
    return nullptr;
}

QString q(const std::string& s) { return QString::fromUtf8(s.c_str()); }
std::string u(const QString& s) { return s.toUtf8().toStdString(); }

vj::Value numV(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e9) return vj::Value::integer((int64_t)v);
    return vj::Value::number(v);
}

vj::Value normalizeStep() {
    vj::Value st = vj::Value::object();
    st.set("type", vj::Value::str("normalize"));
    st.set("lufs", vj::Value::integer(-14));
    st.set("peak_db", vj::Value::integer(-1));
    return st;
}

vj::Value fxStep(const FxDef& f) {
    vj::Value st = vj::Value::object();
    st.set("type", vj::Value::str("plugin"));
    st.set("name", vj::Value::str(f.name));
    vj::Value p = vj::Value::object();
    for (auto& pp : f.params) p.set(pp.key, numV(pp.def));
    st.set("params", p);
    return st;
}

// 処理スレッド
class Worker : public QThread {
    Q_OBJECT
public:
    std::string path;
    std::atomic<bool> cancel{false};
    vj::Value info;
    QString error;
    void run() override {
        try {
            ProcessOptions o;
            o.force = true;
            o.cancel = &cancel;
            o.exePath = u(QCoreApplication::applicationFilePath());
            o.progress = [this](double f, const std::string& m) { emit progress((int)(f * 100), q(m)); };
            info = processProject(path, o);
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
            if (error.isEmpty()) error = "失敗";
        }
    }
signals:
    void progress(int pct, QString msg);
};

}  // namespace

// 波形(最小・最大の包絡)
class WaveView : public QWidget {
public:
    explicit WaveView(const QString& title, QWidget* p = nullptr) : QWidget(p), title_(title) { setMinimumHeight(90); }
    void setAudio(const Audio& a) {
        env_.clear();
        if (a.empty() || a[0].empty()) { update(); return; }
        const int cols = 1200;
        size_t n = a[0].size();
        for (int c = 0; c < cols; c++) {
            size_t lo = n * (size_t)c / cols, hi = n * (size_t)(c + 1) / cols;
            if (hi <= lo) hi = lo + 1;
            float mn = 0, mx = 0;
            for (auto& ch : a)
                for (size_t i = lo; i < hi && i < ch.size(); i++) { mn = std::min(mn, ch[i]); mx = std::max(mx, ch[i]); }
            env_.push_back({mn, mx});
        }
        update();
    }
    void clear() { env_.clear(); update(); }
    bool empty() const { return env_.empty(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 26, 30));
        p.setPen(QColor(70, 74, 82));
        p.drawLine(0, height() / 2, width(), height() / 2);
        p.setPen(QColor(120, 200, 140));
        for (size_t i = 0; i < env_.size(); i++) {
            int x = (int)(i * (size_t)width() / env_.size());
            int y0 = height() / 2 - (int)(std::min(1.f, env_[i].second) * height() / 2);
            int y1 = height() / 2 - (int)(std::max(-1.f, env_[i].first) * height() / 2);
            p.drawLine(x, y0, x, y1);
        }
        p.setPen(QColor(200, 200, 200));
        p.drawText(6, 14, title_);
    }

private:
    QString title_;
    std::vector<std::pair<float, float>> env_;
};

AudioWindow::AudioWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("vaudio");
    auto* root = new QWidget(this);
    setCentralWidget(root);
    auto* top = new QHBoxLayout();
    auto* left = new QVBoxLayout();
    list_ = new QListWidget(this);
    list_->setObjectName("chainList");
    left->addWidget(new QLabel("音声処理(上から順に実行)"));
    left->addWidget(list_, 1);

    auto mk = [&](const QString& t, const char* name, auto fn) {
        auto* b = new QPushButton(t, this);
        b->setObjectName(name);
        connect(b, &QPushButton::clicked, this, fn);
        return b;
    };
    auto* r1 = new QHBoxLayout();
    r1->addWidget(mk("+ De-click", "addDeclick", [this] { addDeclick(); }));
    r1->addWidget(mk("+ ゲイン", "addGain", [this] { addGain(); }));
    r1->addWidget(mk("+ VST3…", "addVst3", [this] {
        QString f = QFileDialog::getOpenFileName(this, "VST3 を選ぶ", "C:/Program Files/Common Files/VST3", "VST3 (*.vst3)");
        if (!f.isEmpty()) addVst3(f);
    }));
    r1->addWidget(mk("+ ノーマライズ", "addNormalize", [this] { addNormalize(); }));
    left->addLayout(r1);
    fxBox_ = new QComboBox(this);
    fxBox_->setObjectName("fxBox");
    fxBox_->addItem("+ エフェクトを追加…", "");
    for (auto& f : fxDefs()) fxBox_->addItem(q(f.title), f.name);
    connect(fxBox_, &QComboBox::activated, this, [this](int i) {
        QString n = fxBox_->itemData(i).toString();
        fxBox_->setCurrentIndex(0);
        if (!n.isEmpty()) addPlugin(n);
    });
    left->addWidget(fxBox_);
    auto* r2 = new QHBoxLayout();
    r2->addWidget(mk("↑", "up", [this] { moveRow(currentRow(), -1); }));
    r2->addWidget(mk("↓", "down", [this] { moveRow(currentRow(), 1); }));
    r2->addWidget(mk("削除", "del", [this] { removeRow(currentRow()); }));
    left->addLayout(r2);
    left->addWidget(mk("声のおすすめ設定", "preset", [this] { applyVoicePreset(); }));
    top->addLayout(left, 1);

    auto* right = new QVBoxLayout();
    detail_ = new QWidget(this);
    form_ = new QFormLayout(detail_);
    right->addWidget(detail_, 1);
    top->addLayout(right, 1);

    auto* all = new QVBoxLayout(root);
    all->addLayout(top, 2);
    before_ = new WaveView("処理前", this);
    after_ = new WaveView("処理後", this);
    all->addWidget(before_);
    all->addWidget(after_);
    auto* run = new QHBoxLayout();
    run->addWidget(mk("音声処理を実行", "run", [this] { runProcess(); }));
    run->addWidget(mk("保存", "save", [this] {
        QString e;
        if (!saveProject(&e)) QMessageBox::warning(this, "vaudio", e);
    }));
    bar_ = new QProgressBar(this);
    bar_->setRange(0, 100);
    run->addWidget(bar_, 1);
    all->addLayout(run);
    note_ = new QLabel("プロジェクト未選択", this);
    all->addWidget(note_);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { rebuildDetail(); });
    chain_ = vj::Value::array();
    resize(1000, 720);
}

AudioWindow::~AudioWindow() {
    if (editor_) editor_->close();
}

int AudioWindow::currentRow() const { return list_->currentRow(); }
void AudioWindow::selectRow(int n) { list_->setCurrentRow(n); }

QString AudioWindow::stepTitle(const vj::Value& st) const {
    std::string t = st.getStr("type");
    if (t == "plugin") {
        auto* f = findFx(st.getStr("name"));
        return f ? q(f->title) : q(st.getStr("name"));
    }
    if (t == "declick") return "De-click (リップノイズ除去)";
    if (t == "gain") return "ゲイン";
    if (t == "normalize") return "ノーマライズ";
    if (t == "vst3") return "VST3";
    return q(t);
}

bool AudioWindow::loadProject(const QString& path, QString* err) {
    std::string txt;
    if (!readFileText(u(path), txt)) { if (err) *err = "読めません: " + path; return false; }
    try {
        doc_ = vj::parse(txt);
    } catch (const std::exception& e) {
        if (err) *err = QString::fromUtf8(e.what());
        return false;
    }
    if (!doc_.isObj()) { if (err) *err = "プロジェクトの形式が違います"; return false; }
    path_ = u(path);
    vj::Value* au = doc_.find("audio");
    chain_ = vj::Value::array();
    if (au && au->isObj()) {
        if (auto* c = au->find("chain")) if (c->isArr()) chain_ = c->clone();
    }
    stale_ = false;
    rebuildList();
    if (list_->count()) list_->setCurrentRow(0);
    setWindowTitle("vaudio - " + path);
    refreshWaves();
    note_->setText("読み込みました");
    return true;
}

bool AudioWindow::saveProject(QString* err) {
    if (path_.empty()) { if (err) *err = "プロジェクト未選択"; return false; }
    // ディスクの最新を読み直して audio.chain だけ差し替える(ほかのキーは触らない)
    std::string txt;
    vj::Value cur;
    if (readFileText(path_, txt)) {
        try { cur = vj::parse(txt); } catch (...) { cur = doc_.clone(); }
    } else cur = doc_.clone();
    vj::Value* au = cur.find("audio");
    if (au && au->isObj()) au->set("chain", chain_.clone());
    else {
        vj::Value a = vj::Value::object();
        a.set("chain", chain_.clone());
        cur.set("audio", a);
    }
    std::string out = vj::dump(cur, vj::DumpOpt{false, false, 2});
    if (!writeFileText(path_ + ".tmp", out)) { if (err) *err = "書けません"; return false; }
    if (!MoveFileExW(widenPath(path_ + ".tmp").c_str(), widenPath(path_).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        if (err) *err = "保存の置き換えに失敗";
        return false;
    }
    doc_ = cur;
    return true;
}

void AudioWindow::markStale(bool s) {
    stale_ = s;
    if (s) note_->setText("設定が変わりました。「音声処理を実行」で反映します");
}

void AudioWindow::rebuildList() {
    int keep = list_->currentRow();
    list_->blockSignals(true);
    list_->clear();
    for (size_t i = 0; i < chain_.size(); i++) list_->addItem(stepTitle(chain_.at(i)));
    if (keep >= 0 && keep < list_->count()) list_->setCurrentRow(keep);
    list_->blockSignals(false);
    rebuildDetail();
}

void AudioWindow::rebuildDetail() {
    while (form_->rowCount()) form_->removeRow(0);
    int n = list_->currentRow();
    if (n < 0 || (size_t)n >= chain_.size()) return;
    vj::Value& st = chain_.at((size_t)n);
    std::string t = st.getStr("type");
    auto addNum = [&](const QString& label, const std::string& key, bool inParams, double def, const QString& unit, double step, double mn, double mx) {
        auto* sp = new QDoubleSpinBox(detail_);
        sp->setObjectName(q("sp_" + key));
        sp->setRange(mn, mx);
        sp->setDecimals(2);
        sp->setSingleStep(step);
        if (!unit.isEmpty()) sp->setSuffix(" " + unit);
        double cur = def;
        if (inParams) { if (auto* p = st.find("params")) cur = p->getNum(key, def); }
        else cur = st.getNum(key, def);
        sp->setValue(cur);
        connect(sp, &QDoubleSpinBox::valueChanged, this, [this, n, key](double v) { setStepNumber(n, q(key), v); });
        form_->addRow(label, sp);
    };
    if (t == "declick") {
        addNum("しきい値", "threshold", true, 8, "", 0.5, 2, 30);
        form_->addRow(new QLabel("小さいほど強く直す(雑音を多く拾う)。標準 8"));
    } else if (t == "gain") {
        addNum("ゲイン", "db", false, 0, "dB", 0.5, -60, 60);
    } else if (t == "normalize") {
        bool lufs = st.find("lufs") != nullptr;
        auto* cb = new QComboBox(detail_);
        cb->setObjectName("normMode");
        cb->addItem("ラウドネス (LUFS)", "lufs");
        cb->addItem("ピーク", "peak");
        cb->setCurrentIndex(lufs ? 0 : 1);
        connect(cb, &QComboBox::activated, this, [this, n, cb](int i) { setNormalizeMode(n, cb->itemData(i).toString() == "lufs"); });
        form_->addRow("基準", cb);
        if (lufs) {
            addNum("目標", "lufs", false, -14, "LUFS", 0.5, -70, 0);
            addNum("上限ピーク", "peak_db", false, -1, "dB", 0.5, -60, 6);
            form_->addRow(new QLabel("YouTube等は -14 / 放送は -24。上限ピークを超える分はゲインを抑えます"));
        } else addNum("ピーク", "peak_db", false, -3, "dB", 0.5, -60, 6);
    } else if (t == "vst3") {
        form_->addRow(new QLabel(q(st.getStr("path"))));
        auto* b = new QPushButton("エディタを開く", detail_);
        b->setObjectName("openEditor");
        connect(b, &QPushButton::clicked, this, [this, n] { openVst3Editor(n); });
        form_->addRow(b);
        if (auto* p = st.find("params")) if (p->isObj() && p->size())
            form_->addRow(new QLabel(QString("設定済みパラメータ %1 件").arg(p->size())));
    } else if (t == "plugin") {
        if (auto* f = findFx(st.getStr("name"))) {
            for (auto& pp : f->params) addNum(q(pp.label), pp.key, true, pp.def, pp.unit, pp.step, -100000, 100000);
            if (*f->note) form_->addRow(new QLabel(q(f->note)));
        } else form_->addRow(new QLabel(q(st.getStr("name"))));
    }
}

void AudioWindow::insertStep(vj::Value st) {
    size_t nz = chain_.size();
    for (size_t i = 0; i < chain_.size(); i++)
        if (chain_.at(i).getStr("type") == "normalize") { nz = i; break; }
    chain_.a->insert(chain_.a->begin() + (long)nz, std::move(st));
    markStale();
    list_->setCurrentRow(-1);
    rebuildList();
    list_->setCurrentRow((int)nz);
}

void AudioWindow::addDeclick() {
    vj::Value st = vj::Value::object();
    st.set("type", vj::Value::str("declick"));
    vj::Value p = vj::Value::object();
    p.set("threshold", vj::Value::integer(8));
    st.set("params", p);
    insertStep(st);
}
void AudioWindow::addGain() {
    vj::Value st = vj::Value::object();
    st.set("type", vj::Value::str("gain"));
    st.set("db", vj::Value::integer(6));
    insertStep(st);
}
void AudioWindow::addVst3(const QString& path) {
    vj::Value st = vj::Value::object();
    st.set("type", vj::Value::str("vst3"));
    st.set("path", vj::Value::str(u(path)));
    st.set("params", vj::Value::object());
    insertStep(st);
}
void AudioWindow::addNormalize() {
    for (size_t i = 0; i < chain_.size(); i++)
        if (chain_.at(i).getStr("type") == "normalize") return;  // 1個だけ
    insertStep(normalizeStep());
}
void AudioWindow::addPlugin(const QString& name) {
    if (auto* f = findFx(u(name))) insertStep(fxStep(*f));
}
void AudioWindow::applyVoicePreset() {
    vj::Value nc = vj::Value::array();
    for (size_t i = 0; i < chain_.size(); i++) {
        std::string t = chain_.at(i).getStr("type");
        if (t == "declick" || t == "vst3") nc.push(chain_.at(i));
    }
    for (const char* n : {"HighpassFilter", "NoiseGate", "Compressor", "Limiter"}) nc.push(fxStep(*findFx(n)));
    nc.push(normalizeStep());
    chain_ = nc;
    markStale();
    rebuildList();
}
void AudioWindow::removeRow(int n) {
    if (n < 0 || (size_t)n >= chain_.size()) return;
    chain_.a->erase(chain_.a->begin() + n);
    markStale();
    rebuildList();
}
void AudioWindow::moveRow(int n, int d) {
    int m = n + d;
    if (n < 0 || m < 0 || (size_t)n >= chain_.size() || (size_t)m >= chain_.size()) return;
    // declick は常に最初・normalize は常に最後(app.js と同じ)
    auto fixed = [&](int i) { std::string t = chain_.at((size_t)i).getStr("type"); return t == "declick" || t == "normalize"; };
    if (fixed(n) || fixed(m)) return;
    std::swap(chain_.at((size_t)n), chain_.at((size_t)m));
    markStale();
    rebuildList();
    list_->setCurrentRow(m);
}
void AudioWindow::setStepNumber(int n, const QString& key, double v) {
    if (n < 0 || (size_t)n >= chain_.size()) return;
    vj::Value& st = chain_.at((size_t)n);
    std::string k = u(key), t = st.getStr("type");
    bool inParams = (t == "declick" || t == "plugin" || t == "vst3");
    vj::Value* tgt = &st;
    if (inParams) {
        vj::Value* p = st.find("params");
        if (!p || !p->isObj()) { st.set("params", vj::Value::object()); p = st.find("params"); }
        tgt = p;
    }
    tgt->set(k, numV(v));
    markStale();
}
void AudioWindow::setNormalizeMode(int n, bool lufs) {
    if (n < 0 || (size_t)n >= chain_.size()) return;
    vj::Value& st = chain_.at((size_t)n);
    if (lufs) { st.set("lufs", vj::Value::integer(-14)); st.set("peak_db", vj::Value::integer(-1)); }
    else { st.erase("lufs"); st.set("peak_db", vj::Value::integer(-3)); }
    markStale();
    rebuildDetail();
}

void AudioWindow::openVst3Editor(int n) {
    if (n < 0 || (size_t)n >= chain_.size() || editorWin_) return;
    vj::Value& st = chain_.at((size_t)n);
    if (st.getStr("type") != "vst3") return;
    try {
        auto pl = std::make_unique<Vst3Plugin>(st.getStr("path"));
        if (auto* p = st.find("params")) if (p->isObj())
            for (auto& kv : *p->o) pl->setParam(kv.first, kv.second);
        if (!pl->hasEditor()) { QMessageBox::information(this, "vaudio", "このプラグインにはエディタがありません"); return; }
        auto* w = new QWidget(nullptr, Qt::Window);
        w->setAttribute(Qt::WA_NativeWindow);
        w->setWindowTitle(q(pl->pluginName()));
        QWidget* wp = w;
        auto ed = pl->openEditor((void*)w->winId(), [wp](int cw, int ch) { wp->resize(cw, ch); });
        if (!ed) { delete w; QMessageBox::warning(this, "vaudio", "エディタを開けません"); return; }
        int ew = 0, eh = 0;
        ed->size(ew, eh);
        w->resize(ew, eh);
        editorPlugin_ = std::move(pl);
        editor_ = std::move(ed);
        editorWin_ = w;
        w->setAttribute(Qt::WA_DeleteOnClose);
        connect(w, &QObject::destroyed, this, [this, n] {
            // 閉じたとき、変わったパラメータを保存対象に反映する
            try {
                if (editorPlugin_ && (size_t)n < chain_.size()) {
                    vj::Value cp = editorPlugin_->changedParams();
                    if (cp.isObj()) { chain_.at((size_t)n).set("params", cp); markStale(); rebuildDetail(); }
                }
            } catch (...) {}
            if (editor_) editor_->close();
            editor_.reset();
            editorPlugin_.reset();
            editorWin_ = nullptr;
        });
        w->show();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "vaudio", QString::fromUtf8(e.what()));
    }
}

void AudioWindow::refreshWaves() {
    before_->clear();
    after_->clear();
    if (path_.empty()) return;
    try {
        std::string src = doc_.getStr("source");
        if (!src.empty()) before_->setAudio(decodeAudio(resolveInProject(path_, src), 8000).ch);
    } catch (...) {}
    try {
        std::string wav = defaultCacheDir(path_) + "/audio.wav";
        std::string tmp;
        if (readFileText(wav, tmp)) after_->setAudio(decodeAudio(wav, 8000).ch);
    } catch (...) {}
}

void AudioWindow::runProcess() {
    if (busy_ || path_.empty()) return;
    QString e;
    if (!saveProject(&e)) { QMessageBox::warning(this, "vaudio", e); return; }
    busy_ = true;
    bar_->setValue(0);
    auto* w = new Worker();
    w->path = path_;
    connect(w, &Worker::progress, this, [this](int p, QString m) { bar_->setValue(p); note_->setText(m); });
    connect(w, &QThread::finished, this, [this, w] {
        busy_ = false;
        bool ok = w->error.isEmpty();
        QString msg;
        if (ok) {
            bar_->setValue(100);
            stale_ = false;
            const vj::Value& i = w->info;
            msg = QString("処理済み: クリック修復 %1件 / ゲイン補正 %2dB").arg((int)i.getNum("clicks_repaired", 0)).arg(i.getNum("norm_gain_db", 0));
            refreshWaves();
        } else msg = "失敗: " + w->error;
        note_->setText(msg);
        emit processFinished(ok, msg);
        w->deleteLater();
    });
    w->start();
}

int runGui(const std::string& file) {
    int argc = 1;
    char a0[] = "vaudio";
    char* argv[] = {a0, nullptr};
    QApplication app(argc, argv);
    AudioWindow win;
    QString f = QString::fromUtf8(file.c_str());
    if (f.isEmpty()) f = QFileDialog::getOpenFileName(nullptr, "プロジェクトを開く", QString(), "vedit (*.vedit.json *.json)");
    if (!f.isEmpty()) {
        QString err;
        if (!win.loadProject(f, &err)) QMessageBox::warning(nullptr, "vaudio", err);
    }
    win.show();
    return app.exec();
}

#include "gui.moc"
