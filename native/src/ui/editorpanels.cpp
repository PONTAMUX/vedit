// 左右のパネルの中身(app.js: renderRight / styleTab / histTab / renderLeft / mediaTab / audioTab / cutTab の移植)
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <memory>
#include "media/membudget.h"
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include "editorwindow.h"
#include "timelineview.h"
#include "widgets.h"
#include "core/timeline.h"
#include "media/videostream.h"

namespace vui {

namespace {

// 0 を捨てて表示する数値欄(app.js の <input type=number> は 0.40 でなく 0.4 と出る)
class TrimSpin : public QDoubleSpinBox {
public:
    explicit TrimSpin(QWidget* p = nullptr) : QDoubleSpinBox(p) { setButtonSymbols(QAbstractSpinBox::NoButtons); setFocusPolicy(Qt::StrongFocus); setKeyboardTracking(true); }
    QString textFromValue(double v) const override {
        QString s = QString::number(v, 'f', decimals());
        if (s.contains('.')) { while (s.endsWith('0')) s.chop(1); if (s.endsWith('.')) s.chop(1); }
        return s;
    }
};

// スタイルの見本(.sample)。縁取りは外側から内側へ重ねて描く
class SampleView : public QWidget {
public:
    SampleView() { setFixedHeight(64); }
    OVal st;
    bool topic = false;
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#333355"));
        const double k = 0.5;
        QFont f(st.strOr("font", "Yu Gothic UI"));
        f.setPixelSize(std::max(6, (int)std::lround(st.numOr("size", 64) * k)));
        f.setBold(st.get("bold") && st.get("bold")->asBool());
        const QString text = topic ? QStringLiteral("トピック:サンプル") : QStringLiteral("サンプルテロップ");
        QFontMetrics fm(f);
        if (topic) {
            QColor bc(st.strOr("bar_color", "#3C3C3C"));
            bc.setAlphaF(std::clamp(st.numOr("bar_alpha", 1.0), 0.0, 1.0));
            const double px = st.numOr("pad_x", 19) * k, py = st.numOr("pad_y", 12) * k, aw = st.numOr("accent_w", 10) * k;
            const QRectF r(10, (height() - (fm.height() + 2 * py)) / 2, std::max(st.numOr("min_width", 0) * k, aw + 2 * px + fm.horizontalAdvance(text)), fm.height() + 2 * py);
            p.fillRect(r, bc);
            p.fillRect(QRectF(r.left(), r.top(), aw, r.height()), QColor(st.strOr("accent_color", "#c82828")));
            p.setFont(f);
            p.setPen(QColor(st.strOr("color", "#FFFFFF")));
            p.drawText(QPointF(r.left() + aw + px, r.top() + py + fm.ascent()), text);
            return;
        }
        QPainterPath path;
        const QPointF o((width() - fm.horizontalAdvance(text)) / 2.0, (height() + fm.ascent() - fm.descent()) / 2.0);
        path.addText(o, f, text);
        const OVal* outs = st.get("outlines");
        if (outs && outs->isArr()) {
            double cum = 0;
            std::vector<double> cw(outs->size());
            for (int i = outs->size() - 1; i >= 0; --i) { cum += outs->at(i).numOr("width", 0); cw[i] = cum; }
            for (int i = 0; i < outs->size(); ++i) {
                QPen pen(QColor(outs->at(i).strOr("color", "#000000")), 2 * cw[i] * k, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                p.strokePath(path, pen);
            }
        }
        p.fillPath(path, QColor(st.strOr("color", "#FFFFFF")));
    }
};

struct FxParam { const char *key, *label; double def; const char* unit; double step; };
struct FxDef { const char *name, *title; std::vector<FxParam> params; const char* note; };
const std::vector<FxDef>& fxTable() {
    static const std::vector<FxDef> t = {
        {"HighpassFilter", "ローカット(ハイパス)", {{"cutoff_frequency_hz", "周波数", 80, "Hz", 5}}, "低いゴロゴロ・吹かれを消す"},
        {"NoiseGate", "ノイズゲート", {{"threshold_db", "しきい値", -45, "dB", 1}, {"ratio", "比率", 4, ":1", 0.5}, {"release_ms", "リリース", 150, "ms", 10}}, "しきい値より小さい音(無音部の雑音)を下げる"},
        {"Compressor", "コンプレッサー", {{"threshold_db", "しきい値", -18, "dB", 1}, {"ratio", "比率", 3, ":1", 0.5}, {"attack_ms", "アタック", 5, "ms", 1}, {"release_ms", "リリース", 120, "ms", 10}}, "声の大小をならす"},
        {"LowShelfFilter", "EQ 低域", {{"cutoff_frequency_hz", "周波数", 200, "Hz", 10}, {"gain_db", "増減", -3, "dB", 0.5}}, ""},
        {"PeakFilter", "EQ 中域(ピーク)", {{"cutoff_frequency_hz", "周波数", 3000, "Hz", 50}, {"gain_db", "増減", 2, "dB", 0.5}, {"q", "Q(幅)", 1, "", 0.1}}, ""},
        {"HighShelfFilter", "EQ 高域", {{"cutoff_frequency_hz", "周波数", 8000, "Hz", 100}, {"gain_db", "増減", 2, "dB", 0.5}}, ""},
        {"Limiter", "リミッター", {{"threshold_db", "しきい値", -1, "dB", 0.5}, {"release_ms", "リリース", 100, "ms", 10}}, "これ以上大きくしない(音割れ防止)"},
    };
    return t;
}
const FxDef* fxFind(const QString& n) {
    for (const FxDef& f : fxTable()) if (n == f.name) return &f;
    return nullptr;
}

using Opts = QList<QPair<QString, QString>>;
const Opts kAnimT = {{"none", "なし"}, {"fade", "フェード"}, {"pop", "ポップ"}, {"slide-up", "下からスライド"}};
const Opts kAnimB = {{"none", "なし"}, {"fade", "フェード"}};
const Opts kAnimO = {{"none", "なし"}, {"fade", "フェード"}, {"slide-left", "左へスライド"}, {"slide-right", "右へスライド"}, {"slide-up", "下へスライド"}, {"slide-down", "上へスライド"}};

OVal defaultStyle(const QString& name) {
    static const char* telop = R"({"font":"HGP創英角ｺﾞｼｯｸUB","size":64,"bold":false,"color":"#FFFFFF","outlines":[{"color":"#000000","width":6}],"align":"bottom","margin_v":70})";
    static const char* topic = R"({"font":"HGP創英角ｺﾞｼｯｸUB","size":43,"bold":false,"color":"#FFFFFF","bar_color":"#3C3C3C","bar_alpha":1.0,"accent_color":"#C82828","accent_w":10,"pad_x":19,"pad_y":12,"x":0,"y":40,"min_width":0})";
    OVal v;
    OVal::parse(QByteArray(name == "topic" ? topic : telop), &v);
    return v;
}

QString canon(const OVal& v) {
    if (v.isArr()) { QStringList l; for (const OVal& x : v.items()) l << canon(x); return "[" + l.join(",") + "]"; }
    if (v.isObj()) {
        QStringList ks;
        for (int i = 0; i < v.size(); ++i) ks << v.keyAt(i);
        ks.sort();
        QStringList l;
        for (const QString& k : ks) l << OVal::str(k).dump(false) + ":" + canon(*v.get(k));
        return "{" + l.join(",") + "}";
    }
    return v.dump(false);
}

struct AudioInfo {
    bool hasJson = false, hasWav = false;
    OVal key;           // 処理した時の [source, mtime, chain]
    OVal json;
};

}  // namespace

// ---- フォーム部品(app.js の bind / fNum / fText / fColor / fCheck / fSel) ----
struct PanelForm {
    static EditModel* M(EditorWindow* w) { return w->m_; }
    static OVal* cur(EditorWindow* w) {
        EditModel* m = w->m_;
        if (m->selKind.isEmpty() || m->selI < 0) return nullptr;
        OVal* l = m->d().get(m->selKind);
        if (!l || !l->isArr() || m->selI >= l->size()) return nullptr;
        return &l->at(m->selI);
    }
    // 入力中は欄を作り直さない(inspector=false)。選択の変更など作り直したい時は true
    static void commitIn(EditorWindow* w, bool inspector, bool keepOverlap) {
        if (inspector) { w->m_->commit(keepOverlap); return; }
        const bool was = w->panelEditing_;
        w->panelEditing_ = true;
        w->m_->commit(keepOverlap);
        w->panelEditing_ = was;
    }
    static QWidget* rowOf(const QString& label, QWidget* c, const QString& unit = QString(), QWidget* c2 = nullptr) {
        auto* r = new QWidget;
        auto* h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        if (!label.isEmpty()) {
            auto* l = new QLabel(label);
            l->setProperty("class", "rowlabel");
            l->setMinimumWidth(72);
            h->addWidget(l);
        }
        h->addWidget(c, c->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding ? 1 : 0);
        if (c2) h->addWidget(c2);
        if (!unit.isEmpty()) { auto* u = new QLabel(unit); u->setProperty("class", "dim"); h->addWidget(u); }
        if (c->sizePolicy().horizontalPolicy() != QSizePolicy::Expanding) h->addStretch(1);
        return r;
    }
    static QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("class", "h4"); return l; }
    static QLabel* note(const QString& t, bool warn = false) {
        auto* l = new QLabel(t);
        l->setProperty("class", warn ? "notewarn" : "note");
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        return l;
    }
    static QPushButton* btn(const QString& t, std::function<void()> fn, bool danger = false, bool primary = false) {
        auto* b = new QPushButton(t);
        b->setFocusPolicy(Qt::NoFocus);
        b->setCursor(Qt::PointingHandCursor);
        if (danger) b->setProperty("danger", true);
        if (primary) b->setObjectName("primary");
        QObject::connect(b, &QPushButton::clicked, b, [fn] { fn(); });
        return b;
    }
    static QWidget* btnRow(std::initializer_list<QWidget*> l) {
        auto* r = new QWidget;
        auto* h = new QHBoxLayout(r);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(6);
        for (QWidget* b : l) if (b) h->addWidget(b);
        h->addStretch(1);
        return r;
    }
    // 数値。入力のたびに 1 回だけ snap → 反映 → commit(keepOverlap)。入力が終わったら commit() で重なりを詰める
    static QWidget* num(EditorWindow* w, const QString& label, double val, std::function<void(double)> set, double step = 0.1, double mn = -1e9, double mx = 1e9,
                        const QString& unit = QString(), bool inspector = false) {
        auto* sp = new TrimSpin;
        sp->setDecimals(4);
        sp->setRange(mn, mx);
        sp->setSingleStep(step);
        sp->setValue(val);
        sp->setFixedWidth(84);
        sp->setObjectName("num_" + label);
        auto st = std::make_shared<bool>(false);
        QObject::connect(sp, &QDoubleSpinBox::valueChanged, sp, [=](double v) {
            if (!*st) { w->m_->snap(); *st = true; }
            set(v);
            commitIn(w, inspector, true);
        });
        QObject::connect(sp, &QAbstractSpinBox::editingFinished, sp, [=] {
            if (*st) commitIn(w, inspector, false);
            *st = false;
        });
        return rowOf(label, sp, unit);
    }
    static QWidget* text(EditorWindow* w, const QString& label, const QString& val, std::function<void(const QString&)> set, bool inspector = false, const QStringList& complete = {}, int width = 0) {
        auto* e = new QLineEdit(val);
        e->setObjectName("text_" + label);
        if (width) e->setFixedWidth(width);
        else e->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        if (!complete.isEmpty()) {
            auto* c = new QCompleter(complete, e);
            c->setCaseSensitivity(Qt::CaseInsensitive);
            e->setCompleter(c);
        }
        auto st = std::make_shared<bool>(false);
        QObject::connect(e, &QLineEdit::textEdited, e, [=](const QString& v) {
            if (!*st) { w->m_->snap(); *st = true; }
            set(v);
            commitIn(w, inspector, true);
        });
        QObject::connect(e, &QLineEdit::editingFinished, e, [=] {
            if (*st) commitIn(w, inspector, false);
            *st = false;
        });
        return rowOf(label, e);
    }
    static QPlainTextEdit* area(EditorWindow* w, const QString& val, std::function<void(const QString&)> set) {
        auto* e = new QPlainTextEdit(val);
        e->setObjectName("textarea");
        e->setFixedHeight(64);
        auto st = std::make_shared<bool>(false);
        QObject::connect(e, &QPlainTextEdit::textChanged, e, [=] {
            if (!*st) { w->m_->snap(); *st = true; }
            set(e->toPlainText());
            commitIn(w, false, true);
        });
        return e;
    }
    static QPushButton* colorBtn(EditorWindow* w, const QString& val, std::function<void(const QString&)> set) {
        auto* b = new QPushButton;
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(46, 24);
        b->setProperty("color", val);
        b->setObjectName("color");
        auto paint = [b] { b->setStyleSheet(QString("QPushButton{background:%1;border:1px solid #3a3a43;border-radius:3px;}").arg(b->property("color").toString())); };
        paint();
        QObject::connect(b, &QPushButton::clicked, b, [=] {
            QString c;
            const QString curc = b->property("color").toString();
            if (w->pickColor) c = w->pickColor(curc);
            else { const QColor q = QColorDialog::getColor(QColor(curc), w); if (q.isValid()) c = q.name(); }
            if (c.isEmpty() || c == curc) return;
            w->m_->snap();
            b->setProperty("color", c);
            paint();
            set(c);
            commitIn(w, false, true);
            commitIn(w, false, false);
        });
        return b;
    }
    static QWidget* color(EditorWindow* w, const QString& label, const QString& val, std::function<void(const QString&)> set) { return rowOf(label, colorBtn(w, val, set)); }
    static QWidget* check(EditorWindow* w, const QString& label, bool val, std::function<void(bool)> set, bool inspector = false) {
        auto* c = new QCheckBox;
        c->setChecked(val);
        c->setObjectName("check_" + label);
        c->setFocusPolicy(Qt::NoFocus);
        QObject::connect(c, &QCheckBox::toggled, c, [=](bool on) {
            w->m_->snap();
            set(on);
            commitIn(w, inspector, false);
        });
        return rowOf(label, c);
    }
    static QComboBox* combo(EditorWindow* w, const Opts& opts, const QString& curv, std::function<void(const QString&)> set, bool inspector = true) {
        auto* s = new QComboBox;
        s->setFocusPolicy(Qt::StrongFocus);
        int ci = -1;
        for (int i = 0; i < opts.size(); ++i) { s->addItem(opts[i].second, opts[i].first); if (opts[i].first == curv) ci = i; }
        s->setCurrentIndex(ci);
        QObject::connect(s, &QComboBox::currentIndexChanged, s, [=](int i) {
            if (i < 0) return;
            w->m_->snap();
            set(s->itemData(i).toString());
            commitIn(w, inspector, false);
        });
        return s;
    }
    static QWidget* sel(EditorWindow* w, const QString& label, const Opts& opts, const QString& curv, std::function<void(const QString&)> set, bool inspector = true) {
        auto* c = combo(w, opts, curv, set, inspector);
        c->setObjectName("sel_" + label);
        return rowOf(label, c);
    }
    static OVal* chain(EditorWindow* w) {
        OVal* a = w->m_->d().get("audio");
        if (!a || !a->isObj()) return nullptr;
        OVal* c = a->get("chain");
        return c && c->isArr() ? c : nullptr;
    }
    static OVal paramsFor(const FxDef& f) {
        OVal p = OVal::object();
        for (const FxParam& q : f.params) p.set(q.key, OVal::num(q.def));
        return p;
    }
    static OVal fxStep(const FxDef& f) {
        OVal s = OVal::object();
        s.set("type", OVal::str("plugin"));
        s.set("name", OVal::str(f.name));
        s.set("params", paramsFor(f));
        return s;
    }
    static void chainInsert(EditorWindow* w, const OVal& step) {   // ノーマライズの手前(無ければ末尾)
        w->m_->mutate([&] {
            OVal* c = chain(w);
            if (!c) return;
            int nz = -1;
            for (int i = 0; i < c->size(); ++i) if (c->at(i).strOr("type", "") == "normalize") { nz = i; break; }
            c->insert(nz < 0 ? c->size() : nz, step);
        });
    }
    static void setN(OVal& o, const char* k, double v) { o.set(k, OVal::num(v)); }
    static OVal& ensureObj(OVal& o, const char* k) {
        OVal* x = o.get(k);
        if (!x || !x->isObj()) { o.set(k, OVal::object()); x = o.get(k); }
        return *x;
    }
    static AudioInfo audioInfo(EditorWindow* w) {
        AudioInfo a;
        const QString cd = w->m_->dir() + "/" + QFileInfo(w->m_->path()).completeBaseName() + ".cache";
        a.hasWav = QFileInfo::exists(cd + "/audio.wav");
        QFile f(cd + "/audio.json");
        if (f.open(QIODevice::ReadOnly) && OVal::parse(f.readAll(), &a.json) && a.json.isObj()) {
            a.hasJson = true;
            const OVal* k = a.json.get("key");
            if (k && k->isStr()) OVal::parse(k->asStr().toUtf8(), &a.key);
        }
        return a;
    }
};

// ---- 音声の古さ ----
bool EditorWindow::audioStale() const {
    const AudioInfo a = PanelForm::audioInfo(const_cast<EditorWindow*>(this));
    if (!a.hasJson || !a.hasWav || !a.key.isArr() || a.key.size() < 3) return false;
    const OVal* au = m_->d().get("audio");
    const OVal* ch = au ? au->get("chain") : nullptr;
    if (!ch) return false;
    return canon(a.key.at(2)) != canon(*ch);
}

QString EditorWindow::audioNoteText() const {
    const AudioInfo a = PanelForm::audioInfo(const_cast<EditorWindow*>(this));
    if (audioStale()) return QStringLiteral("⚠ 設定が変わりました。「音声処理を実行」で反映します(今は元の音で再生)");
    if (a.hasJson && a.hasWav) {
        const OVal* cl = a.json.get("clicks_repaired");
        QString s = QStringLiteral("✔ 処理済み: クリック修復 %1件 / ゲイン補正 %2dB")
                        .arg(cl && cl->isNum() ? fmtNumber(cl->asNum()) : QStringLiteral("-"))
                        .arg(a.json.get("norm_gain_db") && a.json.get("norm_gain_db")->isNum() ? fmtNumber(a.json.get("norm_gain_db")->asNum()) : QStringLiteral("undefined"));
        const OVal* la = a.json.get("lufs_after");
        if (la && la->isNum()) {
            const OVal* lb = a.json.get("lufs_before");
            s += QStringLiteral(" / %1→%2 LUFS").arg(lb && lb->isNum() ? fmtNumber(lb->asNum()) : QStringLiteral("null"), fmtNumber(la->asNum()));
        }
        return s;
    }
    return QStringLiteral("未処理(元の音のまま再生)");
}

void EditorWindow::updAudioWarn() {
    if (!bRaw_) return;
    const AudioInfo a = PanelForm::audioInfo(this);
    const bool stale = audioStale();
    const OVal* au = m_->d().get("audio");
    const OVal* ch = au ? au->get("chain") : nullptr;
    const bool raw = stale || (!a.hasWav && ch && ch->isArr() && ch->size() > 0);
    if (audioJob_ && audioJob_->running()) return;      // 処理中の表示は runAudio が持つ
    bRaw_->setText(stale ? QStringLiteral("⚠ 元の音で再生中(音声設定が変わった)") : QStringLiteral("⚠ 元の音で再生中(未処理)"));
    bRaw_->setVisible(raw && cProc_ && cProc_->isChecked());
}

// ---- スタイル ----
QStringList EditorWindow::styleNames() const {
    QStringList l{"telop"};
    const OVal* s = m_->d().get("styles");
    if (s && s->isObj()) for (int i = 0; i < s->size(); ++i) if (s->keyAt(i) != "telop" && s->keyAt(i) != "topic") l << s->keyAt(i);
    return l;
}

OVal EditorWindow::effStyle(const QString& name) const {
    OVal base = defaultStyle(name == "topic" ? "topic" : "telop");
    const OVal* s = m_->d().get("styles");
    const OVal* o = s && s->isObj() ? s->get(name) : nullptr;
    if (o && o->isObj()) for (int i = 0; i < o->size(); ++i) base.set(o->keyAt(i), o->at(i));
    return base;
}

void EditorWindow::editStyle(const QString& name, const std::function<void(OVal&)>& fn) {
    OVal& d = m_->d();
    OVal* s = d.get("styles");
    if (!s || !s->isObj()) { d.set("styles", OVal::object()); s = d.get("styles"); }
    if (!s->get(name)) s->set(name, effStyle(name));
    fn(*s->get(name));
}

QStringList EditorWindow::fonts() {
    if (fontsLoaded_) return fonts_;
    fontsLoaded_ = true;
    if (fontList) fonts_ = fontList();
    else {
        QSettings reg(QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"), QSettings::NativeFormat);
        QSet<QString> seen;
        for (const QString& k : reg.allKeys()) {
            QString n = k;
            n.replace('\\', '/');
            n = n.section('/', -1);
            n.remove(QRegularExpression(R"(\s*\((TrueType|OpenType|All res)\)\s*$)"));
            for (const QString& part : n.split(" & ")) {
                const QString t = part.trimmed();
                if (!t.isEmpty() && !seen.contains(t)) { seen.insert(t); fonts_ << t; }
            }
        }
        fonts_.sort(Qt::CaseInsensitive);
    }
    return fonts_;
}

// ---- 入口 ----
// 欄に出ている中身の印。空なら毎回作り直す。素材タブ(左0)とクリップタブ(右0)だけ印を作る。ほかのタブは
// 読むものが多い(音声の状態・カット一覧・履歴など)ので毎回作り直す
QString EditorWindow::panelSig(bool left) const {
    OVal& p = m_->d();
    auto dumpOf = [&](const char* k) { const OVal* o = p.get(k); return o ? o->dump(false) : QString(); };
    if (left) {
        if (leftTab_ != 0) return {};
        return "L0|" + p.strOr("source", "") + "|" + dumpOf("cut_fx") + "|" + dumpOf("bgm") + "|" + dumpOf("output");
    }
    if (rightTab_ != 0) return {};
    QString s = "R0|" + m_->selKind + "|" + QString::number(m_->selI) + "|" + styleTarget_;
    if (const OVal* it = PanelForm::cur(const_cast<EditorWindow*>(this))) s += "|" + it->dump(false);
    return s;
}

void EditorWindow::fillLeft() {
    if (!leftScroll_) return;
    leftSig_ = panelSig(true);
    auto* host = new QWidget;
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(12, 10, 12, 12);
    v->setSpacing(6);
    if (leftTab_ == 0) mediaTab(v);
    else if (leftTab_ == 1) audioTab(v);
    else cutTab(v);
    v->addStretch(1);
    const int sv = leftScroll_->verticalScrollBar()->value();
    setBody(true, host);
    leftScroll_->verticalScrollBar()->setValue(sv);
}

void EditorWindow::fillRight() {
    if (!rightScroll_) return;
    rightSig_ = panelSig(false);
    auto* host = new QWidget;
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(12, 10, 12, 12);
    v->setSpacing(6);
    if (rightTab_ == 0) clipTab(v);
    else if (rightTab_ == 1) styleTab(v);
    else if (rightTab_ == 2) historyTab(v);
    else settingsTab(v);
    v->addStretch(1);
    const int sv = rightScroll_->verticalScrollBar()->value();
    setBody(false, host);
    rightScroll_->verticalScrollBar()->setValue(sv);
}

// ---- 右: クリップ ----
void EditorWindow::clipTab(QVBoxLayout* v) {
    using F = PanelForm;
    EditorWindow* w = this;
    OVal* it = F::cur(this);
    if (!it) {
        v->addWidget(F::note(QStringLiteral("タイムラインのクリップを選ぶと、ここで編集できます。")));
        v->addWidget(F::note(QStringLiteral("ショートカットは Premiere の設定を取り込めます(vedit import-keys)。主なキー: Space 再生 / E・Ctrl+K 分割 / C 分割ツール / Q・W 前後の編集点まで削除 / Delete・Shift+Delete 選択区間を削除(詰める) / I・O・; 範囲を削除 / ↑↓ 編集点へ移動 / J・K・L シャトル / Ctrl+Z・Ctrl+Shift+Z / T テロップ・B トピック")));
        auto* stop = new QCheckBox;
        stop->setChecked(prefGet("vedit.stopOnTrim", "1") != "0");
        stop->setFocusPolicy(Qt::NoFocus);
        QObject::connect(stop, &QCheckBox::toggled, stop, [](bool on) { prefSet("vedit.stopOnTrim", on ? "1" : "0"); });
        v->addWidget(F::rowOf(QStringLiteral("Q・W・シークで再生を止める"), stop));
        return;
    }
    const QString kind = m_->selKind;
    auto itp = [w]() { return F::cur(w); };   // 適用の時に選択中の項目を引き直す(配列が組み替わっても安全)
    auto delBtn = [this] { return F::btn(QStringLiteral("削除"), [this] { m_->deleteSel(); }, true); };
    auto tm = [&]() {
        v->addWidget(F::num(w, QStringLiteral("開始"), toFixedNum(it->numOr("start", 0), 2), [itp](double x) { if (auto* o = itp()) F::setN(*o, "start", x); }, 0.05));
        v->addWidget(F::num(w, QStringLiteral("終了"), toFixedNum(it->numOr("end", 0), 2), [itp](double x) { if (auto* o = itp()) F::setN(*o, "end", x); }, 0.05));
    };
    if (kind == "cuts") {
        v->addWidget(F::h4(QStringLiteral("カット範囲(削除される部分)")));
        const double a = it->size() > 0 ? it->at(0).asNum() : 0, b = it->size() > 1 ? it->at(1).asNum() : 0;
        v->addWidget(F::num(w, QStringLiteral("開始"), toFixedNum(a, 2), [itp](double x) { if (auto* o = itp()) if (o->size() > 0) o->at(0) = OVal::num(x); }, 0.05));
        v->addWidget(F::num(w, QStringLiteral("終了"), toFixedNum(b, 2), [itp](double x) { if (auto* o = itp()) if (o->size() > 1) o->at(1) = OVal::num(x); }, 0.05));
        v->addWidget(F::btnRow({F::btn(QStringLiteral("このカットを取り消す"), [this] { m_->deleteSel(); }, true)}));
        return;
    }
    if (kind == "telops" || kind == "topics") {
        const bool tel = kind == "telops";
        v->addWidget(F::h4(tel ? QStringLiteral("テロップ") : QStringLiteral("トピックバー")));
        v->addWidget(F::area(this, it->strOr("text", ""), [itp](const QString& s) { if (auto* o = itp()) o->set("text", OVal::str(s)); }));
        tm();
        if (tel) {
            QList<QPair<QString, QString>> names;
            for (const QString& n : styleNames()) names.push_back({n, n});
            const QString cs = it->strOr("style", "");
            v->addWidget(F::sel(w, QStringLiteral("スタイル"), names, cs.isEmpty() ? QStringLiteral("telop") : cs, [itp](const QString& s) {
                if (auto* o = itp()) { if (s == "telop") o->remove("style"); else o->set("style", OVal::str(s)); }
            }));
            v->addWidget(F::sel(w, QStringLiteral("出入り"), kAnimT, it->strOr("anim", "none"), [itp](const QString& s) { if (auto* o = itp()) o->set("anim", OVal::str(s)); }));
            const OVal* ps = it->get("pos");
            const bool hasPos = ps && ps->asBool();
            v->addWidget(F::btnRow({F::btn(QStringLiteral("スタイルを編集…"), [this, it] {
                                        const QString s = it->strOr("style", "");
                                        styleTarget_ = s.isEmpty() ? QStringLiteral("telop") : s;
                                        setRightTab(1);
                                    }),
                                    hasPos ? F::btn(QStringLiteral("位置を初期に戻す"), [this] { m_->mutate([this] { if (auto* o = PanelForm::cur(this)) o->remove("pos"); }); }) : nullptr}));
            v->addWidget(F::note(QStringLiteral("モニター上でドラッグすると自由に動かせます。")));
        } else {
            v->addWidget(F::sel(w, QStringLiteral("出入り"), kAnimB, it->strOr("anim", "none"), [itp](const QString& s) { if (auto* o = itp()) o->set("anim", OVal::str(s)); }));
        }
        const QString an = it->strOr("anim", "none");
        if (!an.isEmpty() && an != "none") v->addWidget(F::num(w, QStringLiteral("アニメ秒"), it->numOr("anim_dur", 0.2), [itp](double x) { if (auto* o = itp()) F::setN(*o, "anim_dur", x); }, 0.05, 0.05));
        v->addWidget(F::btnRow({delBtn()}));
        return;
    }
    if (kind == "sfx") {
        const double L = m_->sfxFull(*it);
        v->addWidget(F::h4(QStringLiteral("効果音")));
        v->addWidget(F::note(it->strOr("file", "")));
        tm();
        v->addWidget(F::num(w, QStringLiteral("音量"), it->numOr("gain_db", 0), [itp](double x) { if (auto* o = itp()) F::setN(*o, "gain_db", x); }, 0.5, -1e9, 1e9, "dB"));
        v->addWidget(F::num(w, QStringLiteral("音の開始"), it->numOr("in", 0), [this, itp](double x) {
            auto* o = itp();
            if (!o) return;
            const bool autoEnd = m_->sfxAuto(*o);
            F::setN(*o, "in", std::max(0.0, x));
            if (autoEnd) F::setN(*o, "end", toFixedNum(o->numOr("start", 0) + m_->sfxLen(*o), 3));
        }, 0.05, 0, 1e9, QStringLiteral("秒(頭を飛ばす)")));
        v->addWidget(F::btnRow({L >= 0 ? F::btn(QStringLiteral("長さを音に合わせる"), [this] {
                                              m_->mutate([this] { if (auto* o = PanelForm::cur(this)) PanelForm::setN(*o, "end", toFixedNum(o->numOr("start", 0) + m_->sfxLen(*o), 3)); });
                                          }) : nullptr,
                                delBtn()}));
        v->addWidget(F::note(QStringLiteral("時刻は差し込みと同じ元素材の秒。カットで消えた所の効果音は鳴りません。書き出しでは本編(処理済み)・BGM と最後にミックスします。")));
        return;
    }
    // overlays
    const Opts posOpts = {{"full", "全画面"}, {"center", "中央"}, {"top-left", "左上"}, {"top-right", "右上"}, {"bottom-left", "左下"}, {"bottom-right", "右下"}};
    v->addWidget(F::h4(QStringLiteral("映像・画像の差し込み")));
    v->addWidget(F::note(it->get("file") && !it->strOr("file", "").isEmpty() ? it->strOr("file", "") : QStringLiteral("✦ ") + (it->get("kind") && it->get("kind")->isStr() ? it->strOr("kind", "?") : QStringLiteral("?"))));
    tm();
    const OVal* pp = it->get("pos");
    QString posCur;
    if (pp && pp->isArr()) posCur = "center";
    else if (pp && pp->isStr() && !pp->asStr().isEmpty()) posCur = pp->asStr();
    else posCur = it->get("scale") && it->get("scale")->isNum() ? "top-right" : "full";
    v->addWidget(F::sel(w, QStringLiteral("配置"), posOpts, posCur, [itp](const QString& s) {
        auto* o = itp();
        if (!o) return;
        o->set("pos", OVal::str(s));
        if (s != "full" && !(o->get("scale") && !o->get("scale")->isNull())) F::setN(*o, "scale", 0.35);
    }));
    const bool notFull = (pp && pp->isArr()) || (pp && pp->isStr() && !pp->asStr().isEmpty() && pp->asStr() != "full");
    if (notFull) v->addWidget(F::num(w, QStringLiteral("大きさ"), it->numOr("scale", 0.3), [itp](double x) { if (auto* o = itp()) F::setN(*o, "scale", x); }, 0.05, 0.05, 1, QStringLiteral("(画面幅比)")));
    else v->addWidget(F::sel(w, QStringLiteral("はめ方"), {{"contain", "全体を表示"}, {"cover", "画面を埋める"}}, it->strOr("fit", "contain"), [itp](const QString& s) { if (auto* o = itp()) o->set("fit", OVal::str(s)); }, false));
    v->addWidget(F::num(w, QStringLiteral("素材の開始"), it->numOr("in", 0), [itp](double x) { if (auto* o = itp()) F::setN(*o, "in", x); }, 0.1, 0, 1e9, QStringLiteral("秒(動画のみ)")));
    v->addWidget(F::check(w, QStringLiteral("音も入れる"), it->get("audio") && it->get("audio")->asBool(), [itp](bool on) { if (auto* o = itp()) o->set("audio", OVal::boolean(on)); }));
    v->addWidget(F::sel(w, QStringLiteral("入り"), kAnimO, it->strOr("in_anim", "none"), [itp](const QString& s) { if (auto* o = itp()) o->set("in_anim", OVal::str(s)); }, false));
    v->addWidget(F::sel(w, QStringLiteral("出"), kAnimO, it->strOr("out_anim", "none"), [itp](const QString& s) { if (auto* o = itp()) o->set("out_anim", OVal::str(s)); }, false));
    v->addWidget(F::num(w, QStringLiteral("アニメ秒"), it->numOr("anim_dur", 0.3), [itp](double x) { if (auto* o = itp()) F::setN(*o, "anim_dur", x); }, 0.05, 0.05));
    v->addWidget(F::btnRow({delBtn()}));
}

// ---- 右: スタイル ----
void EditorWindow::styleTab(QVBoxLayout* v) {
    using F = PanelForm;
    EditorWindow* w = this;
    QStringList names = styleNames();
    names << "topic";
    if (!names.contains(styleTarget_)) styleTarget_ = "telop";
    const QString target = styleTarget_;
    const bool topic = target == "topic";
    auto stNow = [w, target]() { return w->effStyle(target); };
    OVal st = stNow();

    Opts nopts;
    for (const QString& n : names) nopts.push_back({n, n == "topic" ? QStringLiteral("トピックバー") : n});
    auto* tsel = F::combo(this, nopts, target, [](const QString&) {}, true);
    tsel->disconnect();   // 対象の切り替えは編集ではない(snap/commit しない)
    connect(tsel, &QComboBox::currentIndexChanged, this, [this, tsel](int i) { if (i >= 0) { styleTarget_ = tsel->itemData(i).toString(); QTimer::singleShot(0, this, [this] { fillRight(); }); } });
    tsel->setObjectName("style-target");
    v->addWidget(F::rowOf(QStringLiteral("対象"), tsel, QString(), F::btn(QStringLiteral("+ 新規"), [this] {
                              const QString n = askLine(QStringLiteral("新しいスタイル名"), QString());
                              const OVal* s = m_->d().get("styles");
                              if (n.isEmpty() || n == "topic" || (s && s->get(n))) return;
                              m_->mutate([&] { editStyle(n, [](OVal&) {}); });
                              styleTarget_ = n;
                              fillRight();
                          })));
    fonts();
    auto* sample = new SampleView;
    sample->topic = topic;
    sample->st = st;
    sample->setObjectName("style-sample");
    sample->setProperty("class", "sample");
    v->addWidget(sample);
    QPointer<SampleView> sp(sample);
    auto redo = [w, sp, target]() { if (sp) { sp->st = w->effStyle(target); sp->update(); } };
    auto setK = [w, target, redo](const char* k, const OVal& val) { w->editStyle(target, [&](OVal& s) { s.set(k, val); }); redo(); };
    auto S2 = [&](const QString& label, const char* key, double step = 1) {
        v->addWidget(F::num(w, label, st.numOr(key, 0), [setK, key](double x) { setK(key, OVal::num(x)); }, step, 0));
    };
    v->addWidget(F::text(w, QStringLiteral("フォント"), st.strOr("font", ""), [setK](const QString& s) { setK("font", OVal::str(s)); }, false, fonts_, 170));
    S2(QStringLiteral("大きさ"), "size");
    v->addWidget(F::check(w, QStringLiteral("太字"), st.get("bold") && st.get("bold")->asBool(), [setK](bool on) { setK("bold", OVal::boolean(on)); }));
    v->addWidget(F::color(w, QStringLiteral("文字色"), st.strOr("color", "#ffffff"), [setK](const QString& s) { setK("color", OVal::str(s)); }));
    if (topic) {
        v->addWidget(F::color(w, QStringLiteral("帯の色"), st.strOr("bar_color", "#3C3C3C"), [setK](const QString& s) { setK("bar_color", OVal::str(s)); }));
        v->addWidget(F::color(w, QStringLiteral("アクセント色"), st.strOr("accent_color", "#c82828"), [setK](const QString& s) { setK("accent_color", OVal::str(s)); }));
        S2(QStringLiteral("アクセント幅"), "accent_w");
        v->addWidget(F::num(w, QStringLiteral("帯の濃さ"), st.numOr("bar_alpha", 1), [setK](double x) { setK("bar_alpha", OVal::num(x)); }, 0.05, 0, 1));
        S2(QStringLiteral("余白 横"), "pad_x");
        S2(QStringLiteral("余白 縦"), "pad_y");
        S2(QStringLiteral("位置 X"), "x");
        S2(QStringLiteral("位置 Y"), "y");
        S2(QStringLiteral("最小幅"), "min_width");
        v->addWidget(F::note(QStringLiteral("数値は 1080p 基準のピクセル。解像度に合わせて自動で拡大縮小されます。")));
    } else {
        v->addWidget(F::sel(w, QStringLiteral("位置"), {{"bottom", "下"}, {"center", "中央"}, {"top", "上"}}, st.strOr("align", "bottom"), [setK](const QString& s) { setK("align", OVal::str(s)); }, false));
        S2(QStringLiteral("下/上の余白"), "margin_v");
        v->addWidget(F::h4(QStringLiteral("縁取り(外側 → 内側)")));
        const OVal* outs = st.get("outlines");
        const int n = outs && outs->isArr() ? outs->size() : 0;
        for (int i = 0; i < n; ++i) {
            auto* r = new QWidget;
            r->setObjectName(QString("outline_%1").arg(i));
            auto* h = new QHBoxLayout(r);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(6);
            auto edit = [w, target, redo](int idx, const std::function<void(OVal&)>& fn) {
                w->editStyle(target, [&](OVal& s) { OVal* o = s.get("outlines"); if (o && idx < o->size()) fn(o->at(idx)); });
                redo();
            };
            h->addWidget(F::colorBtn(w, outs->at(i).strOr("color", "#000000"), [edit, i](const QString& c) { edit(i, [&](OVal& o) { o.set("color", OVal::str(c)); }); }));
            h->addWidget(F::num(w, QString("縁%1").arg(i + 1), outs->at(i).numOr("width", 0), [edit, i](double x) { edit(i, [&](OVal& o) { o.set("width", OVal::num(x)); }); }, 1, 0, 1e9, "px"), 1);
            h->addWidget(F::btn(QStringLiteral("×"), [w, target, i] { w->m_->mutate([&] { w->editStyle(target, [&](OVal& s) { OVal* o = s.get("outlines"); if (o && i < o->size()) o->erase(i); }); }); }));
            v->addWidget(r);
        }
        v->addWidget(F::btnRow({F::btn(QStringLiteral("+ 縁を追加"), [w, target] {
            w->m_->mutate([&] {
                w->editStyle(target, [&](OVal& s) {
                    OVal* o = s.get("outlines");
                    if (!o || !o->isArr()) { s.set("outlines", OVal::array()); o = s.get("outlines"); }
                    OVal e = OVal::object();
                    e.set("color", OVal::str("#000000"));
                    e.set("width", OVal::num(3));
                    o->push(e);
                });
            });
        })}));
    }
    const OVal* styles = m_->d().get("styles");
    if (styles && styles->isObj() && styles->get(target))
        v->addWidget(F::btnRow({F::btn(target == "telop" || topic ? QStringLiteral("初期設定に戻す") : QStringLiteral("このスタイルを削除"), [w, target] {
            w->m_->mutate([&] { if (OVal* s = w->m_->d().get("styles")) s->remove(target); });
        }, true)}));
}

// ---- 右: 設定 ----
void EditorWindow::settingsTab(QVBoxLayout* v) {
    using F = PanelForm;
    auto* hd = new QLabel(QStringLiteral("<b>設定</b>"));
    hd->setTextFormat(Qt::RichText);
    v->addWidget(hd);
    // 使えるメモリ: 差し込み画像のテクスチャと字幕の絵を覚えておく量
    auto* mem = new QComboBox;
    mem->setFocusPolicy(Qt::NoFocus);
    const int cur = prefGet("vedit.memMB", "2048").toInt();
    for (int mb : {512, 1024, 2048, 4096, 8192, 16384}) {
        mem->addItem(mb < 1024 ? QString("%1 MB").arg(mb) : QString("%1 GB").arg(mb / 1024), mb);
        if (mb == cur) mem->setCurrentIndex(mem->count() - 1);
    }
    QObject::connect(mem, &QComboBox::currentIndexChanged, mem, [mem](int i) {
        const int mb = mem->itemData(i).toInt();
        prefSet("vedit.memMB", QString::number(mb));
        vn::setMemBudgetMB(mb);
    });
    v->addWidget(F::rowOf(QStringLiteral("使えるメモリ"), mem));
    PROCESS_MEMORY_COUNTERS pmc{};
    MEMORYSTATUSEX ms{}; ms.dwLength = sizeof(ms);
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    GlobalMemoryStatusEx(&ms);
    v->addWidget(F::note(QStringLiteral("差し込み画像と字幕の絵を覚えておく量。多いほど同じ所を見直す時に速く、少ないほど PC が軽くなります。今の使用量 %1 MB / PC の空き %2 MB")
                             .arg(pmc.WorkingSetSize >> 20).arg(ms.ullAvailPhys >> 20)));
    auto* stop = new QCheckBox;
    stop->setChecked(prefGet("vedit.stopOnTrim", "1") != "0");
    stop->setFocusPolicy(Qt::NoFocus);
    QObject::connect(stop, &QCheckBox::toggled, stop, [](bool on) { prefSet("vedit.stopOnTrim", on ? "1" : "0"); });
    v->addWidget(F::rowOf(QStringLiteral("Q・W・シークで再生を止める"), stop));
    // 写しを残す間隔: 自動保存はしないので、戻れる対策として今の内容(未保存の分も)を別のファイルに残す
    auto* bk = new QComboBox;
    bk->setFocusPolicy(Qt::NoFocus);
    const int bcur = prefGet("vedit.backupMin", "5").toInt();
    for (int min : {0, 1, 3, 5, 10, 15, 30, 60}) {
        bk->addItem(min == 0 ? QStringLiteral("残さない") : QStringLiteral("%1 分ごと").arg(min), min);
        if (min == bcur) bk->setCurrentIndex(bk->count() - 1);
    }
    QObject::connect(bk, &QComboBox::currentIndexChanged, bk, [this, bk](int i) {
        const int min = bk->itemData(i).toInt();
        prefSet("vedit.backupMin", QString::number(min));
        m_->setBackupMinutes(min);
    });
    v->addWidget(F::rowOf(QStringLiteral("写しを残す間隔"), bk));
    v->addWidget(F::note(QStringLiteral("保存は Ctrl+S。写しはプロジェクトの隣の「(名前).backup」フォルダに新しい 30 個まで残ります。戻す時は写しをプロジェクトと同じフォルダへコピーして開きます。")));
}

// ---- 右: ヒストリー ----
void EditorWindow::historyTab(QVBoxLayout* v) {
    using F = PanelForm;
    const auto rows = m_->histRows();
    const int c = m_->undoCount(), total = c + m_->redoCount();
    auto* hd = new QLabel(QString("<b>ヒストリー</b> <span style='color:#8d8d99'> %1 / %2(最大200)</span>").arg(c).arg(total));
    hd->setTextFormat(Qt::RichText);
    v->addWidget(hd);
    auto* ls = new QListWidget;
    ls->setObjectName("hist");
    ls->setMinimumHeight(300);
    ls->setFocusPolicy(Qt::NoFocus);
    int curRow = -1;
    for (size_t n = 0; n < rows.size(); ++n) {
        const auto& r = rows[n];
        auto* it = new QListWidgetItem((n ? QString::number(n) + "   " : QStringLiteral("    ")) + r.label);
        it->setData(Qt::UserRole, r.idx);
        it->setToolTip(r.fut ? QStringLiteral("クリックでここまでやり直す") : QStringLiteral("クリックでここまで戻す"));
        if (r.fut) it->setForeground(QColor("#6b6b77"));
        ls->addItem(it);
        if (r.cur) curRow = (int)n;
    }
    if (curRow >= 0) { ls->setCurrentRow(curRow); ls->scrollToItem(ls->item(curRow)); }
    connect(ls, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        const int idx = it->data(Qt::UserRole).toInt();
        QTimer::singleShot(0, this, [this, idx] { m_->histJump(idx); });
    });
    v->addWidget(ls);
    v->addWidget(F::note(QStringLiteral("クリックでその時点に戻ります(灰色はやり直せる操作。ここから編集すると消えます)。Ctrl+Z / Ctrl+Y と同じ控えです。外部(AI・CLI)の変更を取り込むとここから始まります。")));
}

// ---- 左: 素材 ----
void EditorWindow::mediaTab(QVBoxLayout* v) {
    using F = PanelForm;
    EditorWindow* w = this;
    OVal& p = m_->d();
    v->addWidget(F::h4(QStringLiteral("素材")));
    auto* src = new QLabel(p.strOr("source", ""));
    src->setWordWrap(true);
    src->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->addWidget(src);
    const vn::MediaProps mp = vn::probeMedia(m_->resolve(p.strOr("source", "")));
    v->addWidget(F::note(mp.ok ? QString("%1×%2 / %3fps / %4").arg(mp.w).arg(mp.h).arg(mp.fps, 0, 'f', 2).arg(fmtT(mp.duration, false)) : QStringLiteral("見つかりません")));
    v->addWidget(F::h4(QStringLiteral("カットのつなぎ目")));
    const OVal* cf = p.get("cut_fx");
    const double afm = cf && cf->isObj() ? cf->numOr("audio_fade_ms", 8) : 8;
    v->addWidget(F::sel(w, QStringLiteral("音"), {{"0", "そのまま"}, {"8", "8ms フェード(推奨)"}, {"20", "20ms フェード"}}, fmtNumber(afm), [w](const QString& s) {
        F::setN(F::ensureObj(w->m_->d(), "cut_fx"), "audio_fade_ms", s.toDouble());
    }, false));
    const QString vfx = cf && cf->isObj() && cf->get("video") && cf->get("video")->asBool() ? cf->strOr("video", "none") : QStringLiteral("none");
    v->addWidget(F::sel(w, QStringLiteral("映像"), {{"none", "なし"}, {"dip-black", "暗転"}, {"dip-white", "白飛び"}}, vfx, [w](const QString& s) {
        F::ensureObj(w->m_->d(), "cut_fx").set("video", OVal::str(s));
    }, false));
    v->addWidget(F::note(QStringLiteral("音のブツ切れ防止は常に入れておくのがおすすめ。映像のつなぎは尺を変えません。")));
    v->addWidget(F::h4(QStringLiteral("BGM")));
    const OVal* bgm = p.get("bgm");
    if (!bgm || !bgm->isObj()) {
        v->addWidget(F::btnRow({F::btn(QStringLiteral("BGMを選ぶ…"), [this] {
            const QString f = pickPath("audio");
            if (f.isEmpty()) return;
            m_->mutate([&] {
                OVal b = OVal::object();
                b.set("file", OVal::str(m_->relPath(f)));
                b.set("volume_db", OVal::num(-23.5));
                b.set("duck", OVal::boolean(false));
                m_->d().set("bgm", b);
            });
        })}));
    } else {
        v->addWidget(F::note(bgm->strOr("file", "")));
        v->addWidget(F::num(w, QStringLiteral("音量"), bgm->numOr("volume_db", 0), [w](double x) { if (OVal* b = w->m_->d().get("bgm")) F::setN(*b, "volume_db", x); }, 0.5, -1e9, 1e9, "dB"));
        v->addWidget(F::check(w, QStringLiteral("声の間は下げる"), bgm->get("duck") && bgm->get("duck")->asBool(), [w](bool on) { if (OVal* b = w->m_->d().get("bgm")) b->set("duck", OVal::boolean(on)); }));
        v->addWidget(F::btnRow({F::btn(QStringLiteral("BGMを外す"), [this] { m_->mutate([this] { m_->d().set("bgm", OVal::null()); }); }, true)}));
    }
    v->addWidget(F::h4(QStringLiteral("書き出し")));
    const OVal* out = p.get("output");
    v->addWidget(F::text(w, QStringLiteral("ファイル"), out && out->isObj() ? out->strOr("file", "") : QString(), [w](const QString& s) { F::ensureObj(w->m_->d(), "output").set("file", OVal::str(s)); }, false));
}

// ---- 左: 音声 ----
void EditorWindow::audioTab(QVBoxLayout* v) {
    using F = PanelForm;
    EditorWindow* w = this;
    OVal* chain = F::chain(this);
    auto* run = F::btn(QStringLiteral("音声処理を実行"), [this] { runAudio(); }, false, true);
    run->setObjectName("b-audio");
    run->setEnabled(!(audioJob_ && audioJob_->running()));
    v->addWidget(F::btnRow({run}));
    const bool stale = audioStale();
    auto* an = F::note(audioNoteText(), stale);
    an->setObjectName("audio-note");
    v->addWidget(an);
    v->addWidget(F::h4(QStringLiteral("音声処理(上から順に実行)")));
    if (!chain) return;
    static const QHash<QString, QString> stepName = {{"declick", "De-click (リップノイズ除去)"}, {"gain", "ゲイン"}, {"normalize", "ノーマライズ"}, {"vst3", "VST3"}, {"plugin", "内蔵エフェクト"}};
    for (int n = 0; n < chain->size(); ++n) {
        const OVal& st = chain->at(n);
        const QString type = st.strOr("type", "");
        auto* box = new QFrame;
        box->setProperty("class", "step");
        box->setObjectName(QString("step_%1").arg(n));
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(8, 6, 8, 8);
        bl->setSpacing(4);
        auto* hd = new QWidget;
        auto* hh = new QHBoxLayout(hd);
        hh->setContentsMargins(0, 0, 0, 0);
        hh->setSpacing(4);
        const FxDef* fx = type == "plugin" ? fxFind(st.strOr("name", "")) : nullptr;
        auto* title = new QLabel(type == "plugin" ? (fx ? QString::fromUtf8(fx->title) : st.strOr("name", "")) : stepName.value(type, type));
        QFont tf = title->font();
        tf.setBold(true);
        title->setFont(tf);
        hh->addWidget(title, 1);
        const bool fixed = type == "declick" || type == "normalize";
        if (fixed) {
            auto* nl = F::note(type == "declick" ? QStringLiteral("常に最初") : QStringLiteral("常に最後"));
            nl->setToolTip(QStringLiteral("処理の順番は固定です"));
            hh->addWidget(nl);
        } else {
            auto* up = F::btn(QStringLiteral("↑"), [this, n] { m_->mutate([&] { OVal* c = PanelForm::chain(this); if (c && n > 0 && n < c->size()) std::swap(c->at(n - 1), c->at(n)); }); });
            up->setToolTip(QStringLiteral("上へ"));
            auto* dn = F::btn(QStringLiteral("↓"), [this, n] { m_->mutate([&] { OVal* c = PanelForm::chain(this); if (c && n >= 0 && n + 1 < c->size()) std::swap(c->at(n), c->at(n + 1)); }); });
            dn->setToolTip(QStringLiteral("下へ"));
            up->setObjectName(QString("up_%1").arg(n));
            dn->setObjectName(QString("down_%1").arg(n));
            hh->addWidget(up);
            hh->addWidget(dn);
        }
        auto* rm = F::btn(QStringLiteral("×"), [this, n] { m_->mutate([&] { OVal* c = PanelForm::chain(this); if (c && n < c->size()) c->erase(n); }); });
        rm->setObjectName(QString("rm_%1").arg(n));
        hh->addWidget(rm);
        bl->addWidget(hd);
        auto stp = [w, n]() -> OVal* { OVal* c = F::chain(w); return c && n < c->size() ? &c->at(n) : nullptr; };
        if (type == "declick") {
            const OVal* pr = st.get("params");
            bl->addWidget(F::num(w, QStringLiteral("しきい値"), pr && pr->isObj() ? pr->numOr("threshold", 8) : 8, [stp](double x) { if (auto* s = stp()) F::setN(F::ensureObj(*s, "params"), "threshold", x); }, 0.5, 2, 30));
            bl->addWidget(F::note(QStringLiteral("小さいほど強く直す(雑音を多く拾う)。標準 8")));
        }
        if (type == "gain") bl->addWidget(F::num(w, QStringLiteral("ゲイン"), st.numOr("db", 0), [stp](double x) { if (auto* s = stp()) F::setN(*s, "db", x); }, 0.5, -1e9, 1e9, "dB"));
        if (type == "normalize") {
            const bool lufs = st.get("lufs") && !st.get("lufs")->isNull();
            bl->addWidget(F::sel(w, QStringLiteral("基準"), {{"lufs", "ラウドネス (LUFS)"}, {"peak", "ピーク"}}, lufs ? "lufs" : "peak", [stp](const QString& s) {
                auto* o = stp();
                if (!o) return;
                if (s == "lufs") { F::setN(*o, "lufs", -14); F::setN(*o, "peak_db", -1); }
                else { o->remove("lufs"); F::setN(*o, "peak_db", -3); }
            }));
            if (lufs) {
                bl->addWidget(F::num(w, QStringLiteral("目標"), st.numOr("lufs", -14), [stp](double x) { if (auto* o = stp()) F::setN(*o, "lufs", x); }, 0.5, -1e9, 1e9, "LUFS"));
                bl->addWidget(F::num(w, QStringLiteral("上限ピーク"), st.numOr("peak_db", -1), [stp](double x) { if (auto* o = stp()) F::setN(*o, "peak_db", x); }, 0.5, -1e9, 1e9, "dB"));
                bl->addWidget(F::note(QStringLiteral("YouTube等は -14 / 放送は -24。上限ピークを超える分はゲインを抑えます")));
            } else {
                bl->addWidget(F::num(w, QStringLiteral("ピーク"), st.numOr("peak_db", -3), [stp](double x) { if (auto* o = stp()) F::setN(*o, "peak_db", x); }, 0.5, -1e9, 1e9, "dB"));
            }
        }
        if (type == "vst3") bl->addWidget(F::note(st.strOr("path", "")));
        if (type == "plugin") {
            if (!fx) bl->addWidget(F::note(st.strOr("name", "")));
            else {
                const OVal* pr = st.get("params");
                for (const FxParam& q : fx->params) {
                    const QString key = q.key;
                    bl->addWidget(F::num(w, QString::fromUtf8(q.label), pr && pr->isObj() ? pr->numOr(key, q.def) : q.def,
                                         [stp, key](double x) { if (auto* s = stp()) F::setN(F::ensureObj(*s, "params"), key.toUtf8().constData(), x); }, q.step, -1e9, 1e9, QString::fromUtf8(q.unit)));
                }
                if (*fx->note) bl->addWidget(F::note(QString::fromUtf8(fx->note)));
            }
        }
        v->addWidget(box);
    }
    bool hasNorm = false;
    for (const OVal& s : chain->items()) if (s.strOr("type", "") == "normalize") hasNorm = true;
    auto addB = [&](const QString& label, const QString& name, std::function<OVal()> mk) {
        auto* b = F::btn(label, [w, mk] { F::chainInsert(w, mk()); });
        b->setObjectName(name);
        return b;
    };
    auto declick = [] { OVal s = OVal::object(); s.set("type", OVal::str("declick")); OVal p = OVal::object(); p.set("threshold", OVal::num(8)); s.set("params", p); return s; };
    auto gain = [] { OVal s = OVal::object(); s.set("type", OVal::str("gain")); s.set("db", OVal::num(6)); return s; };
    auto norm = [] { OVal s = OVal::object(); s.set("type", OVal::str("normalize")); s.set("lufs", OVal::num(-14)); s.set("peak_db", OVal::num(-1)); return s; };
    auto* vst = F::btn(QStringLiteral("+ VST3…"), [this] {
        const QString f = pickPath("vst3");
        if (f.isEmpty()) return;
        OVal s = OVal::object();
        s.set("type", OVal::str("vst3"));
        s.set("path", OVal::str(f));
        s.set("params", OVal::object());
        PanelForm::chainInsert(this, s);
    });
    vst->setObjectName("add_vst3");
    v->addWidget(F::btnRow({addB(QStringLiteral("+ De-click"), "add_declick", declick), addB(QStringLiteral("+ ゲイン"), "add_gain", gain), vst,
                            hasNorm ? nullptr : addB(QStringLiteral("+ ノーマライズ"), "add_norm", norm)}));
    auto* fxs = new QComboBox;
    fxs->setObjectName("add_fx");
    fxs->setFocusPolicy(Qt::StrongFocus);
    fxs->addItem(QStringLiteral("+ エフェクトを追加…"), QString());
    for (const FxDef& f : fxTable()) fxs->addItem(QString::fromUtf8(f.title), QString::fromUtf8(f.name));
    connect(fxs, &QComboBox::activated, this, [this, fxs](int i) {
        const QString n = fxs->itemData(i).toString();
        fxs->setCurrentIndex(0);
        if (n.isEmpty()) return;
        if (const FxDef* f = fxFind(n)) QTimer::singleShot(0, this, [this, f] { PanelForm::chainInsert(this, PanelForm::fxStep(*f)); });
    });
    v->addWidget(F::rowOf(QString(), fxs));
    auto* rec = F::btn(QStringLiteral("声のおすすめ設定"), [this] {
        m_->mutate([this] {
            OVal* c = PanelForm::chain(this);
            if (!c) return;
            OVal nc = OVal::array();
            for (const OVal& s : c->items()) { const QString t = s.strOr("type", ""); if (t == "declick" || t == "vst3") nc.push(s); }
            for (const char* n : {"HighpassFilter", "NoiseGate", "Compressor", "Limiter"}) nc.push(PanelForm::fxStep(*fxFind(n)));
            OVal nz = OVal::object();
            nz.set("type", OVal::str("normalize"));
            nz.set("lufs", OVal::num(-14));
            nz.set("peak_db", OVal::num(-1));
            nc.push(nz);
            *c = nc;
        });
    });
    rec->setToolTip(QStringLiteral("ローカット→ゲート→コンプ→リミッター→-14LUFS の声向けセット"));
    rec->setObjectName("rec_voice");
    v->addWidget(F::btnRow({rec}));
}

// ---- 左: カット ----
void EditorWindow::cutTab(QVBoxLayout* v) {
    using F = PanelForm;
    EditorWindow* w = this;
    OVal& p = m_->d();
    v->addWidget(F::h4(QStringLiteral("無音カット")));
    // 設定は画面の状態(プロジェクトには保存しない)。入力しても編集履歴は作らない
    auto sn = [w](const QString& label, double val, std::function<void(double)> set, double step, const QString& unit) {
        auto* sp = new TrimSpin;
        sp->setDecimals(4);
        sp->setRange(-1e9, 1e9);
        sp->setSingleStep(step);
        sp->setValue(val);
        sp->setFixedWidth(84);
        sp->setObjectName("sil_" + label);
        QObject::connect(sp, &QDoubleSpinBox::valueChanged, sp, [set](double x) { set(x); });
        Q_UNUSED(w);
        return F::rowOf(label, sp, unit);
    };
    v->addWidget(sn(QStringLiteral("無音の長さ"), sil_.min, [w](double x) { w->sil_.min = x; }, 0.1, QStringLiteral("秒以上")));
    v->addWidget(sn(QStringLiteral("無音の基準"), sil_.noise, [w](double x) { w->sil_.noise = x; }, 1, QStringLiteral("dB以下")));
    v->addWidget(sn(QStringLiteral("頭に残す"), sil_.kb, [w](double x) { w->sil_.kb = x; }, 0.05, QStringLiteral("秒")));
    v->addWidget(sn(QStringLiteral("後ろに残す"), sil_.ka, [w](double x) { w->sil_.ka = x; }, 0.05, QStringLiteral("秒")));
    auto* sb = F::btn(silBusy_ ? QStringLiteral("検出中…") : QStringLiteral("無音を検出"), [this] { runSilence(); });
    sb->setObjectName("b-sil");
    sb->setEnabled(!silBusy_);
    v->addWidget(F::btnRow({sb}));
    if (silHave_) {
        const QJsonArray cuts = silRes_.value("cuts").toArray();
        double rem = 0;
        for (const QJsonValue& c : cuts) rem += c.toArray().at(1).toDouble() - c.toArray().at(0).toDouble();
        auto* sn2 = F::note(QStringLiteral("%1か所 / 約%2秒を削除").arg(cuts.size()).arg(rem, 0, 'f', 1));
        sn2->setObjectName("sil-note");
        v->addWidget(sn2);
        auto* add = F::btn(QStringLiteral("カットに追加"), [this] {
            const QJsonArray cs = silRes_.value("cuts").toArray();
            silHave_ = false;
            silRes_ = QJsonObject();
            m_->mutate([&] {
                std::vector<std::pair<double, double>> all;
                if (const OVal* cu = m_->d().get("cuts")) for (const OVal& c : cu->items()) if (c.size() >= 2) all.push_back({c.at(0).asNum(), c.at(1).asNum()});
                for (const QJsonValue& c : cs) all.push_back({c.toArray().at(0).toDouble(), c.toArray().at(1).toDouble()});
                std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                std::vector<std::pair<double, double>> mg;
                for (const auto& c : all) {
                    if (!mg.empty() && c.first <= mg.back().second + 1e-3) mg.back().second = std::max(mg.back().second, c.second);
                    else mg.push_back(c);
                }
                OVal na = OVal::array();
                for (const auto& c : mg) { OVal pr = OVal::array(); pr.push(OVal::num(c.first)); pr.push(OVal::num(c.second)); na.push(pr); }
                m_->d().set("cuts", na);
            });
        }, false, true);
        add->setObjectName("sil-add");
        auto* drop = F::btn(QStringLiteral("破棄"), [this] { silHave_ = false; silRes_ = QJsonObject(); fillLeft(); });
        drop->setObjectName("sil-drop");
        v->addWidget(F::btnRow({add, drop}));
    }
    const OVal* cu = p.get("cuts");
    const int nc = cu && cu->isArr() ? cu->size() : 0;
    v->addWidget(F::h4(QStringLiteral("カット一覧 (%1)").arg(nc)));
    double removed = 0;
    for (const auto& c : m_->mergedCuts()) removed += c.second - c.first;
    v->addWidget(F::note(QStringLiteral("合計 %1秒 削除 → 仕上がり %2").arg(removed, 0, 'f', 1).arg(fmtT(m_->totalOut(), false))));
    auto* ls = new QListWidget;
    ls->setObjectName("cutlist");
    ls->setMaximumHeight(260);
    ls->setMinimumHeight(std::min(260, 24 * std::max(1, std::min(nc, 10)) + 8));
    ls->setFocusPolicy(Qt::NoFocus);
    std::vector<std::pair<int, std::pair<double, double>>> idx;
    for (int i = 0; i < nc; ++i) if (cu->at(i).size() >= 2) idx.push_back({i, {cu->at(i).at(0).asNum(), cu->at(i).at(1).asNum()}});
    std::stable_sort(idx.begin(), idx.end(), [](const auto& a, const auto& b) { return a.second.first < b.second.first; });
    for (size_t k = 0; k < idx.size() && k < 300; ++k) {
        auto* it = new QListWidgetItem(QStringLiteral("%1 – %2   (%3s)").arg(fmtT(idx[k].second.first), fmtT(idx[k].second.second)).arg(idx[k].second.second - idx[k].second.first, 0, 'f', 2));
        it->setData(Qt::UserRole, idx[k].first);
        it->setData(Qt::UserRole + 1, idx[k].second.first);
        ls->addItem(it);
    }
    connect(ls, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) {
        m_->select("cuts", it->data(Qt::UserRole).toInt());
        seekTo(it->data(Qt::UserRole + 1).toDouble());
    });
    v->addWidget(ls);
    if (nc) {
        auto* all = F::btn(QStringLiteral("すべて取り消す"), [this] { if (askYes(QStringLiteral("カットをすべて取り消しますか?"))) m_->mutate([this] { m_->d().set("cuts", OVal::array()); }); }, true);
        all->setObjectName("cuts-clear");
        v->addWidget(F::btnRow({all}));
    }
}

}  // namespace vui
