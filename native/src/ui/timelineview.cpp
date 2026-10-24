#include "timelineview.h"
#include <QFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTimer>
#include <QMap>
#include <algorithm>
#include <cmath>
#include <map>

namespace vui {

static const QColor kBg("#1b1b1f"), kPanel("#242429"), kPanel2("#2d2d34"), kLine("#3a3a43"), kDim("#8d8d99"), kRed("#e5484d");

static QFont uiFont(int px, bool bold = false) {
    QFont f("Yu Gothic UI");
    f.setStyleHint(QFont::SansSerif);
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

static QString baseName(const QString& p) { return p.mid(std::max(p.lastIndexOf('/'), p.lastIndexOf('\\')) + 1); }

TimelineView::TimelineView(EditModel* m, QWidget* parent) : QWidget(parent), m_(m) {
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    hbar_ = new QScrollBar(Qt::Horizontal, this);
    hbar_->setFocusPolicy(Qt::NoFocus);
    hbar_->setStyleSheet(
        "QScrollBar:horizontal{background:#1b1b1f;height:15px;margin:0}"
        "QScrollBar::handle:horizontal{background:#4a4a55;min-width:30px;border-radius:0}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:#1b1b1f}");
    connect(hbar_, &QScrollBar::valueChanged, this, [this] { update(); });
    seekSnap_ = prefGet("vedit.seeksnap", "0") == "1";
    connect(m_, &EditModel::changed, this, [this] { updateBar(); clampV(); update(); });
    connect(m_, &EditModel::selectionChanged, this, [this] { update(); });
    connect(m_, &EditModel::playheadChanged, this, [this] { updatePlayhead(); });
    setAttribute(Qt::WA_OpaquePaintEvent);   // 全面を自分で塗る(地を塗る手間を省く)
    setMinimumHeight(120);
}

int TimelineView::contentWidth() const { return std::max(1, width() - kLabelW); }

void TimelineView::resizeEvent(QResizeEvent*) {
    hbar_->setGeometry(kLabelW, height() - 15, std::max(1, width() - kLabelW), 15);
    updateBar();
    clampV();
    emit vChanged();
}

void TimelineView::updateBar() {
    const double tl = (m_->collapsed ? m_->totalOut() : m_->dur()) * zoom_ + 400;
    const int page = contentWidth();
    hbar_->setRange(0, std::max(0, (int)tl - page));
    hbar_->setPageStep(page);
    hbar_->setSingleStep(40);
}

// 描画中(pin_)は build() で作った lanes_ を使う。lanes() は毎回オーバーレイを全部数えるので、テロップが数百あると描画1回が百数十ms になっていた
std::vector<LaneDef> TimelineView::lanesNow() const { return pin_ ? lanes_ : m_->lanes(); }
namespace { struct LanePin { int& n; explicit LanePin(int& x) : n(x) { ++n; } ~LanePin() { --n; } }; }

// 描画中(pin_)は lanes_ をそのまま見る。lanesNow() は一覧の写しを返すので、1本ごとに呼ぶと分割線の数×トラック数の写しになる
int TimelineView::laneHeight(const QString& id) const {
    auto find = [&](const std::vector<LaneDef>& ls) { for (auto& l : ls) if (l.id == id) return (int)std::lround(l.h * vz_ * lz(id)); return 34; };
    return pin_ ? find(lanes_) : find(m_->lanes());
}
int TimelineView::laneTop(const QString& id) const {
    auto top = [&](const std::vector<LaneDef>& ls) {
        int y = kRuler;
        for (auto& l : ls) { if (l.id == id) return y; y += (int)std::lround(l.h * vz_ * lz(l.id)); }
        return y;
    };
    return pin_ ? top(lanes_) : top(m_->lanes());
}
int TimelineView::fullHeight() const {
    auto full = [&](const std::vector<LaneDef>& ls) { int y = kRuler; for (auto& l : ls) y += (int)std::lround(l.h * vz_ * lz(l.id)); return y; };
    return pin_ ? full(lanes_) : full(m_->lanes());
}
int TimelineView::viewHeight() const { return height() - 15; }
int TimelineView::vMax() const { return std::max(0, fullHeight() - viewHeight()); }
void TimelineView::clampV() { vOff_ = std::min(vMax(), std::max(0, vOff_)); }
void TimelineView::setVOff(int v) { const int o = vOff_; vOff_ = v; clampV(); if (o != vOff_) { update(); emit vChanged(); } }
int TimelineView::hScroll() const { return hbar_->value(); }
void TimelineView::setHScroll(int v) { hbar_->setValue(v); }
void TimelineView::setSeekSnap(bool v) { seekSnap_ = v; prefSet("vedit.seeksnap", v ? "1" : "0"); update(); }
QPoint TimelineView::screenOf(double srcT, const QString& lane) const {
    const double x = (m_->collapsed ? m_->outTime(srcT) : srcT) * zoom_ - hbar_->value() + kLabelW;
    return QPoint((int)std::lround(x), laneTop(lane) - vOff_ + laneHeight(lane) / 2);
}

QString TimelineView::laneAtY(int y) const {
    if (y < kRuler) return "ruler";
    const int yy = y + vOff_;
    int t = kRuler;
    for (auto& l : lanesNow()) { const int h = laneHeight(l.id); if (yy >= t && yy < t + h) return l.id; t += h; }
    return QString();
}

double TimelineView::timeAtX(int x) const {
    const double o = (x - kLabelW + hbar_->value()) / zoom_;
    return m_->srcOf(std::max(0.0, o), m_->collapsed);
}

double TimelineView::playX() { return m_->collapsed ? m_->tout() * zoom_ : X(m_->tcur()); }

void TimelineView::setZoom(double z, int anchorPx) {
    z = clampd(z, 0.5, 300);
    const int ax = anchorPx >= 0 ? anchorPx : contentWidth() / 2;
    const double t = (hbar_->value() + ax) / zoom_;
    zoom_ = z;
    updateBar();
    hbar_->setValue((int)std::max(0.0, t * zoom_ - ax));
    emit zoomChanged(zoom_);
    update();
}

void TimelineView::zoomFit(int reservePx) {
    const double len = m_->collapsed ? m_->totalOut() : m_->dur();
    setZoom(clampd((contentWidth() - reservePx - 40) / std::max(0.01, len), 0.5, 300));
    hbar_->setValue(0);
}

void TimelineView::ensureVisible(double srcT) {
    const double x = std::fabs(srcT - m_->tcur()) < 1e-9 ? playX() : X(srcT), v0 = hbar_->value(), w = contentWidth();
    if (x < v0 || x > v0 + w) hbar_->setValue((int)std::max(0.0, x - 200));
}

void TimelineView::loadWave() {
    const QString key = m_->path();
    if (waveTried_ && waveFor_ == key) return;
    waveTried_ = true;
    waveFor_ = key;
    peaks_.clear();
    waveRate_ = 0;
    const QString dir = m_->dir() + "/" + m_->name() + ".cache";
    QFile f(dir + "/wave.json");
    if (!f.open(QIODevice::ReadOnly)) return;
    OVal v;
    if (!OVal::parse(f.readAll(), &v) || !v.isObj()) return;
    waveRate_ = (int)v.numOr("rate", 0);
    if (const OVal* pk = v.get("peaks"); pk && pk->isArr()) {
        peaks_.reserve(pk->size());
        for (const OVal& x : pk->items()) peaks_.push_back((unsigned char)std::min(255.0, std::max(0.0, x.asNum(0))));
    }
    waveCol_ = QFile::exists(dir + "/audio.wav") ? QColor("#6ee7b7") : QColor("#7c8aa5");
}

double TimelineView::levelAt(double srcT) {
    loadWave();
    if (peaks_.empty() || waveRate_ <= 0) return -1;
    const int b = (int)std::floor(srcT * waveRate_), a = std::max(0, b - 2);
    int mx = 0;
    for (int i = a; i <= b && i < (int)peaks_.size(); ++i) mx = std::max<int>(mx, peaks_[i]);
    return mx / 255.0;
}

void TimelineView::drawWave(QPainter& p, int top, int H) {
    loadWave();
    if (peaks_.empty() || waveRate_ <= 0) return;
    const int w = contentWidth(), sc = hbar_->value();
    const double r = waveRate_;
    p.setPen(Qt::NoPen);
    p.setBrush(waveCol_);
    for (int x = 0; x < w; ++x) {
        const double o = (sc + x) / zoom_;
        if (m_->collapsed && m_->gapPieceAt(o)) continue;
        const int a = (int)std::floor(m_->srcOf(o, m_->collapsed) * r);
        const int b = std::min(a + (int)std::ceil(r / zoom_) + 1, std::max(a + 1, (int)std::floor(m_->srcOf((sc + x + 1) / zoom_, m_->collapsed) * r)));
        int mx = 0;
        for (int i = a; i < b && i < (int)peaks_.size(); ++i) if (i >= 0 && peaks_[i] > mx) mx = peaks_[i];
        const double hh = std::max(1.0, (mx > 0 ? std::max(0.0, 1 + std::log10(mx / 255.0) * 20 / 48) : 0) * (H - 4));
        p.drawRect(QRectF(kLabelW + x, top + (H - hh) / 2, 1, hh));
    }
}

void TimelineView::build() {
    els_.clear();
    clipSplits_.clear();
    lanes_ = m_->lanes();
    LanePin pin(pin_);
    const OVal& d = m_->d();
    const QString srcName = baseName(d.strOr("source", ""));
    const bool coll = m_->collapsed;
    auto push = [&](El e) { els_.push_back(std::move(e)); };
    QHash<QString, std::pair<int, int>> laneY;   // 要素ごとに laneTop を数え直さない
    for (auto& l : lanes_) laneY.insert(l.id, {laneTop(l.id), laneHeight(l.id)});
    auto yOf = [&](const QString& lane, double* y, double* h) {
        const auto it = laneY.constFind(lane);
        if (it != laneY.constEnd()) { *y = it->first; *h = it->second; } else { *y = laneTop(lane); *h = laneHeight(lane); }
    };
    const auto segs = m_->segments();
    const bool lk = m_->linked;
    (void)lk;
    auto segOn = [&](double a, double b, QChar trk) {
        for (const SegSel& q : m_->segSel)
            if (std::fabs(q.a - a) < 1e-6 && std::fabs(q.b - b) < 1e-6 && (q.trk.isNull() || q.trk == trk)) return true;
        return false;
    };
    for (auto& s : segs) for (const QString l : {"v1", "a1"}) {
        El e; e.t = El::Seg; e.lane = l; e.a = s.first; e.b = s.second; e.trk = l == "v1" ? 'v' : 'a';
        double y, h; yOf(l, &y, &h);
        const double x0 = X(s.first), x1 = X(s.second);
        e.r = QRectF(x0, y + 3, std::max(2.0, x1 - x0), h - 1 - 6);
        e.alpha = l == "a1"; e.label = l == "v1" ? srcName : QString(); e.sel = segOn(s.first, s.second, e.trk);
        e.col = e.alpha ? QColor(59, 90, 138, 46) : QColor("#3b5a8a");
        push(e);
    }
    for (auto& g : m_->gapsN()) for (const QString l : {"v1", "a1"}) {
        El e; e.t = El::Gap; e.lane = l; e.a = g.first; e.b = g.second;
        double y, h; yOf(l, &y, &h);
        if (coll) e.r = QRectF(X(g.first) - g.second * zoom_, y + 3, std::max(2.0, g.second * zoom_), h - 7);
        else e.r = QRectF(X(g.first) - 3, y + 3, 6, h - 7);
        for (double x : m_->gapSel) if (std::fabs(x - g.first) < 1e-6) e.sel = true;
        if (l == "v1" && coll) e.label = QString("空白 %1s").arg(g.second, 0, 'f', 1);
        push(e);
    }
    auto trk = [&](const char* key, const char* lane, const QString& label) {
        const OVal* a = d.get(key);
        if (!a || !a->isArr()) return;
        for (int i = 0; i < a->size(); ++i) {
            const OVal& r = a->at(i);
            const double A = r.isArr() ? r.at(0).asNum() : r.numOr("a", 0), B = r.isArr() ? r.at(1).asNum() : r.numOr("b", 0);
            El e; e.t = El::Mute; e.lane = lane; e.kind = key; e.idx = i; e.a = A; e.b = B; e.label = label;
            double y, h; yOf(lane, &y, &h);
            e.r = QRectF(X(A), y + 3, std::max(2.0, X(B) - X(A)), h - 7);
            e.sel = m_->trkSel.contains(QString(key) + ":" + QString::number(i));
            push(e);
        }
    };
    trk("vmute", "v1", "黒");
    trk("amute", "a1", "無音");
    if (const OVal* a = d.get("aslip"); a && a->isArr())
        for (int i = 0; i < a->size(); ++i) {
            const OVal& sl = a->at(i);
            const double A = sl.numOr("a", 0), B = sl.numOr("b", 0), dd = sl.numOr("d", 0);
            El g; g.t = El::Mute; g.ghost = true; g.lane = "a1"; g.a = A; g.b = B; g.label = "無音(移動元)";
            double y, h; yOf("a1", &y, &h);
            g.r = QRectF(X(A), y + 3, std::max(2.0, X(B) - X(A)), h - 7);
            push(g);
            El e; e.t = El::Slip; e.lane = "a1"; e.kind = "aslip"; e.idx = i; e.a = A; e.b = B;
            e.label = QString("♪ %1%2s").arg(dd >= 0 ? "+" : "").arg(dd, 0, 'f', 2);
            const double x0 = coll ? (m_->outTime(A) + dd) * zoom_ : X(A) + dd * zoom_;
            e.r = QRectF(x0, y + 3, std::max(2.0, (B - A) * zoom_), h - 7);
            e.sel = m_->trkSel.contains("aslip:" + QString::number(i));
            push(e);
        }
    static const QMap<QString, QString> colors = {{"telops", "#2b6cb0"}, {"topics", "#b83243"}, {"overlays", "#2f855a"}, {"sfx", "#b7791f"}};
    for (const char* kind : {"overlays", "topics", "telops", "sfx"}) {
        const OVal* a = d.get(kind);
        if (!a) continue;
        for (int i = 0; i < a->size(); ++i) {
            const OVal& it = a->at(i);
            El e; e.t = El::Clip; e.kind = kind; e.idx = i; e.lane = m_->laneOf(kind, it);
            QString kk = kind;
            e.col = colors.value(kk);
            if (kk == "sfx" && it.strOr("kind", "") == "bgm") e.col = QColor("#6b46c1");
            if (kk == "overlays") e.label = !it.strOr("file", "").isEmpty() ? baseName(it.strOr("file", "")) : "✦ " + it.strOr("kind", "?");
            else if (kk == "sfx") e.label = "♪ " + baseName(it.strOr("file", "")) + " " + fmtNumber(it.numOr("gain_db", 0)) + "dB";
            else e.label = it.strOr("text", "").replace('\n', ' ');
            double x0, x1;
            m_->itRange(it, coll, &x0, &x1);
            double y, h; yOf(e.lane, &y, &h);
            if (EditModel::isGapIt(it)) {
                e.ingap = true;
                if (x0 == x1) { x0 = x0 * zoom_ - 3; x1 = x0 + 6; } else { x0 *= zoom_; x1 *= zoom_; }
            } else {
                x0 = X(it.numOr("start", 0));
                x1 = coll ? m_->outEnd(it.numOr("end", 0)) * zoom_ : X(it.numOr("end", 0));
            }
            e.r = QRectF(x0, y + 3, std::max(4.0, x1 - x0), h - 7);
            e.sel = m_->selSet.contains(QString(kind) + ":" + QString::number(i));
            push(e);
        }
    }
    {
        const OVal* a = d.get("cuts");
        for (int i = 0; a && i < a->size(); ++i) {
            const double A = std::min(a->at(i).at(0).asNum(), a->at(i).at(1).asNum()), B = std::max(a->at(i).at(0).asNum(), a->at(i).at(1).asNum());
            for (const QString l : {"v1", "a1"}) {
                El e; e.t = El::Cut; e.lane = l; e.kind = "cuts"; e.idx = i; e.a = A; e.b = B;
                double y, h; yOf(l, &y, &h);
                e.seam = coll;
                e.r = coll ? QRectF(X(A), y, 2, h - 1) : QRectF(X(A), y, std::max(2.0, X(B) - X(A)), h - 1);
                e.sel = m_->selKind == "cuts" && m_->selI == i;
                push(e);
            }
        }
    }
    if (const OVal* b = d.get("bgm"); b && b->isObj()) {
        El e; e.t = El::Bgm; e.lane = "bgm"; e.kind = "bgm"; e.col = QColor("#6b46c1");
        e.label = "♪ " + baseName(b->strOr("file", "")) + QString("  %1dB").arg(fmtNumber(b->numOr("volume_db", -23.5)));
        double y, h; yOf("bgm", &y, &h);
        e.r = QRectF(0, y + 3, m_->totalOut() * zoom_, h - 7);
        e.sel = m_->bgmSel;
        push(e);
    }
    // 分割線(V1/A1 は splits、ほかは「前の終わり = 次の頭」)
    splitsV_.clear();
    if (const OVal* sp = d.get("splits")) for (const OVal& t : sp->items()) splitsV_.push_back(X(t.asNum()));
    std::map<QString, std::vector<std::pair<double, double>>> byLane;
    for (const char* kind : {"overlays", "topics", "telops", "sfx"}) {
        const OVal* a = d.get(kind);
        if (!a) continue;
        for (const OVal& it : a->items()) if (!EditModel::isGapIt(it)) byLane[m_->laneOf(kind, it)].push_back({it.numOr("start", 0), it.numOr("end", 0)});
    }
    for (auto& kv : byLane) {
        std::vector<double> st;
        for (auto& r : kv.second) st.push_back(r.first);
        std::sort(st.begin(), st.end());
        std::vector<double> seen;
        for (auto& r : kv.second) {
            const double e = r.second;
            auto it = std::find_if(st.begin(), st.end(), [&](double s) { return s > e - 1e-3; });
            if (it != st.end() && *it < e + 1e-3 && std::find(seen.begin(), seen.end(), e) == seen.end()) { seen.push_back(e); clipSplits_.push_back({kv.first, X(e)}); }
        }
    }
}

// 呼ぶ側が area で切り抜き、アンチエイリアスを切ってから並べて呼ぶ(1個ずつ save/restore すると、全体表示で数千個のとき重い)
void TimelineView::drawClip(QPainter& p, const El& e, double sx, const QRect& area, double op) {
    QRectF r = e.r.translated(kLabelW - sx, -vOff_);
    if (r.right() < kLabelW || r.left() > width()) return;
    static const QFont small = uiFont(10), norm = uiFont(11);
    if (e.t == El::Cut) {
        if (e.seam) { p.fillRect(r, QColor(255, 59, 59, 242)); return; }
        p.fillRect(r, QColor(229, 72, 77, 100));
        p.fillRect(r, QBrush(QColor(229, 72, 77, 140), Qt::BDiagPattern));
        p.setPen(kRed);
        p.drawLine(QPointF(r.left() + .5, r.top()), QPointF(r.left() + .5, r.bottom()));
        p.drawLine(QPointF(r.right() - .5, r.top()), QPointF(r.right() - .5, r.bottom()));
        if (e.sel) { p.setPen(QPen(QColor("#ffd84d"), 3)); p.setBrush(Qt::NoBrush); p.drawRect(r.adjusted(1.5, 1.5, -1.5, -1.5)); }
        return;
    }
    QColor bg = e.col, border(255, 255, 255, 64), txt = Qt::white;
    QFont f = norm;
    int padL = 6;
    if (e.t == El::Seg) { border = QColor(255, 255, 255, 89); padL = 8; }
    if (e.t == El::Gap) { bg = QColor("#17171b"); border = QColor("#777"); txt = QColor("#aaa"); f = small; }
    if (e.t == El::Mute) { bg = QColor(20, 20, 24, 210); border = QColor("#99aaaa"); txt = QColor("#cccccc"); f = small; }
    if (e.t == El::Slip) { bg = QColor(46, 160, 110, 140); border = QColor("#6ee7b7"); f = small; }
    if (e.sel && !e.alpha) bg = bg.lighter(135);
    if (e.ghost) p.setOpacity(.8);
    if (e.sel) {   // 光彩
        for (int i = 4; i >= 1; --i) { p.setPen(Qt::NoPen); p.setBrush(QColor(255, 216, 77, 38)); p.drawRoundedRect(r.adjusted(-i * 1.5, -i * 1.5, i * 1.5, i * 1.5), 4, 4); }
    }
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, 4, 4);
    if (e.t == El::Gap || e.t == El::Mute) { p.setBrush(QBrush(QColor(255, 255, 255, 18), Qt::BDiagPattern)); p.drawRoundedRect(r, 4, 4); }
    if (e.alpha && e.sel) { p.setBrush(QColor(255, 216, 77, 56)); p.drawRoundedRect(r, 4, 4); }
    p.setBrush(Qt::NoBrush);
    QPen bp(border, 1);
    if (e.t == El::Gap || e.t == El::Mute) bp.setStyle(Qt::DashLine);
    p.setPen(bp);
    p.drawRoundedRect(r.adjusted(.5, .5, -.5, -.5), 4, 4);
    if (!e.label.isEmpty()) {
        p.setClipRect(r.adjusted(padL, 0, -padL, 0).toAlignedRect().intersected(area));
        p.setFont(f);
        p.setPen(txt);
        p.drawText(QRectF(r.left() + padL, r.top() + 2, std::max(0.0, r.width() - padL * 2), 16), Qt::AlignLeft | Qt::AlignTop, e.label);
        p.setClipRect(area);
    }
    if (e.sel) {
        p.setPen(QPen(QColor("#ffd84d"), 3));
        p.drawRoundedRect(r.adjusted(1.5, 1.5, -1.5, -1.5), 4, 4);
    }
    if (e.ghost) p.setOpacity(op);
}


void TimelineView::drawRuler(QPainter& p) {
    const int w = contentWidth();
    const double sc = hbar_->value();
    p.fillRect(QRect(kLabelW, 0, w, kRuler), kPanel2);
    const double t0 = sc / zoom_;
    static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 1200};
    double step = 1800;
    for (double s : steps) if (s * zoom_ >= 80) { step = s; break; }
    p.setFont(uiFont(11));
    p.setPen(QColor("#555555"));
    for (double t = std::floor(t0 / step) * step; t < (sc + w) / zoom_; t += step) {
        const double x = std::round(t * zoom_ - sc) + .5 + kLabelW;
        p.setPen(QColor("#555555"));
        p.drawLine(QPointF(x, 20), QPointF(x, kRuler));
        p.setPen(kDim);
        p.drawText(QPointF(x + 3, 15), fmtT(t, step < 1));
    }
}

// 左のトラック名。見た目を決めるもの(高さ・縦位置・各トラックの状態)が前と同じなら、ためた絵を貼る
void TimelineView::drawLabels(QPainter& p) {
    const qreal dpr = p.device()->devicePixelRatioF();
    QString sig = QString("%1|%2|%3|%4|%5|").arg(height()).arg(vOff_).arg(dpr).arg(seekSnap_).arg(m_->vTarget);
    for (auto& l : lanes_) {
        const bool audio = EditModel::isAudioLane(l.id);
        sig += l.id + "," + l.name + "," + QString::number(laneHeight(l.id)) + "," + QString::number(l.vn) + ","
             + (m_->isLocked(l.id) ? "L" : "") + ((audio ? m_->isMuted(l.id) : m_->isHidden(l.id)) ? "O" : "") + ";";
    }
    const QSize px = (QSizeF(kLabelW, height()) * dpr).toSize();
    if (sig != lblSig_ || lblPix_.size() != px) {
        lblPix_ = QPixmap(px);
        lblPix_.setDevicePixelRatio(dpr);
        QPainter lp(&lblPix_);
        lp.setRenderHints(p.renderHints());
        drawLabelsRaw(lp);
        lblCache_ = lbl_;
        lblSig_ = sig;
    } else lbl_ = lblCache_;
    p.drawPixmap(0, 0, lblPix_);
}

void TimelineView::drawLabelsRaw(QPainter& p) {
    lbl_.clear();
    p.fillRect(QRect(0, 0, kLabelW, height()), kPanel);
    p.setPen(kLine);
    p.drawLine(kLabelW - 1, 0, kLabelW - 1, height());
    p.save();
    p.setClipRect(QRect(0, kRuler, kLabelW, height() - kRuler - 15));
    int y = kRuler - vOff_;
    for (auto& l0 : lanes_) {
        LaneDef l = l0;
        l.h = laneHeight(l0.id);
        const bool vt = l.vn && l.vn == m_->vTarget;
        const QRect row(0, y, kLabelW - 1, l.h);
        if (vt) { p.fillRect(QRect(0, y, kLabelW - 1, l.h - 1), QColor(74, 144, 226, 30)); p.fillRect(QRect(0, y, 3, l.h - 1), QColor("#4a90e2")); }
        p.setPen(kLine);
        p.drawLine(0, y + l.h - 1, kLabelW - 2, y + l.h - 1);
        int x = 2;
        const bool locked = m_->isLocked(l.id), audio = EditModel::isAudioLane(l.id), off = audio ? m_->isMuted(l.id) : m_->isHidden(l.id);
        if (l.vn) { p.setFont(uiFont(13)); p.setPen(QColor("#8a93a3")); p.drawText(QRect(x, y, 10, l.h - 1), Qt::AlignCenter, QString::fromUtf8("⠿")); x += 12; }
        const QRect lk(x, y + (l.h - 1 - 16) / 2, 16, 16);
        p.setFont(uiFont(11));
        p.setOpacity(locked ? 1 : .45);
        p.setPen(Qt::white);
        p.drawText(lk, Qt::AlignCenter, locked ? QString::fromUtf8("\U0001F512") : QString::fromUtf8("\U0001F513"));
        p.setOpacity(1);
        lbl_.push_back({lk, l.id, 0});
        x += 18;
        const QRect tg(x, y + (l.h - 1 - 16) / 2, 16, 16);
        if (audio) {
            p.setFont(uiFont(10, true));
            const QRect mb(x + 1, tg.top() + 2, 13, 13);
            if (off) { p.setPen(Qt::NoPen); p.setBrush(QColor("#2e8b57")); p.drawRoundedRect(mb, 3, 3); }
            p.setBrush(Qt::NoBrush);
            p.setPen(off ? QPen(QColor("#2e8b57")) : QPen(QColor("#555555")));
            if (!off) p.drawRoundedRect(mb.adjusted(0, 0, -1, -1), 3, 3);
            p.setPen(off ? Qt::white : kDim);
            p.drawText(mb, Qt::AlignCenter, "M");
        } else {
            p.setFont(uiFont(11));
            p.setPen(off ? QColor("#f5c542") : kDim);
            p.drawText(tg, Qt::AlignCenter, off ? QString::fromUtf8("◌") : QString::fromUtf8("\U0001F441"));
        }
        lbl_.push_back({tg, l.id, 1});
        x += 20;
        p.setFont(uiFont(11));
        p.setOpacity((locked ? .5 : 1) * (off ? .45 : 1));
        p.setPen(vt ? QColor("#cfe3ff") : kDim);
        p.drawText(QRect(x, y, kLabelW - 2 - x, l.h - 1), Qt::AlignLeft | Qt::AlignVCenter, l.name);
        p.setOpacity(1);
        y += l.h;
    }
    p.restore();
    // ルーラー側のラベル(🧲 区切りで止める)
    p.fillRect(QRect(0, 0, kLabelW - 1, kRuler), kPanel);
    p.setPen(QColor("#444444"));
    p.setBrush(Qt::NoBrush);
    const QRect sb(3, (kRuler - 18) / 2, 108, 18);
    if (seekSnap_) { p.setPen(QColor("#4a90e2")); p.setBrush(QColor("#24425f")); }
    else p.setOpacity(.6);
    p.drawRoundedRect(sb, 3, 3);
    p.setFont(uiFont(10));
    p.setPen(seekSnap_ ? QColor(Qt::white) : kDim);
    p.drawText(sb, Qt::AlignCenter, QString::fromUtf8("\U0001F9F2 区切りで止める"));
    p.setOpacity(1);
    p.setPen(kLine);
    p.drawLine(0, kRuler - 1, kLabelW - 1, kRuler - 1);
}

// 再生ヘッドだけ動いた時: 前の線と今の線のまわりだけ描き直す(ほかはためた絵を貼る)
void TimelineView::updatePlayhead() {
    const int x = (int)std::round(playX()) - hbar_->value() + kLabelW;
    if (x == phX_) return;
    QWidget::update(QRect(phX_ - 8, 0, 18, height()));
    QWidget::update(QRect(x - 8, 0, 18, height()));
}

void TimelineView::paintEvent(QPaintEvent*) {
    const int sc = hbar_->value();
    const qreal dpr = devicePixelRatioF();
    const QSize px = (QSizeF(size()) * dpr).toSize();
    if (!baseOk_ || base_.size() != px || base_.devicePixelRatio() != dpr || baseSc_ != sc || baseV_ != vOff_) {
        if (base_.size() != px) base_ = QPixmap(px);
        base_.setDevicePixelRatio(dpr);
        QPainter bp(&base_);
        paintBase(bp);
        baseOk_ = true; baseSc_ = sc; baseV_ = vOff_;
    }
    QPainter p(this);
    p.drawPixmap(0, 0, base_);
    // 再生ヘッド
    const int viewH = height() - 15;
    const double x = std::round(playX()) - sc + kLabelW;
    phX_ = (int)x;
    if (x >= kLabelW - 7 && x <= width() + 7) {
        p.setClipRect(QRect(kLabelW, 0, contentWidth(), viewH));
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(QColor("#ff5d5d"), 2));
        p.drawLine(QPointF(x + 1, 0), QPointF(x + 1, viewH));
        QPainterPath tri;
        tri.moveTo(x + 1 - 6, 0); tri.lineTo(x + 1 + 6, 0); tri.lineTo(x + 1, 9);
        tri.closeSubpath();
        p.fillPath(tri, QColor("#ff5d5d"));
    }
}

void TimelineView::paintBase(QPainter& p) {
    build();
    LanePin pin(pin_);
    const int sc = hbar_->value();
    p.setRenderHint(QPainter::TextAntialiasing);
    p.fillRect(rect(), kPanel);
    const int viewH = height() - 15;
    p.save();
    p.setClipRect(QRect(kLabelW, kRuler, contentWidth(), viewH - kRuler));
    // トラックの地
    int y = kRuler - vOff_, n = 0;
    for (auto& l0 : lanes_) {
        LaneDef l = l0;
        l.h = laneHeight(l0.id);
        QRect lane(kLabelW, y, contentWidth(), l.h);
        p.fillRect(lane, n % 2 ? QColor("#202025") : kPanel);
        if (m_->isLocked(l.id)) p.fillRect(lane, QBrush(QColor(255, 255, 255, 13), Qt::BDiagPattern));
        p.setPen(kLine);
        p.drawLine(lane.left(), y + l.h - 1, lane.right(), y + l.h - 1);
        if (l.id == "a1") drawWave(p, y, l.h);
        y += l.h; ++n;
    }
    // イン/アウトの帯
    if (m_->markIn >= 0) {
        const double a = m_->markIn, b = m_->markOut >= 0 ? m_->markOut : m_->markIn;
        const double x0 = X(std::min(a, b)) - sc + kLabelW, x1 = X(std::max(a, b)) - sc + kLabelW;
        p.fillRect(QRectF(x0, kRuler - vOff_, std::max(2.0, x1 - x0), fullHeight()), QColor(59, 130, 246, 46));
        p.setPen(QColor("#3b82f6"));
        p.drawLine(QPointF(x0, kRuler - vOff_), QPointF(x0, fullHeight() - vOff_));
        p.drawLine(QPointF(x0 + std::max(2.0, x1 - x0), kRuler - vOff_), QPointF(x0 + std::max(2.0, x1 - x0), fullHeight() - vOff_));
    }
    p.restore();
    // 要素(下のトラックの要素が上に被らないよう、順に描く)
    {
        const QRect area(kLabelW, kRuler, contentWidth(), height() - kRuler - 15);
        p.save();
        p.setClipRect(area);
        p.setRenderHint(QPainter::Antialiasing, false);
        QHash<QString, bool> hidden;
        for (const El& e : els_) {
            if (e.t == El::Clip) {
                auto it = hidden.find(e.lane);
                if (it == hidden.end()) it = hidden.insert(e.lane, m_->isHidden(e.lane));
                if (*it) { p.setOpacity(.35); drawClip(p, e, sc, area, .35); p.setOpacity(1); continue; }
            }
            drawClip(p, e, sc, area, 1);
        }
        p.restore();
    }
    // 分割線
    p.save();
    p.setClipRect(QRect(kLabelW, kRuler, contentWidth(), viewH - kRuler));
    auto splitLine = [&](const QString& lane, double x) {
        const double lx = x - sc + kLabelW;
        if (lx < kLabelW - 2 || lx > width() + 2) return;
        const double ly = laneTop(lane) - vOff_;
        p.setPen(QPen(QColor(255, 255, 255, 230), 2));
        p.drawLine(QPointF(lx, ly), QPointF(lx, ly + laneHeight(lane) - 1));
    };
    for (double x : splitsV_) { splitLine("v1", x); splitLine("a1", x); }
    for (auto& cs : clipSplits_) splitLine(cs.first, cs.second);
    p.restore();
    // 範囲選択の枠・範囲カットの帯
    p.save();
    p.setClipRect(QRect(kLabelW, kRuler, contentWidth(), viewH - kRuler));
    if (drag_ == MarqueeD && (mq_.width() > 0 || mq_.height() > 0)) {
        const QRectF r = mq_.translated(kLabelW - sc, -vOff_);
        p.fillRect(r, QColor(255, 216, 77, 31));
        p.setBrush(Qt::NoBrush); p.setPen(QPen(QColor("#ffd84d"), 2));
        p.drawRect(r.adjusted(1, 1, -1, -1));
    }
    if (drag_ == CutRangeD) {
        const double x0 = X(std::min(ctA_, ctB_)) - sc + kLabelW, x1 = X(std::max(ctA_, ctB_)) - sc + kLabelW;
        p.fillRect(QRectF(x0, laneTop("v1") - vOff_, std::max(1.0, x1 - x0), laneHeight("v1")), QColor(229, 72, 77, 110));
    }
    p.restore();
    drawRuler(p);
    drawLabels(p);
    if (drag_ == LabelD && lbOn_ && lbSlot_ >= 0) {   // 並べ替えの挿入線
        std::vector<LaneDef> vls;
        for (auto& l : lanes_) if (l.vn) vls.push_back(l);
        int ly = lbSlot_ < (int)vls.size() ? laneTop(vls[lbSlot_].id) - vOff_ : (vls.empty() ? kRuler : laneTop(vls.back().id) - vOff_ + laneHeight(vls.back().id));
        p.setPen(QPen(QColor("#4da3ff"), 2));
        p.drawLine(0, ly, kLabelW - 2, ly);
    }
}

int TimelineView::hitEl(const QPoint& pos) const {
    const double sx = hbar_->value();
    for (int i = (int)els_.size() - 1; i >= 0; --i) {
        const El& e = els_[i];
        if (e.ghost) continue;
        if (e.t == El::Cut && e.seam) continue;   // 詰め表示の継ぎ目は押せない(pointer-events:none)
        if (e.r.translated(kLabelW - sx, -vOff_).contains(pos)) return i;
    }
    return -1;
}

// クリップの外側(トラックの空き)でも、端から6px以内ならその端をつまめる(app.js edgeNear)
int TimelineView::nearEdge(const QPoint& pos, QChar* mode) const {
    const QString lane = const_cast<TimelineView*>(this)->laneAtY(pos.y());
    if (lane.isEmpty() || lane == "ruler") return -1;
    const double sx = hbar_->value();
    int best = -1;
    double bd = 6;
    for (int i = 0; i < (int)els_.size(); ++i) {
        const El& e = els_[i];
        if (e.t != El::Clip || e.lane != lane) continue;
        const QRectF r = e.r.translated(kLabelW - sx, -vOff_);
        if (std::fabs(pos.x() - r.right()) < bd) { bd = std::fabs(pos.x() - r.right()); best = i; *mode = 'r'; }
        if (std::fabs(pos.x() - r.left()) < bd) { bd = std::fabs(pos.x() - r.left()); best = i; *mode = 'l'; }
    }
    return best;
}

// 端のつまみは位置で決める。端から8pxまで。短いクリップは幅の1/3まで(真ん中は移動に残す)
QChar TimelineView::edgeMode(const El& e, int px) const {
    const QRectF r = e.r.translated(kLabelW - hbar_->value(), -vOff_);
    const double E = std::max(3.0, std::min(8.0, r.width() / 3));
    return px - r.left() < E ? 'l' : r.right() - px < E ? 'r' : 'm';
}

double TimelineView::snapT(double t) {
    if (!m_->snapOn) return t;
    double best = t, bd = 8 / zoom_;
    std::vector<double> pts = m_->edgePoints();
    pts.push_back(m_->tcur());
    for (double q : pts) if (std::fabs(q - t) < bd) { bd = std::fabs(q - t); best = q; }
    return best;
}

bool TimelineView::selOnSeg(double a, double b, QChar trk) const {
    for (const SegSel& q : m_->segSel)
        if (std::fabs(q.a - a) < 1e-6 && std::fabs(q.b - b) < 1e-6 && (q.trk.isNull() || trk.isNull() || q.trk == trk)) return true;
    return false;
}

void TimelineView::seekAt(const QPoint& pos, bool shift) {
    if (m_->collapsed) {   // 空白(章カードなど)の中は表示上の秒のまま置く(元素材の時刻にすると空白の右端へ飛ぶ)
        const double o = std::max(0.0, (pos.x() - kLabelW + hbar_->value()) / zoom_);
        if (m_->gapPieceAt(o)) { emit seekOutRequested(o); return; }
    }
    double t = timeAtX(pos.x());
    t = m_->seekSnapT(t, zoom_, seekSnap_ || shift);
    emit seekRequested(t);
}

// ---------------- メニュー ----------------
void TimelineView::showMenu(const QPoint& global, const std::vector<MenuItem>& items0) {
    std::vector<MenuItem> items;
    for (size_t i = 0; i < items0.size(); ++i) {   // 区切りは前後に項目がある時だけ
        if (items0[i].sep && (items.empty() || items.back().sep || i + 1 >= items0.size())) continue;
        items.push_back(items0[i]);
    }
    while (!items.empty() && items.back().sep) items.pop_back();
    lastMenu = items;
    if (menuHandler) { menuHandler(global, items); return; }
    QMenu menu(this);
    menu.setStyleSheet("QMenu{background:#26262c;border:1px solid #3a3a43;border-radius:6px;padding:4px 0;min-width:220px}"
                       "QMenu::item{padding:6px 14px;color:#e6e6ea}QMenu::item:selected{background:#3b82f6}"
                       "QMenu::separator{height:1px;background:#3a3a43;margin:4px 0}");
    for (const MenuItem& it : items) {
        if (it.sep) { menu.addSeparator(); continue; }
        QAction* a = menu.addAction(it.label);
        connect(a, &QAction::triggered, this, [fn = it.fn] { if (fn) fn(); });
    }
    menu.exec(global);
}

void TimelineView::clipMenu(const QPoint& g) {
    std::vector<MenuItem> it;
    const bool noSeg = m_->segSel.empty() && m_->gapSel.empty() && m_->selSet.isEmpty();
    if (m_->bgmSel && noSeg) {
        it = {{"BGMを外す (Delete)", [this] { m_->editDelete(false); }}, {"BGMの設定(左パネル)", [this] { emit bgmPanelRequested(); }}};
    } else if (!m_->trkSel.isEmpty() && noSeg) {
        it = {{"削除 (Delete)", [this] { m_->editDelete(false); }}};
    } else if (!m_->segSel.empty() || !m_->gapSel.empty()) {
        if (!m_->segSel.empty()) it.push_back({"削除・空白を残す (Delete)", [this] { m_->editDelete(false); }});
        it.push_back({"リップル削除・詰める (Shift+Delete)", [this] { m_->editDelete(true); }});
        it.push_back({"再生ヘッドで分割 (Ctrl+K)", [this] { m_->splitAll(m_->tcur()); }});
    } else {
        it = {{"コピー (Ctrl+C)", [this] { m_->copySel(); }}, {"再生ヘッドにペースト (Ctrl+V)", [this] { m_->pasteClip(); }}, {"削除 (Delete)", [this] { m_->deleteSel(); }}};
        static const QRegularExpression img("\\.(png|jpe?g|webp)$", QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression ps("\\.(png|jpe?g|webp|psd)$", QRegularExpression::CaseInsensitiveOption);
        if (m_->selKind == "overlays" && m_->selI >= 0) {
            const QString f = m_->d().get("overlays")->at(m_->selI).strOr("file", "");
            if (img.match(f).hasMatch()) it.push_back({"画像編集で編集", [this] { emit imageEditRequested(); }});
            if (ps.match(f).hasMatch()) it.push_back({"Photoshop で編集", [this, f] { emit photoshopRequested(f); }});
        }
        bool anyOv = false;
        for (const QString& k : m_->selSet) if (k.startsWith("overlays:")) anyOv = true;
        if (anyOv) {
            it.push_back({"上のビデオトラックへ移動", [this] { m_->moveToTrack(1); }});
            it.push_back({"下のビデオトラックへ移動", [this] { m_->moveToTrack(-1); }});
        }
    }
    showMenu(g, it);
}

void TimelineView::trackMenu(const QPoint& g, const QString& id) {
    std::vector<MenuItem> it;
    int n = 0;
    QString kind;
    for (auto& l : m_->lanes()) if (l.id == id) { n = l.vn; kind = l.kind; }
    it.push_back({m_->isLocked(id) ? "トラックのロックを解除" : "トラックをロック", [this, id] { m_->toggleLock(id, false); }});
    if (EditModel::isAudioLane(id)) it.push_back({m_->isMuted(id) ? "ミュートを解除" : "トラックをミュート", [this, id] { m_->toggleMute(id, false); }});
    else it.push_back({m_->isHidden(id) ? "トラックを表示(出力する)" : "トラックを非表示(出力しない)", [this, id] { m_->toggleHide(id, false); }});
    if (!kind.isEmpty()) it.push_back({"このトラックのクリップをすべて選択", [this, id, kind] {
        m_->selSet.clear();
        const OVal& a = *m_->d().get(kind);
        for (int i = 0; i < a.size(); ++i) if (m_->laneOf(kind, a.at(i)) == id) { m_->selSet.insert(kind + ":" + QString::number(i)); m_->selKind = kind; m_->selI = i; }
        emit m_->selectionChanged();
    }});
    it.push_back({QString(), nullptr, true});
    if (n) {
        it.push_back({"トラック名を変更…", [this, n] {
            const QString cur = m_->vName(n);
            QString s;
            if (askText) s = askText(QString("V%1 の名前").arg(n), cur);
            else { bool ok = false; s = QInputDialog::getText(this, "V-edit", QString("V%1 の名前").arg(n), QLineEdit::Normal, cur, &ok); if (!ok) return; }
            if (s.isNull()) return;
            m_->renameVTrack(n, s);
        }});
        it.push_back({"上にビデオトラックを追加", [this, n] { m_->addVTrack(n + 1); }});
        it.push_back({"下にビデオトラックを追加", [this, n] { m_->addVTrack(n); }});
        const int top = m_->vCount() + 1;
        if (n < top) it.push_back({"トラックを上へ移動", [this, n, top] { m_->moveVTrack(n, top - n - 1); }});
        if (n > 2) it.push_back({"トラックを下へ移動", [this, n, top] { m_->moveVTrack(n, top - n + 2); }});
        it.push_back({"このトラックを削除", [this, n] {
            const int cnt = m_->overlaysOn(n);
            if (m_->vCount() <= 1) { m_->deleteVTrack(n); return; }
            if (cnt) {
                const QString msg = QString("V%1 の差し込み %2 個も一緒に削除します。よろしいですか?").arg(n).arg(cnt);
                const bool ok = confirm ? confirm(msg) : QMessageBox::question(this, "V-edit", msg) == QMessageBox::Yes;
                if (!ok) return;
            }
            m_->deleteVTrack(n);
        }});
        it.push_back({"空のビデオトラックを削除", [this] { m_->deleteEmptyVTracks(); }});
        if (m_->vTarget != n) it.push_back({"ここを差し込みの追加先にする", [this, n] { m_->vTarget = n; update(); }});
    } else it.push_back({"ビデオトラックを追加(一番上)", [this] { m_->addVTrack(m_->vCount() + 2); }});
    it.push_back({QString(), nullptr, true});
    it.push_back({"すべてのトラックを表示・ミュート解除", [this] { m_->mutate([this] { m_->d().remove("hide"); m_->d().remove("mute"); }); }});
    it.push_back({"すべてのトラックのロックを解除", [this] { QStringList all; for (auto& l : m_->lanes()) all << l.id; m_->setLocks(all, false); }});
    if (lz_.count(id)) it.push_back({"このトラックの高さを元に戻す", [this, id] { lz_.erase(id); clampV(); update(); emit vChanged(); }});
    it.push_back({"トラックの高さを元に戻す(すべて)", [this] { resetHeights(); }});
    showMenu(g, it);
}

// ---------------- マウス ----------------
void TimelineView::rightClick(QMouseEvent* ev) {
    const QPoint pos = ev->position().toPoint();
    const QPoint g = ev->globalPosition().toPoint();
    if (pos.x() < kLabelW) {
        const QString lane = laneAtY(pos.y());
        if (!lane.isEmpty() && lane != "ruler") trackMenu(g, lane);
        return;
    }
    build();
    const int hi = hitEl(pos);
    if (hi < 0) return;
    const El e = els_[hi];
    if (m_->isLocked(e.lane)) return;
    const bool one = !m_->linked || (ev->modifiers() & Qt::AltModifier);
    switch (e.t) {
    case El::Clip:
        if (!m_->selSet.contains(e.kind + ":" + QString::number(e.idx))) m_->select(e.kind, e.idx);
        clipMenu(g);
        break;
    case El::Seg:
        if (!selOnSeg(e.a, e.b, one ? e.trk : QChar())) { m_->select(QString(), -1); m_->gapSel.clear(); m_->segSel = {SegSel{e.a, e.b, one ? e.trk : QChar()}}; emit m_->selectionChanged(); }
        clipMenu(g);
        break;
    case El::Gap:
        if (!std::any_of(m_->gapSel.begin(), m_->gapSel.end(), [&](double x) { return std::fabs(x - e.a) < 1e-6; })) {
            m_->select(QString(), -1); m_->segSel.clear(); m_->gapSel = {e.a}; emit m_->selectionChanged();
        }
        clipMenu(g);
        break;
    case El::Mute: case El::Slip: {
        const QString key = e.kind + ":" + QString::number(e.idx);
        if (!m_->trkSel.contains(key)) { m_->select(QString(), -1); m_->segSel.clear(); m_->trkSel = {key}; emit m_->selectionChanged(); }
        clipMenu(g);
        break;
    }
    case El::Bgm:
        if (!m_->bgmSel) { m_->select(QString(), -1); m_->segSel.clear(); m_->gapSel.clear(); m_->bgmSel = true; emit m_->selectionChanged(); }
        clipMenu(g);
        break;
    default: break;
    }
}

void TimelineView::mousePressEvent(QMouseEvent* ev) {
    setFocus(Qt::MouseFocusReason);
    if (ev->button() == Qt::RightButton) { rightClick(ev); return; }
    if (ev->button() != Qt::LeftButton) return;
    const QPoint pos = ev->position().toPoint();
    press_ = pos;
    const bool mod = ev->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
    lastX_ = pos.x();
    lastShift_ = ev->modifiers() & Qt::ShiftModifier;
    if (pos.x() < kLabelW) {
        if (pos.y() < kRuler) {
            if (QRect(3, (kRuler - 18) / 2, 108, 18).contains(pos)) setSeekSnap(!seekSnap_);
            return;
        }
        for (auto& b : lbl_) if (b.r.contains(pos)) {
            const bool all = ev->modifiers() & Qt::ShiftModifier;
            if (b.what == 0) m_->toggleLock(b.lane, all);
            else if (EditModel::isAudioLane(b.lane)) m_->toggleMute(b.lane, all);
            else m_->toggleHide(b.lane, all);
            return;
        }
        const QString lane = laneAtY(pos.y());
        if (lane.isEmpty() || lane == "ruler") return;
        const int top = laneTop(lane) - vOff_, h = laneHeight(lane);
        if (pos.y() >= top + h - 5) {   // 下端 = そのトラックだけ高さを変える
            drag_ = LaneResizeD; rzLane_ = lane; rzH0_ = h;
            return;
        }
        int vn = 0;
        for (auto& l : m_->lanes()) if (l.id == lane) vn = l.vn;
        if (vn) { drag_ = LabelD; lbLane_ = lane; lbVn_ = vn; lbOn_ = false; lbSlot_ = -1; lbY0_ = pos.y(); lbCurY_ = pos.y(); }
        return;
    }
    build();
    // カットツール: V1 の上ならクリップの上でもドラッグで範囲カット(クリップの移動より先に拾う)
    if (m_->tool == "cut" && pos.y() >= kRuler) {
        const QString lane = laneAtY(pos.y());
        if (lane == "v1") { drag_ = CutRangeD; ctA_ = ctB_ = clampd(timeAtX(pos.x()), 0, m_->dur()); update(); return; }
    }
    if (pos.y() < kRuler) {   // ルーラー = シーク
        drag_ = Seek;
        emit scrubStarted();
        seekAt(pos, lastShift_);
        return;
    }
    QChar edgeM = 'm';
    int hi = hitEl(pos);
    const bool razor = m_->tool == "razor";
    if (hi < 0 && !razor) {   // 空き: 端から6px以内ならそのクリップの端
        QChar nm;
        const int ni = nearEdge(pos, &nm);
        if (ni >= 0) {
            const El e = els_[ni];
            if (!m_->isLocked(e.lane)) { clipDown(ev, ni, nm); return; }
        }
    }
    if (razor) {
        if (hi >= 0 && els_[hi].t == El::Seg) { emit splitRequested(snapT(timeAtX(pos.x()))); return; }
        if (hi < 0 || true) { emit splitRequested(snapT(timeAtX(pos.x()))); return; }
    }
    if (hi < 0) {
        if (std::fabs(pos.x() - (std::round(playX()) - hbar_->value() + kLabelW)) <= 6) {   // 赤い線をつまむ
            drag_ = Seek;
            emit scrubStarted();
            seekAt(pos, lastShift_);
            return;
        }
        startMarquee(pos, mod);
        return;
    }
    const El e = els_[hi];
    if (m_->isLocked(e.lane)) return;
    switch (e.t) {
    case El::Clip:
        edgeM = edgeMode(e, pos.x());
        clipDown(ev, hi, edgeM);
        break;
    case El::Seg: {
        const QRectF r = e.r.translated(kLabelW - hbar_->value(), -vOff_);
        if (pos.x() - r.left() < 6 || r.right() - pos.x() < 6) {   // i.l / i.r
            startSegTrim(e, pos.x() - r.left() < 6 ? 'l' : 'r', pos);
            break;
        }
        const bool one = !m_->linked || (ev->modifiers() & Qt::AltModifier);
        SegSel ent{e.a, e.b, one ? e.trk : QChar()};
        auto same = [&](const SegSel& q) { return std::fabs(q.a - e.a) < 1e-6 && std::fabs(q.b - e.b) < 1e-6 && (q.trk.isNull() || e.trk.isNull() || q.trk == e.trk); };
        const bool on = std::any_of(m_->segSel.begin(), m_->segSel.end(), same);
        if (mod) {
            if (on) m_->segSel.erase(std::remove_if(m_->segSel.begin(), m_->segSel.end(), same), m_->segSel.end());
            else m_->segSel.push_back(ent);
            emit m_->selectionChanged();
            break;
        }
        bool anyLinked = false;
        for (const SegSel& q : m_->segSel) if (q.trk.isNull()) anyLinked = true;
        if (!on || (one && anyLinked)) {
            m_->select(QString(), -1);
            m_->gapSel.clear();
            m_->segSel = {ent};
            emit m_->selectionChanged();
        }
        if (one) { if (e.trk == 'a') startSlip(e, pos); else { /* 映像だけの移動はまだできない */ } break; }
        startSegMove(e, pos);
        break;
    }
    case El::Gap: {
        auto has = [&](double x) { return std::fabs(x - e.a) < 1e-6; };
        const bool on = std::any_of(m_->gapSel.begin(), m_->gapSel.end(), has);
        if (mod) {
            if (on) m_->gapSel.erase(std::remove_if(m_->gapSel.begin(), m_->gapSel.end(), has), m_->gapSel.end());
            else m_->gapSel.push_back(e.a);
        } else { m_->select(QString(), -1); m_->segSel.clear(); m_->gapSel = {e.a}; }
        emit m_->selectionChanged();
        break;
    }
    case El::Mute:
    case El::Slip: {
        const QString key = e.kind + ":" + QString::number(e.idx);
        const bool on = m_->trkSel.contains(key);
        if (mod) { if (on) m_->trkSel.removeAll(key); else m_->trkSel << key; emit m_->selectionChanged(); break; }
        if (!on) { m_->select(QString(), -1); m_->segSel.clear(); m_->gapSel.clear(); m_->trkSel = {key}; emit m_->selectionChanged(); }
        if (e.t == El::Slip) startSlip(e, pos);
        break;
    }
    case El::Cut:
        m_->select("cuts", e.idx);
        if (!m_->collapsed) {   // 元の並びの表示では赤い帯の端・中をドラッグ
            const QRectF r = e.r.translated(kLabelW - hbar_->value(), -vOff_);
            El c = e;
            c.kind = "cuts";
            startClipDrag(c, pos.x() - r.left() < 6 ? 'l' : r.right() - pos.x() < 6 ? 'r' : 'm', pos);
        }
        break;
    case El::Bgm:
        if (mod) m_->bgmSel = !m_->bgmSel;
        else { m_->select(QString(), -1); m_->segSel.clear(); m_->gapSel.clear(); m_->bgmSel = true; }
        emit m_->selectionChanged();
        emit bgmPanelRequested();
        break;
    }
}

void TimelineView::clipDown(QMouseEvent* ev, int ei, QChar mode) {
    const El e = els_[ei];
    if (ev->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) { m_->toggleSel(e.kind, e.idx); return; }   // Premiere: Ctrl/Shift+クリックで追加選択
    const QString key = e.kind + ":" + QString::number(e.idx);
    if (!m_->selSet.contains(key)) m_->select(e.kind, e.idx);
    else { m_->selKind = e.kind; m_->selI = e.idx; emit m_->selectionChanged(); }
    startClipDrag(e, mode, ev->position().toPoint());
}

static void getR(const QString& k, const OVal& it, double* a, double* b) {
    if (k == "cuts") { *a = it.at(0).asNum(); *b = it.at(1).asNum(); }
    else { *a = it.numOr("start", 0); *b = it.numOr("end", 0); }
}
static void setR(const QString& k, OVal& it, double a, double b) {
    if (k == "cuts") { it.at(0) = OVal::num(a); it.at(1) = OVal::num(b); }
    else { it.set("start", OVal::num(a)); it.set("end", OVal::num(b)); }
}

void TimelineView::startClipDrag(const El& e, QChar mode, const QPoint& pos) {
    OVal& d = m_->d();
    const QString kind = e.kind;
    OVal& it = d.get(kind)->at(e.idx);
    if (kind != "cuts" && EditModel::isGapIt(it)) { startGapItemDrag(e, mode, pos); return; }
    cd_ = CD();
    cd_.kind = kind; cd_.i = e.idx; cd_.mode = mode; cd_.x0 = pos.x(); cd_.y0 = pos.y();
    getR(kind, it, &cd_.s0, &cd_.e0);
    cd_.in0 = it.numOr("in", 0);
    cd_.snapPts = {m_->tcur(), 0, m_->dur()};
    for (const char* k : {"overlays", "topics", "telops", "sfx", "cuts"}) {
        const OVal& a = *d.get(k);
        for (int j = 0; j < a.size(); ++j) {
            if (QString(k) == kind && j == e.idx) continue;
            if (QString(k) != "cuts" && EditModel::isGapIt(a.at(j))) continue;
            double x, y; getR(k, a.at(j), &x, &y);
            cd_.snapPts.push_back(x); cd_.snapPts.push_back(y);
        }
    }
    if (kind != "cuts" && mode == 'm') {
        for (const QString& s : m_->selSet) {
            const QString k = s.left(s.indexOf(':')); const int j = s.mid(s.indexOf(':') + 1).toInt();
            if (k == "cuts" || (k == kind && j == e.idx)) continue;
            const OVal& o = d.get(k)->at(j);
            if (EditModel::isGapIt(o)) continue;
            double a, b; getR(k, o, &a, &b);
            cd_.group.push_back({k, j, a, b});
        }
    }
    drag_ = ClipD;
}

void TimelineView::moveClipDrag(const QPoint& pos) {
    OVal& d = m_->d();
    const QString kind = cd_.kind;
    const bool coll = m_->collapsed;
    const double dm = pos.x() - cd_.x0;
    const bool vert = kind == "overlays" && cd_.mode == 'm' && std::abs(pos.y() - cd_.y0) >= 8;
    if (!cd_.moved && std::abs(dm) < 3 && !vert) return;
    if (!cd_.moved) { m_->snap(); cd_.moved = true; }
    const double dx = dm / zoom_;
    OVal& it = d.get(kind)->at(cd_.i);
    auto mapd = [&](double b0, double dd) { return coll ? m_->srcOf(m_->outTime(b0) + dd, true) : b0 + dd; };
    auto sn = [&](double t) {
        if (!m_->snapOn) return t;
        double best = t, bd = 8 / zoom_;
        for (double q : cd_.snapPts) if (std::fabs(q - t) < bd) { bd = std::fabs(q - t); best = q; }
        return best;
    };
    const double dur = m_->dur();
    double a = cd_.s0, b = cd_.e0;
    if (cd_.mode == 'm' && std::abs(dm) < 3) { /* 上下だけ = 時刻はそのまま */ }
    else if (cd_.mode == 'm') {
        const double len = cd_.e0 - cd_.s0;
        a = clampd(sn(mapd(cd_.s0, dx)), 0, dur - len);
        if (std::fabs(a - mapd(cd_.s0, dx)) < 1e-9) a = clampd(sn(mapd(cd_.e0, dx)) - len, 0, dur - len);
        b = a + len;
    } else if (cd_.mode == 'l') a = clampd(sn(mapd(cd_.s0, dx)), 0, cd_.e0 - 0.05);
    else b = clampd(sn(mapd(cd_.e0, dx)), cd_.s0 + 0.05, dur);
    setR(kind, it, toFixedNum(a, 3), toFixedNum(b, 3));
    if (kind == "sfx" && cd_.mode == 'l') it.set("in", OVal::num(toFixedNum(std::max(0.0, cd_.in0 + m_->inDelta(it, cd_.s0, a)), 3)));
    for (auto& g : cd_.group) {
        const double dd = clampd(a - cd_.s0, -g.a, dur - g.b);
        setR(g.k, d.get(g.k)->at(g.j), toFixedNum(g.a + dd, 3), toFixedNum(g.b + dd, 3));
    }
    if (kind == "overlays" && cd_.mode == 'm') {   // 上下にドラッグ = 別のビデオトラックへ
        const QString ln = laneAtY(pos.y());
        int vn = 0;
        for (auto& l : m_->lanes()) if (l.id == ln) vn = l.vn;
        if (vn) {
            const int dt = vn - EditModel::trackOf(it);
            std::vector<OVal*> ovs = {&it};
            for (auto& g : cd_.group) if (g.k == "overlays") ovs.push_back(&d.get("overlays")->at(g.j));
            const int top = m_->vCount() + 1;
            bool ok = dt != 0 && !m_->isLocked(ln);
            for (OVal* o : ovs) { const int nt = EditModel::trackOf(*o) + dt; if (nt < 2 || nt > top) ok = false; }
            if (ok) for (OVal* o : ovs) { const int nt = EditModel::trackOf(*o) + dt; if (nt == 2) o->remove("track"); else o->set("track", OVal::num(nt)); }
        }
    }
    if (kind == "cuts") m_->invalidateCache();
    if (cd_.mode != 'm') m_->seek(cd_.mode == 'l' ? a : b);
    update();
    emit liveEdit();
}

void TimelineView::startGapItemDrag(const El& e, QChar mode, const QPoint& pos) {
    OVal& it = m_->d().get(e.kind)->at(e.idx);
    double gd = 0;
    if (!m_->gapOf(it, nullptr, &gd, nullptr) || !m_->collapsed || m_->isLocked(m_->laneOf(e.kind, it))) return;
    cd_ = CD();
    cd_.kind = e.kind; cd_.i = e.idx; cd_.mode = mode; cd_.x0 = pos.x();
    cd_.s0 = it.numOr("start", 0); cd_.e0 = it.numOr("end", 0); cd_.in0 = it.numOr("in", 0); cd_.gd = gd;
    drag_ = GapItemD;
}

void TimelineView::moveGapItemDrag(const QPoint& pos) {
    const double dm = pos.x() - cd_.x0;
    if (!cd_.moved && std::abs(dm) < 3) return;
    if (!cd_.moved) { m_->snap(); cd_.moved = true; }
    const double dx = dm / zoom_, len = cd_.e0 - cd_.s0;
    OVal& it = m_->d().get(cd_.kind)->at(cd_.i);
    double a = cd_.s0, b = cd_.e0;
    if (cd_.mode == 'm') { a = clampd(cd_.s0 + dx, 0, std::max(0.0, cd_.gd - len)); b = a + len; }
    else if (cd_.mode == 'l') a = clampd(cd_.s0 + dx, 0, cd_.e0 - 0.05);
    else b = clampd(cd_.e0 + dx, cd_.s0 + 0.05, cd_.gd);
    it.set("start", OVal::num(toFixedNum(a, 3)));
    it.set("end", OVal::num(toFixedNum(b, 3)));
    if (cd_.kind == "sfx" && cd_.mode == 'l') it.set("in", OVal::num(toFixedNum(std::max(0.0, cd_.in0 + a - cd_.s0), 3)));
    update();
    emit liveEdit();
}

// ---------------- 範囲選択 ----------------
void TimelineView::startMarquee(const QPoint& pos, bool add) {
    drag_ = MarqueeD;
    mqBase_ = add ? m_->selSet : QSet<QString>();
    mqSeg_ = add ? m_->segSel : std::vector<SegSel>();
    mqGap_ = add ? m_->gapSel : std::vector<double>();
    mqBgm_ = add && m_->bgmSel;
    mqTrk_ = add ? m_->trkSel : QStringList();
    if (!add) { m_->clearSel(); }
    const double cx = pos.x() - kLabelW + hbar_->value(), cy = pos.y() + vOff_;
    mq_ = QRectF(cx, cy, 0, 0);
    press_ = pos;
}

void TimelineView::moveMarquee(const QPoint& pos) {
    const double cx = pos.x() - kLabelW + hbar_->value(), cy = pos.y() + vOff_;
    const double x0 = press_.x() - kLabelW + hbar_->value(), y0 = press_.y() + vOff_;
    const double L = std::min(x0, cx), R = std::max(x0, cx), T = std::min(y0, cy), B = std::max(y0, cy);
    mq_ = QRectF(L, T, R - L, B - T);
    m_->selSet = mqBase_;
    m_->segSel = mqSeg_; m_->gapSel = mqGap_; m_->bgmSel = mqBgm_; m_->trkSel = mqTrk_;
    const bool lk = m_->linked;
    for (const El& e : els_) {
        if (e.ghost || e.t == El::Cut || m_->isLocked(e.lane)) continue;
        const int lt = laneTop(e.lane), lh = laneHeight(e.lane);
        if (lt > B || lt + lh < T) continue;
        if (e.t == El::Bgm) { if (L <= m_->totalOut() * zoom_) m_->bgmSel = true; continue; }
        if (e.r.right() < L || e.r.left() > R) continue;
        switch (e.t) {
        case El::Clip: m_->selSet.insert(e.kind + ":" + QString::number(e.idx)); break;
        case El::Seg: {
            const QChar trk = e.trk;
            if (!selOnSeg(e.a, e.b, lk ? QChar() : trk)) m_->segSel.push_back(SegSel{e.a, e.b, lk ? QChar() : trk});
            break;
        }
        case El::Gap: {
            bool has = false;
            for (double g : m_->gapSel) if (std::fabs(g - e.a) < 1e-6) has = true;
            if (!has) m_->gapSel.push_back(e.a);
            break;
        }
        case El::Mute: case El::Slip: {
            const QString k = e.kind + ":" + QString::number(e.idx);
            if (!m_->trkSel.contains(k)) m_->trkSel << k;
            break;
        }
        default: break;
        }
    }
    emit m_->selectionChanged();
    update();
}

// ---------------- V1/A1 のクリップ移動・トリム・スリップ ----------------
void TimelineView::startSegMove(const El& e, const QPoint& pos) {
    drag_ = SegMoveD; sgA_ = e.a; sgB_ = e.b; sgMoved_ = false; sgHave_ = false;
    press_ = pos;
}

void TimelineView::moveSegMove(const QPoint& pos) {
    if (!sgMoved_ && std::abs(pos.x() - press_.x()) < 4) return;
    if (!m_->collapsed) { if (!sgMoved_) emit m_->toast("クリップの移動は「詰め表示」で行えます"); sgMoved_ = true; return; }
    if (m_->isLocked("v1") || m_->isLocked("a1")) { if (!sgMoved_) emit m_->toast("V1/A1 はロック中です"); sgMoved_ = true; return; }
    OVal& d = m_->d();
    if (!sgMoved_) {
        m_->snap(); sgMoved_ = true; sgHave_ = true;
        sgBaseCuts_ = *d.get("cuts");
        sgBaseGaps_ = d.get("gaps") ? *d.get("gaps") : OVal::array();
    }
    if (!sgHave_) return;
    d.set("cuts", sgBaseCuts_);
    d.set("gaps", sgBaseGaps_);
    m_->invalidateCache();
    const auto plan = m_->planMove(sgA_, sgB_, (pos.x() - press_.x()) / zoom_, 8 / zoom_);
    if (plan.ok) {
        for (auto& c : plan.cuts) { OVal a = OVal::array(); a.push(OVal::num(c.first)); a.push(OVal::num(c.second)); d.get("cuts")->push(a); }
        OVal g = OVal::array();
        for (auto& x : plan.gaps) { OVal a = OVal::array(); a.push(OVal::num(x.first)); a.push(OVal::num(x.second)); g.push(a); }
        d.set("gaps", g);
    }
    m_->invalidateCache();
    update();
    emit liveEdit();
}

void TimelineView::startSegTrim(const El& e, QChar side, const QPoint& pos) {
    m_->snap();
    drag_ = SegTrimD; stSide_ = side; sgA_ = e.a; sgB_ = e.b; press_ = pos; stMoved_ = false;
    const OVal& cuts = *m_->d().get("cuts");
    const double edge = side == 'l' ? e.a : e.b;
    stCut_ = -1;
    for (int i = 0; i < cuts.size(); ++i) {
        const double lo = std::min(cuts.at(i).at(0).asNum(), cuts.at(i).at(1).asNum()), hi = std::max(cuts.at(i).at(0).asNum(), cuts.at(i).at(1).asNum());
        if (std::fabs((side == 'l' ? hi : lo) - edge) < 1e-6) { stCut_ = i; stFixed_ = side == 'l' ? lo : hi; break; }
    }
    if (stCut_ < 0) stFixed_ = edge;
    stEdge_ = edge;
}

void TimelineView::moveSegTrim(const QPoint& pos) {
    OVal& cuts = *m_->d().get("cuts");
    const double d = (pos.x() - press_.x()) / zoom_;
    const double a = sgA_, b = sgB_;
    const double v = stSide_ == 'l' ? clampd(snapT(a + d), stFixed_, b - 0.05) : clampd(snapT(b + d), a + 0.05, stFixed_);
    stMoved_ = true;
    if (std::fabs(v - stFixed_) < 0.005) {
        if (stCut_ >= 0) { cuts.erase(stCut_); stCut_ = -1; }
    } else {
        OVal c = OVal::array();
        c.push(OVal::num(toFixedNum(std::min(v, stFixed_), 3)));
        c.push(OVal::num(toFixedNum(std::max(v, stFixed_), 3)));
        if (stCut_ >= 0) cuts.at(stCut_) = c;
        else { cuts.push(c); stCut_ = cuts.size() - 1; }
    }
    m_->invalidateCache();
    m_->seek(stSide_ == 'l' ? v : std::max(a, v - 1 / (m_->fps() > 0 ? m_->fps() : 30)));
    emit seekRequested(m_->tcur());
    update();
    emit liveEdit();
}

void TimelineView::startSlip(const El& e, const QPoint& pos) {
    drag_ = SlipD; sgA_ = e.a; sgB_ = e.b; press_ = pos; slMoved_ = false; slIdx_ = -1; slD0_ = 0;
}

void TimelineView::moveSlip(const QPoint& pos) {
    if (!slMoved_ && std::abs(pos.x() - press_.x()) < 4) return;
    OVal& d = m_->d();
    if (!slMoved_) {
        slMoved_ = true;
        if (m_->isLocked("a1")) { emit m_->toast("A1 はロック中です"); return; }
        m_->snap();
        if (!d.get("aslip")) d.set("aslip", OVal::array());
        OVal& a = *d.get("aslip");
        for (int i = 0; i < a.size(); ++i) if (std::fabs(a.at(i).numOr("a", 0) - sgA_) < 1e-6 && std::fabs(a.at(i).numOr("b", 0) - sgB_) < 1e-6) slIdx_ = i;
        if (slIdx_ < 0) {
            OVal sl = OVal::object();
            sl.set("a", OVal::num(toFixedNum(sgA_, 3))); sl.set("b", OVal::num(toFixedNum(sgB_, 3))); sl.set("d", OVal::num(0));
            a.push(sl); slIdx_ = a.size() - 1;
        }
        slD0_ = a.at(slIdx_).numOr("d", 0);
    }
    if (slIdx_ < 0) return;
    const double o = m_->outTime(sgA_), tot = m_->totalOut(), L = sgB_ - sgA_;
    double dd = slD0_ + (pos.x() - press_.x()) / zoom_;
    if (m_->snapOn) {
        double bd = 8 / zoom_, best = dd;
        std::vector<double> pts = {m_->tout(), 0, tot};
        for (const Piece& p : m_->pieces()) { pts.push_back(p.o); pts.push_back(p.o + p.span()); }
        for (double q : pts) for (double off : {0.0, L}) { const double c = q - off - o; if (std::fabs(c - dd) < bd) { bd = std::fabs(c - dd); best = c; } }
        dd = best;
    }
    d.get("aslip")->at(slIdx_).set("d", OVal::num(toFixedNum(clampd(dd, -o, tot - L - o), 3)));
    m_->invalidateCache();
    update();
    emit liveEdit();
}

void TimelineView::mouseMoveEvent(QMouseEvent* ev) {
    const QPoint pos = ev->position().toPoint();
    lastX_ = pos.x();
    lastShift_ = ev->modifiers() & Qt::ShiftModifier;
    switch (drag_) {
    case Seek: {
        // 端へ寄せたらその方向へ送る
        const int M = 40;
        int d = 0;
        if (pos.x() < kLabelW + M) d = pos.x() - (kLabelW + M);
        else if (pos.x() > width() - M) d = pos.x() - (width() - M);
        if (d) hbar_->setValue(hbar_->value() + (int)clampd(d * 0.5, -60, 60));
        seekAt(QPoint((int)clampd(pos.x(), kLabelW, width()), pos.y()), lastShift_);
        break;
    }
    case ClipD: moveClipDrag(pos); break;
    case GapItemD: moveGapItemDrag(pos); break;
    case MarqueeD: moveMarquee(pos); break;
    case SegMoveD: moveSegMove(pos); break;
    case SegTrimD: moveSegTrim(pos); break;
    case SlipD: moveSlip(pos); break;
    case CutRangeD: ctB_ = clampd(timeAtX(pos.x()), 0, m_->dur()); update(); break;
    case LaneResizeD: {
        double hh = 34;
        for (auto& l : m_->lanes()) if (l.id == rzLane_) hh = l.h;
        lz_[rzLane_] = clampd((rzH0_ + pos.y() - press_.y()) / (hh * vz_), 0.5, 5);
        clampV(); update(); emit vChanged();
        break;
    }
    case LabelD: {
        if (!lbOn_ && std::abs(pos.y() - lbY0_) < 5) break;
        lbOn_ = true;
        lbCurY_ = pos.y();
        std::vector<LaneDef> vls;
        for (auto& l : m_->lanes()) if (l.vn) vls.push_back(l);
        int slot = (int)vls.size();
        for (int i = 0; i < (int)vls.size(); ++i) {
            const int top = laneTop(vls[i].id) - vOff_;
            if (pos.y() < top + laneHeight(vls[i].id) / 2) { slot = i; break; }
        }
        lbSlot_ = slot;
        update();
        break;
    }
    default: {
        // カーソル: クリップの端 = 左右矢印
        Qt::CursorShape cs = Qt::ArrowCursor;
        if (pos.x() >= kLabelW && pos.y() >= kRuler) {
            build();
            const int hi = hitEl(pos);
            if (hi >= 0 && els_[hi].t == El::Clip && edgeMode(els_[hi], pos.x()) != 'm') cs = Qt::SizeHorCursor;
            else if (hi >= 0 && els_[hi].t == El::Seg) {
                const QRectF r = els_[hi].r.translated(kLabelW - hbar_->value(), -vOff_);
                if (pos.x() - r.left() < 6 || r.right() - pos.x() < 6) cs = Qt::SizeHorCursor;
            } else if (hi < 0) { QChar nm; if (m_->tool != "razor" && nearEdge(pos, &nm) >= 0) cs = Qt::SizeHorCursor; }
        } else if (pos.x() < kLabelW && pos.y() >= kRuler) {
            const QString lane = laneAtY(pos.y());
            if (!lane.isEmpty() && lane != "ruler" && pos.y() >= laneTop(lane) - vOff_ + laneHeight(lane) - 5) cs = Qt::SizeVerCursor;
        }
        setCursor(cs);
    }
    }
}

void TimelineView::mouseReleaseEvent(QMouseEvent*) {
    const Drag dg = drag_;
    drag_ = None;
    switch (dg) {
    case ClipD: case GapItemD:
        if (cd_.moved) m_->commit();
        break;
    case MarqueeD: {
        QString last;
        for (const QString& s : m_->selSet) last = s;
        if (!last.isEmpty()) { m_->selKind = last.left(last.indexOf(':')); m_->selI = last.mid(last.indexOf(':') + 1).toInt(); }
        else { m_->selKind.clear(); m_->selI = -1; }
        emit m_->selectionChanged();
        update();
        break;
    }
    case SegMoveD:
        if (sgHave_) {
            m_->segSel = {SegSel{sgA_, sgB_, QChar()}};
            m_->gapSel.clear();
            m_->commit();
        }
        break;
    case SegTrimD:
        m_->segSel.clear(); m_->gapSel.clear();
        m_->commit();
        break;
    case SlipD:
        if (slMoved_ && slIdx_ >= 0) {
            OVal& a = *m_->d().get("aslip");
            if (std::fabs(a.at(slIdx_).numOr("d", 0)) < 1e-3) { a.erase(slIdx_); slIdx_ = -1; }
            m_->segSel.clear();
            m_->trkSel = slIdx_ >= 0 ? QStringList{"aslip:" + QString::number(slIdx_)} : QStringList();
            m_->commit();
        }
        break;
    case CutRangeD:
        if (std::fabs(ctB_ - ctA_) > 0.05) {
            const double a = std::min(ctA_, ctB_), b = std::max(ctA_, ctB_);
            m_->mutate([&] { OVal c = OVal::array(); c.push(OVal::num(toFixedNum(a, 3))); c.push(OVal::num(toFixedNum(b, 3))); m_->d().get("cuts")->push(c); });
        }
        update();
        break;
    case LabelD:
        if (!lbOn_) { m_->vTarget = lbVn_; update(); }
        else if (lbSlot_ >= 0) m_->moveVTrack(lbVn_, lbSlot_);
        lbOn_ = false; lbSlot_ = -1;
        update();
        break;
    case LaneResizeD: break;
    default: break;
    }
}

void TimelineView::wheelEvent(QWheelEvent* ev) {
    const int dy = ev->angleDelta().y() ? ev->angleDelta().y() : ev->angleDelta().x();
    const QPoint pos = ev->position().toPoint();
    if (pos.x() < kLabelW) {   // トラック名の上 = 縦ズーム(Alt/Ctrl = そのトラックだけ)
        const double f = dy > 0 ? 1.15 : 1 / 1.15;
        const QString lane = laneAtY(pos.y());
        if ((ev->modifiers() & (Qt::AltModifier | Qt::ControlModifier)) && !lane.isEmpty() && lane != "ruler")
            setLz(lane, clampd(lz(lane) * f, 0.5, 5));
        else setVz(clampd(vz_ * f, 0.6, 3));
        ev->accept();
        return;
    }
    // Premiere 準拠: ホイール=横スクロール / Ctrl・Shift+ホイール=縦スクロール / Alt+ホイール=ズーム(カーソル位置基準)
    if (ev->modifiers() & Qt::AltModifier) setZoom(clampd(zoom_ * (dy > 0 ? 1.2 : 1 / 1.2), 0.5, 300), pos.x() - kLabelW);
    else if (ev->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) setVOff(vOff_ - dy);
    else hbar_->setValue(hbar_->value() - dy);
    ev->accept();
}

}  // namespace vui
