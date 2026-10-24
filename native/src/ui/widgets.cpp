#include "widgets.h"
#include <QEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <limits>
#include "timelineview.h"

namespace vui {

static const double kInf = std::numeric_limits<double>::infinity();

// ---------------- 縦スクロール ----------------
VScrollBar::VScrollBar(TimelineView* tl, QWidget* parent) : QWidget(parent), tl_(tl) {
    setFixedWidth(14);
    setMouseTracking(true);
    setToolTip("トラックを上下に送る(つまみをドラッグ / 空いた所をクリックで1画面 / ホイール)");
    connect(tl, &TimelineView::vChanged, this, [this] { update(); });
}

bool VScrollBar::off() const { return tl_->fullHeight() <= tl_->viewHeight() + 1; }

QRect VScrollBar::thumbRect() const {
    const int H = height(), ch = tl_->viewHeight(), sh = std::max(1, tl_->fullHeight());
    const int hh = (int)std::max(24.0, (double)H * ch / sh);
    const int mx = sh - ch;
    const int top = mx > 0 ? (int)std::lround((H - hh) * (double)tl_->vOff() / mx) : 0;
    return QRect(3, top, width() - 3 - 2, hh);   // border-left 1px の内側に left/right 2px
}

void VScrollBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#18181c"));
    p.fillRect(0, 0, 1, height(), QColor("#3a3a43"));
    if (off()) return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(drag_ || (hover_ && thumbRect().contains(mapFromGlobal(QCursor::pos()))) ? "#8a93a3" : "#4b505b"));
    p.drawRoundedRect(thumbRect(), 5, 5);
}

void VScrollBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || off()) return;
    const QRect t = thumbRect();
    if (t.contains(e->pos())) { drag_ = true; y0_ = e->globalPosition().toPoint().y(); s0_ = tl_->vOff(); update(); return; }
    tl_->setVOff(tl_->vOff() + (e->pos().y() < t.top() ? -1 : 1) * (int)(tl_->viewHeight() * 0.9));
}

void VScrollBar::mouseMoveEvent(QMouseEvent* e) {
    if (drag_) {
        const double k = (double)tl_->vMax() / std::max(1, height() - thumbRect().height());
        tl_->setVOff(s0_ + (int)std::lround((e->globalPosition().toPoint().y() - y0_) * k));
    }
    update();
}

void VScrollBar::mouseReleaseEvent(QMouseEvent*) { drag_ = false; update(); }

void VScrollBar::wheelEvent(QWheelEvent* e) {
    const int d = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
    tl_->setVOff(tl_->vOff() - (int)std::lround(d * 100.0 / 120.0));
    e->accept();
}

// ---------------- メーター ----------------
static constexpr double kMin = -60, kFall = 22, kHoldMs = 1500, kHoldFall = 12, kRmsTau = 0.3;
static constexpr int kTop = 4, kBot = 14, kBW = 15, kGap = 2;

MeterWidget::MeterWidget(QWidget* parent) : QWidget(parent) {
    setFixedWidth(68);
    setCursor(Qt::PointingHandCursor);
    setToolTip("オーディオメーター(dBFS・L/R)。プレビュー音量は差し引いた書き出し相当の値。上の赤ランプ=0dB超え(クリップ)。クリックでピークホールド/クリップをリセット");
    lv_[0] = lv_[1] = hold_[0] = hold_[1] = -kInf;
    clock_.start();
    timer_.setInterval(33);
    connect(&timer_, &QTimer::timeout, this, [this] { frame(); });
}

void MeterWidget::reset() {
    hold_[0] = hold_[1] = -kInf;
    clip_[0] = clip_[1] = false;
    update();
}

void MeterWidget::setPlaying(bool on) {
    playing_ = on;
    if (on) { last_ = clock_.elapsed(); timer_.start(); }
}

void MeterWidget::frame() {
    const qint64 now = clock_.elapsed();
    const double dt = std::min(0.1, std::max(0.0, (now - last_) / 1000.0));
    last_ = now;
    double pk[2] = {-kInf, -kInf}, pw[2] = {0, 0};
    if (playing_ && source) {
        double a[2] = {-kInf, -kInf}, b[2] = {0, 0};
        if (source(a, b)) { pk[0] = a[0]; pk[1] = a[1]; pw[0] = b[0]; pw[1] = b[1]; }
    }
    step(dt, pk, pw);
    bool active = false;
    for (int c = 0; c < 2; ++c) if (std::isfinite(lv_[c]) || std::isfinite(hold_[c]) || pw_[c] > 0) active = true;
    if (!playing_ && !active) timer_.stop();
    update();
}

void MeterWidget::step(double dt, const double pk[2], const double pw[2]) {
    const qint64 now = clock_.elapsed();
    for (int c = 0; c < 2; ++c) {
        if (pk[c] > 0) clip_[c] = true;
        const double fallen = std::isfinite(lv_[c]) ? lv_[c] - kFall * dt : -kInf;
        lv_[c] = std::max(pk[c], fallen);
        if (lv_[c] < kMin) lv_[c] = -kInf;
        if (pk[c] >= hold_[c]) { hold_[c] = pk[c]; holdAt_[c] = now; }
        else if (now - holdAt_[c] > kHoldMs) { hold_[c] = std::max(lv_[c], hold_[c] - kHoldFall * dt); if (hold_[c] < kMin) hold_[c] = -kInf; }
        const double k = 1 - std::exp(-dt / kRmsTau);
        pw_[c] += (pw[c] - pw_[c]) * k;
        if (pw_[c] < 1e-12) pw_[c] = 0;
    }
    update();
}

static QString fmtDb(double d) { return std::isfinite(d) && d > kMin ? QString::number(d, 'f', 1) : QStringLiteral("-∞"); }
QString MeterWidget::peakText() const { return fmtDb(std::max(hold_[0], hold_[1])); }
QString MeterWidget::rmsText() const {
    const double rp = std::max(pw_[0], pw_[1]);
    return fmtDb(rp > 0 ? 10 * std::log10(rp) : -kInf);
}

double MeterWidget::yOf(double d, int top, int bot) const {
    if (!std::isfinite(d) || d <= kMin) return bot;
    return top + std::max(0.0, std::min(-d, -kMin)) / -kMin * (bot - top);
}

void MeterWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#1d1d22"));
    p.fillRect(0, 0, 1, height(), QColor("#3a3a43"));
    const int x0 = 1 + 4, W = width() - x0 - 3;
    // ランプ
    for (int c = 0; c < 2; ++c) {
        const QRect r(x0 + c * (kBW + kGap), 4, kBW, 7);
        if (clip_[c]) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 59, 59, 70));
            p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), 3, 3);
            p.setBrush(QColor("#ff2b2b"));
            p.drawRect(r);
            p.setPen(QColor("#ffb3b3"));
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(0, 0, -1, -1));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor("#4a1a1c"));
            p.drawRect(r);
            p.setPen(Qt::black);
            p.setBrush(Qt::NoBrush);
            p.drawRect(r.adjusted(0, 0, -1, -1));
        }
    }
    // 数値(下)
    QFont nf("Consolas");
    nf.setPixelSize(10);
    p.setFont(nf);
    const int lineH = 12;
    const int numTop = height() - 4 - 23;
    auto numLine = [&](int y, const QString& k, const QString& v, bool hot) {
        p.setPen(QColor("#6b6b77"));
        p.drawText(QRect(x0, y, 9, lineH), Qt::AlignLeft | Qt::AlignVCenter, k);
        p.setPen(QColor(hot ? "#ff5a5a" : "#8d8d99"));
        p.drawText(QRect(x0 + 9, y, W - 9, lineH), Qt::AlignLeft | Qt::AlignVCenter, v);
    };
    numLine(numTop, "P", peakText(), std::max(hold_[0], hold_[1]) > 0);
    numLine(numTop + 11, "R", rmsText(), false);
    // キャンバス
    const int cy = 4 + 7 + 3, ch = std::max(1, numTop - 3 - cy);
    p.save();
    p.translate(x0, cy);
    const int top = kTop, bot = ch - kBot;
    auto at = [](double d) { return (d - kMin) / -kMin; };
    QLinearGradient gr(0, bot, 0, top);
    gr.setColorAt(0, "#127a36"); gr.setColorAt(at(-30), "#25b54c"); gr.setColorAt(at(-12), "#5fe05a");
    gr.setColorAt(at(-6.01), "#a8e04a"); gr.setColorAt(at(-6), "#f2d43a"); gr.setColorAt(at(-1.6), "#f5b830");
    gr.setColorAt(at(-0.6), "#ff8a2a"); gr.setColorAt(at(-0.5), "#ff3030"); gr.setColorAt(1, "#ff3030");
    const int ticks[] = {0, -6, -12, -18, -24, -30, -36, -42, -48, -54};
    auto colOf = [](double d) { return d >= -0.5 ? QColor("#ff3030") : d >= -6 ? QColor("#f2d43a") : d >= -18 ? QColor("#7be05c") : QColor("#2fbf55"); };
    for (int c = 0; c < 2; ++c) {
        const int x = c * (kBW + kGap);
        p.fillRect(x, top, kBW, bot - top, QColor("#0e0e11"));
        p.setOpacity(0.13);
        p.fillRect(QRect(x, top, kBW, bot - top), QBrush(gr));
        p.setOpacity(1);
        if (std::isfinite(lv_[c])) { const int y = (int)yOf(lv_[c], top, bot); p.fillRect(QRect(x, y, kBW, bot - y), QBrush(gr)); }
        if (std::isfinite(hold_[c]) && hold_[c] > kMin) { const int y = (int)std::lround(yOf(hold_[c], top, bot)); p.fillRect(x, std::max(top, y - 1), kBW, 2, colOf(hold_[c])); }
        for (int t : ticks) p.fillRect(QRect(x, (int)std::lround(yOf(t, top, bot)), kBW, 1), QColor(0, 0, 0, 140));
    }
    const int sx = 2 * kBW + kGap + 3;
    QFont tf("Consolas");
    tf.setPixelSize(9);
    p.setFont(tf);
    for (int t : ticks) {
        const int y = (int)std::lround(yOf(t, top, bot));
        p.fillRect(sx - 2, y, 3, 1, t >= -6 ? (t == 0 ? QColor("#e07a7a") : QColor("#cfc27a")) : QColor("#8d8d99"));
        p.setPen(t >= -6 ? (t == 0 ? QColor("#e07a7a") : QColor("#cfc27a")) : QColor("#8d8d99"));
        p.drawText(QRect(sx + 3, y - 6, 24, 12), Qt::AlignLeft | Qt::AlignVCenter, QString::number(t));
    }
    p.setPen(QColor("#8d8d99"));
    p.drawText(QRect(sx + 3, bot - 9, 24, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("-∞"));
    p.setPen(QColor("#a0a0ab"));
    p.drawText(QRect(0, ch - 11, kBW, 10), Qt::AlignCenter, "L");
    p.drawText(QRect(kBW + kGap, ch - 11, kBW, 10), Qt::AlignCenter, "R");
    p.setPen(QColor("#6b6b77"));
    p.drawText(QRect(sx + 3, ch - 11, 24, 10), Qt::AlignLeft | Qt::AlignVCenter, "dB");
    p.restore();
}

// ---------------- モーダル ----------------
ModalOverlay::ModalOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("modal");
    auto* outer = new QVBoxLayout(this);
    outer->setAlignment(Qt::AlignCenter);
    frame_ = new QWidget;
    frame_->setObjectName("modalbox");
    frame_->setMinimumWidth(380);
    frame_->setMaximumWidth(560);
    frame_->setStyleSheet("#modalbox{background:#242429;border:1px solid #3a3a43;border-radius:8px;}");
    lay_ = new QVBoxLayout(frame_);
    lay_->setContentsMargins(18, 18, 18, 18);
    outer->addWidget(frame_, 0, Qt::AlignCenter);
    hide();
    parent->installEventFilter(this);
}

void ModalOverlay::setContent(const QList<QWidget*>& w) {
    for (QWidget* o : cur_) { lay_->removeWidget(o); o->hide(); o->deleteLater(); }
    cur_ = w;
    for (QWidget* o : w) lay_->addWidget(o);
}

void ModalOverlay::open() {
    setGeometry(parentWidget()->rect());
    show();
    raise();
}

void ModalOverlay::close() { hide(); }

void ModalOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 153));
}

bool ModalOverlay::eventFilter(QObject* o, QEvent* e) {
    if (o == parentWidget() && e->type() == QEvent::Resize && isVisible()) setGeometry(parentWidget()->rect());
    return false;
}

// ---------------- 仕上がり確認の重ね表示 ----------------
CheckOverlay::CheckOverlay(QWidget* parent) : QWidget(parent) {
    setObjectName("checkimg");
    x_ = new QPushButton("×", this);
    x_->setFocusPolicy(Qt::NoFocus);
    x_->setCursor(Qt::PointingHandCursor);
    x_->setObjectName("checkclose");
    connect(x_, &QPushButton::clicked, this, [this] { hide(); });
    hide();
}

void CheckOverlay::setImage(const QImage& im) {
    img_ = im;
    update();
}

void CheckOverlay::resizeEvent(QResizeEvent*) {
    x_->adjustSize();
    x_->move(width() - x_->width() - 8, 8);
}

void CheckOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (img_.isNull()) return;
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QSize s = img_.size().scaled(size(), Qt::KeepAspectRatio);
    p.drawImage(QRect(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s), img_);
}

StageHost::StageHost(QWidget* p) : QWidget(p) {
    setAutoFillBackground(false);
    check_ = new CheckOverlay(this);
    check_->hide();
}
void StageHost::setContent(QWidget* w) {
    if (content_) content_->hide();
    content_ = w;
    if (w) { w->setParent(this); w->show(); }
    relayout();
}
void StageHost::setImage(const QImage& im) { img_ = im; update(); }
void StageHost::setAspect(double w, double h) { aw_ = w; ah_ = h; relayout(); }
QRect StageHost::stageRect() const {
    const int pad = 8;
    const QRect a = rect().adjusted(pad, pad, -pad, -pad);
    if (a.width() <= 0 || a.height() <= 0) return QRect();
    double s = std::min(a.width() / aw_, a.height() / ah_);
    const int w = (int)(aw_ * s), h = (int)(ah_ * s);
    return QRect(a.left() + (a.width() - w) / 2, a.top() + (a.height() - h) / 2, w, h);
}
void StageHost::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#101013"));
    const QRect r = stageRect();
    p.fillRect(r, Qt::black);
    if (!img_.isNull() && (!content_ || !content_->isVisible())) p.drawImage(r, img_.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    p.setPen(QColor("#3a3a43"));   // box-shadow:0 0 0 1px var(--line)
    p.drawRect(r.adjusted(-1, -1, 0, 0));
}
void StageHost::relayout() {
    const QRect r = stageRect();
    if (content_) content_->setGeometry(r);
    if (check_) { check_->setGeometry(r); check_->raise(); }
    update();
}
QImage StageHost::grabStage() {
    const QRect r = stageRect();
    QImage out(r.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::black);
    QPainter p(&out);
    if (!img_.isNull()) p.drawImage(QRect(QPoint(0, 0), r.size()), img_.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    if (content_) {
        const QImage g = content_->grab().toImage();
        if (!g.isNull()) p.drawImage(QRect(QPoint(0, 0), r.size()), g.scaled(r.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    if (check_ && check_->isVisible()) p.drawImage(QRect(QPoint(0, 0), r.size()), check_->grab().toImage());
    return out;
}

}  // namespace vui
