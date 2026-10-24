#include "editorwindow.h"
#include "reviewplayer.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStyle>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include "media/videostream.h"
#include "media/membudget.h"
#include "timelineview.h"
#include "widgets.h"

namespace vui {

static const char* kStyle = R"(
* { font-family: "Yu Gothic UI","Meiryo UI"; font-size: 13px; color: #e6e6ea; }
QMainWindow, #root { background: #1b1b1f; }
QToolTip { background: #26262c; color: #e6e6ea; border: 1px solid #3a3a43; }
#top { background: #242429; border-bottom: 1px solid #3a3a43; }
#top QLabel { background: transparent; }
#logo { color: #3b82f6; font-size: 15px; font-weight: bold; }
#savestate { color: #8d8d99; font-size: 12px; }
#sysmon { font-family: Consolas, "Cascadia Mono", monospace; font-size: 11px; color: #8d8d99; }
#warn { background: #5c3b00; padding: 6px 12px; }
QPushButton { background: #2d2d34; border: 1px solid #3a3a43; border-radius: 5px; padding: 4px 10px; }
QPushButton:hover { border-color: #5a5a68; }
QPushButton[on="true"], QPushButton#primary { background: #3b82f6; border-color: #3b82f6; }
QPushButton[warnoff="true"] { background: #7a3b1b; border-color: #e08a4a; color: #fff; }
QPushButton:disabled { color: #6b6b77; }
QPushButton[danger="true"] { color: #ff8a8d; }
QPushButton#rawwarn { background: #7a2e00; color: #ffd7a8; border: 1px solid #ff8a3d; border-radius: 4px; padding: 2px 8px; font-weight: bold; }
#left, #right { background: #242429; }
#left { border-right: 1px solid #3a3a43; }
#right { border-left: 1px solid #3a3a43; }
#review { background: #242429; border-left: 1px solid #3a3a43; }
#tabs { border-bottom: 1px solid #3a3a43; }
#tabs QPushButton { background: transparent; border: 0; border-radius: 0; padding: 7px 12px; color: #8d8d99; border-bottom: 2px solid transparent; }
#tabs QPushButton[on="true"] { color: #e6e6ea; border-bottom: 2px solid #3b82f6; }
#tabs QPushButton:hover { border-color: transparent; }
#tabs QPushButton[on="true"]:hover { border-bottom: 2px solid #3b82f6; }
QScrollArea { background: transparent; border: 0; }
#tabbody { background: transparent; }
QScrollBar:vertical { background: #18181c; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #4b505b; border-radius: 4px; min-height: 24px; margin: 1px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QLabel[class="note"] { color: #8d8d99; font-size: 12px; }
QLabel[class="notewarn"] { color: #ffb84d; font-size: 12px; }
QLabel[class="h4"] { color: #8d8d99; font-size: 12px; font-weight: 600; }
QLabel[class="rowlabel"] { color: #8d8d99; }
QLabel[class="dim"] { color: #8d8d99; }
QLineEdit, QPlainTextEdit, QTextEdit, QDoubleSpinBox, QSpinBox, QComboBox { background: #16161a; color: #e6e6ea; border: 1px solid #3a3a43; border-radius: 4px; padding: 3px 6px; selection-background-color: #3b82f6; }
QComboBox::drop-down { border: 0; width: 18px; }
QComboBox QAbstractItemView { background: #16161a; border: 1px solid #3a3a43; selection-background-color: #3b82f6; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button, QSpinBox::up-button, QSpinBox::down-button { width: 0; border: 0; }
QFrame[class="step"] { background: #2d2d34; border: 1px solid #3a3a43; border-radius: 6px; }
QFrame[class="step"] QLabel { background: transparent; }
QFrame[class="sample"] { background: #333355; border-radius: 6px; }
QListWidget { background: transparent; border: 1px solid #3a3a43; border-radius: 4px; }
QListWidget::item { padding: 3px 6px; border-left: 3px solid transparent; }
QListWidget::item:hover { background: #2d2d34; }
QListWidget::item:selected { background: #2a3242; border-left: 3px solid #3b82f6; color: #e6e6ea; }
#monitor { background: #101013; }
#transport { background: #242429; border-top: 1px solid #3a3a43; }
QFrame[class="tcbox"] { background: #161616; border: 1px solid #3a3a3a; border-radius: 4px; }
QFrame[class="tcbox"] QLabel { background: transparent; }
QLabel[class="tcsmall"] { font-size: 10px; color: #8d8d99; }
QPushButton[class="tccp"] { background: transparent; border: 1px solid #444; border-radius: 3px; color: #8d8d99; font-size: 12px; padding: 0 5px; }
#tc { color: #ffc233; font-family: Consolas, "Cascadia Mono", monospace; font-size: 20px; font-weight: bold; }
#tcout { color: #4fc3ff; font-family: Consolas, "Cascadia Mono", monospace; font-size: 20px; font-weight: bold; }
#tl { background: #242429; border-top: 1px solid #3a3a43; }
#tltools { border-bottom: 1px solid #3a3a43; }
QSlider::groove:horizontal { height: 4px; background: #16161a; border: 1px solid #3a3a43; border-radius: 2px; }
QSlider::handle:horizontal { background: #3b82f6; width: 14px; margin: -6px 0; border-radius: 7px; }
#toast { background: rgba(0,0,0,0.8); border: 1px solid #3a3a43; padding: 8px 16px; border-radius: 6px; }
QCheckBox { color: #8d8d99; spacing: 5px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #3a3a43; background: #16161a; border-radius: 2px; }
QCheckBox::indicator:checked { background: #3b82f6; border-color: #3b82f6; }
#modalbox { background: #242429; border: 1px solid #3a3a43; border-radius: 8px; }
QProgressBar { background: #111; border: 0; border-radius: 4px; height: 8px; text-align: center; color: transparent; }
QProgressBar::chunk { background: #3b82f6; border-radius: 4px; }
)";

static QPushButton* mkBtn(const QString& text, const QString& tip = QString(), const char* name = nullptr) {
    auto* b = new QPushButton(text);
    b->setFocusPolicy(Qt::NoFocus);
    b->setCursor(Qt::PointingHandCursor);
    if (!tip.isEmpty()) b->setToolTip(tip);
    if (name) b->setObjectName(name);
    return b;
}
static void setOn(QPushButton* b, bool on) {
    b->setProperty("on", on);
    b->style()->unpolish(b);
    b->style()->polish(b);
}

EditorWindow::EditorWindow(EditModel* m, QWidget* parent) : QMainWindow(parent), m_(m) {
    setStyleSheet(kStyle);
    m_->linked = prefGet("vedit.linked", "1") != "0";
    vn::setMemBudgetMB(prefGet("vedit.memMB", "2048").toInt());
    m_->loadLocks();
    build();
    loadKeymap();
    askOverwrite = [this] {
        return QMessageBox::question(this, "V-edit",
                                     "ほかのプログラム(CLI/AI など)がプロジェクトを書き換えました。\nはい = この画面の内容で上書き保存\nいいえ = 外部の変更を読み込む(この画面の未保存の編集は .local-bak.json に残す)") == QMessageBox::Yes;
    };
    connect(m_, &EditModel::changed, this, [this] { refresh(); emit contentChanged(); });
    connect(m_, &EditModel::selectionChanged, this, [this] {   // 選択も中身も変わらない時は作り直さない(undo で毎回 6ms)
        if (panelEditing_) return;
        if (const QString s = panelSig(false); s.isEmpty() || s != rightSig_) fillRight();
    });
    connect(m_, &EditModel::playheadChanged, this, [this] { updateTc(); });
    connect(m_, &EditModel::toast, this, [this](const QString& s) { showToast(s); });
    connect(m_, &EditModel::saveStateChanged, this, [this](const QString& s) {
        savestate_->setText(s);
        if (s == QStringLiteral("保存済み") && reviewShown()) reviewRefresh();
    });
    connect(m_, &EditModel::externalLoaded, this, [this] { showToast(QStringLiteral("外部の変更を読み込みました")); });
    connect(m_, &EditModel::conflict, this, [this] {
        if (askOverwrite && askOverwrite()) m_->save(true);
        else {
            const QString bak = m_->writeBackup();   // 読み直す前に、この画面の編集を別のファイルに残す
            QString e; m_->reloadFromDisk(&e);
            showToast(bak.isEmpty() ? QStringLiteral("外部の変更を読み込みました") : QStringLiteral("外部の変更を読み込みました(この画面の編集は %1 に残しました)").arg(QFileInfo(bak).fileName()), 6000);
        }
    });
    m_->isPlaying = [this] { return playing_; };
    refresh();
    resize(1400, 860);
    setWindowTitle("V-edit - " + m_->name());
    QTimer::singleShot(0, this, [this] {
        if (centralWidget() && centralWidget()->layout()) centralWidget()->layout()->activate();
        tl_->zoomFit();   // 初回は全体が収まる拡大率。縦スクロールとメーターの分は TimelineView が狭くなっているので自動で引かれる
    });
    sys_ = new bridge::SysMon(this);
    connect(sys_, &bridge::SysMon::updated, this, [this](const bridge::SysStats& s) { sysLabel_->setText(bridge::SysMon::format(s)); });
    sysTimer_ = new QTimer(this);
    connect(sysTimer_, &QTimer::timeout, sys_, &bridge::SysMon::tick);
    sysTimer_->start(2000);
    QTimer::singleShot(0, sys_, &bridge::SysMon::tick);
    QTimer::singleShot(500, this, [this] { ensureWave(); });
    rvTimer_ = new QTimer(this);
    connect(rvTimer_, &QTimer::timeout, this, [this] { reviewTick(); });
    rvTimer_->start(80);
    if (prefGet("vedit.review") == "1") QTimer::singleShot(0, this, [this] { if (!reviewShown()) reviewToggle(); });
}

EditorWindow::~EditorWindow() {
    dying_ = true;
    if (rvTimer_) rvTimer_->stop();
    for (auto* j : findChildren<bridge::Job*>(QString(), Qt::FindDirectChildrenOnly)) { j->disconnect(); j->cancel(); }   // 壊れ始めた窓へ結果が戻って reviewDraw などが走らないように
    if (rvPlayer_ && !rvPlayer_->parentWidget()) delete rvPlayer_;
    for (auto* j : {audioJob_, exportJob_, silJob_, frameJob_, waveJob_}) if (j) { j->disconnect(); j->cancel(); }
}

void EditorWindow::build() {
    auto* root = new QWidget;
    root->setObjectName("root");
    auto* v = new QVBoxLayout(root);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(buildTop());
    warn_ = new QLabel;
    warn_->setObjectName("warn");
    warn_->hide();
    v->addWidget(warn_);
    auto* main = new QWidget;
    auto* h = new QHBoxLayout(main);
    mainLay_ = h;
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    h->addWidget(buildPanel(true));
    h->addWidget(buildMonitor(), 1);
    h->addWidget(buildPanel(false));
    review_ = new QWidget;     // 4列目(修正点)。📝 修正点で出し入れする
    review_->setObjectName("review");
    review_->setFixedWidth(300);
    reviewLay_ = new QVBoxLayout(review_);
    reviewLay_->setContentsMargins(0, 0, 0, 0);
    reviewLay_->setSpacing(0);
    review_->hide();
    h->addWidget(review_);
    v->addWidget(main, 1);
    v->addWidget(buildTimeline());
    setCentralWidget(root);
    toast_ = new QLabel(root);
    toast_->setObjectName("toast");
    toast_->hide();
    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    connect(toastTimer_, &QTimer::timeout, this, [this] { toast_->hide(); });
    modal_ = new ModalOverlay(root);
    modal_->hide();
}

QWidget* EditorWindow::buildTop() {
    auto* top = new QWidget;
    top->setObjectName("top");
    top->setFixedHeight(40);
    auto* h = new QHBoxLayout(top);
    h->setContentsMargins(12, 0, 12, 0);
    h->setSpacing(8);
    auto* icon = new QLabel;
    const QString ip = QStringLiteral("C:/vedit/vedit/web/icon.png");
    if (QFile::exists(ip)) icon->setPixmap(QPixmap(ip).scaled(18, 18, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    h->addWidget(icon);
    auto* logo = new QLabel("V-edit");
    logo->setObjectName("logo");
    h->addWidget(logo);
    pname_ = new QLabel;
    h->addWidget(pname_);
    h->addStretch(1);
    sysLabel_ = new QLabel;
    sysLabel_->setObjectName("sysmon");
    sysLabel_->setTextFormat(Qt::RichText);
    sysLabel_->setToolTip("CPU / GPU / メモリの使用率(2秒ごと)。GPU は NVIDIA のみ。VRAM = GPU のメモリ");
    h->addWidget(sysLabel_);
    h->addSpacing(4);
    savestate_ = new QLabel(m_->saveState());
    savestate_->setObjectName("savestate");
    h->addWidget(savestate_);
    bSaveAs_ = mkBtn("別名で保存…", "Ctrl+Shift+S : プロジェクトを別名で保存(以後は新しいファイルを編集。元のファイルは残る)", "b-saveas");
    bUndo_ = mkBtn("元に戻す", "Ctrl+Z", "b-undo");
    bRedo_ = mkBtn("やり直す", "Ctrl+Y", "b-redo");
    bReview_ = mkBtn("📝 修正点", "修正点の一覧(クリックでその所へ)と修正前の映像との比較", "b-review");
    bCheck_ = mkBtn("仕上がり確認", "実際の書き出しと同じ描画で現在のコマを確認", "b-check");
    bDraft_ = mkBtn("低画質で書き出し", QString(), "b-draft");
    bExport_ = mkBtn("書き出し", QString(), "primary");
    connect(bSaveAs_, &QPushButton::clicked, this, [this] { saveAs(); });
    connect(bReview_, &QPushButton::clicked, this, [this] { reviewToggle(); });
    connect(bCheck_, &QPushButton::clicked, this, [this] { checkFrame(); });
    connect(bDraft_, &QPushButton::clicked, this, [this] { startExport(true, QJsonObject(), false); });
    connect(bExport_, &QPushButton::clicked, this, [this] { exportDialog(); });
    connect(bUndo_, &QPushButton::clicked, this, [this] { doAction("undo"); });
    connect(bRedo_, &QPushButton::clicked, this, [this] { doAction("redo"); });
    for (auto* b : {bSaveAs_, bUndo_, bRedo_, bReview_, bCheck_, bDraft_, bExport_}) h->addWidget(b);
    return top;
}

QWidget* EditorWindow::buildPanel(bool left) {
    auto* p = new QWidget;
    p->setObjectName(left ? "left" : "right");
    p->setFixedWidth(left ? 270 : 310);
    auto* v = new QVBoxLayout(p);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* tabs = new QWidget;
    tabs->setObjectName("tabs");
    auto* th = new QHBoxLayout(tabs);
    th->setContentsMargins(0, 0, 0, 0);
    th->setSpacing(0);
    const QStringList names = left ? QStringList{"素材", "音声", "カット"} : QStringList{"クリップ", "スタイル", "ヒストリー", "設定"};
    QList<QPushButton*>& btns = left ? leftTabs_ : rightTabs_;
    for (int i = 0; i < names.size(); ++i) {
        auto* b = mkBtn(names[i]);
        if (!left && i == 2) b->setToolTip("操作の履歴(Premiere のヒストリー)。クリックでその時点に戻る");
        setOn(b, i == 0);
        btns.push_back(b);
        connect(b, &QPushButton::clicked, this, [this, left, i] { left ? setLeftTab(i) : setRightTab(i); });
        th->addWidget(b);
    }
    th->addStretch(1);
    v->addWidget(tabs);
    auto* sa = new QScrollArea;
    sa->setWidgetResizable(true);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->setFrameShape(QFrame::NoFrame);
    (left ? leftScroll_ : rightScroll_) = sa;
    v->addWidget(sa, 1);
    return p;
}

void EditorWindow::setBody(bool left, QWidget* w) {
    QScrollArea* sa = left ? leftScroll_ : rightScroll_;
    QWidget*& cur = left ? leftBody_ : rightBody_;
    w->setObjectName("tabbody");
    if (QWidget* old = sa->takeWidget()) { old->hide(); old->deleteLater(); }
    sa->setWidget(w);
    cur = w;
}

void EditorWindow::setLeftTab(int i) {
    leftTab_ = i;
    for (int j = 0; j < leftTabs_.size(); ++j) setOn(leftTabs_[j], j == i);
    fillLeft();
}
void EditorWindow::setRightTab(int i) {
    rightTab_ = i;
    for (int j = 0; j < rightTabs_.size(); ++j) setOn(rightTabs_[j], j == i);
    fillRight();
}

QWidget* EditorWindow::buildMonitor() {
    auto* mon = new QWidget;
    mon->setObjectName("monitor");
    auto* v = new QVBoxLayout(mon);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    stage_ = new StageHost(mon);
    v->addWidget(stage_, 1);
    auto* tr = new QWidget;
    tr->setObjectName("transport");
    tr->setFixedHeight(38);
    auto* h = new QHBoxLayout(tr);
    h->setContentsMargins(10, 0, 10, 0);
    h->setSpacing(8);
    auto* bStart = mkBtn("⏮", "Home");
    bPlay_ = mkBtn("▶", "Space");
    auto* bEnd = mkBtn("⏭", "End");
    connect(bStart, &QPushButton::clicked, this, [this] { doAction("home"); });
    connect(bPlay_, &QPushButton::clicked, this, [this] { doAction("play"); });
    connect(bEnd, &QPushButton::clicked, this, [this] { doAction("end"); });
    h->addWidget(bStart); h->addWidget(bPlay_); h->addWidget(bEnd);
    auto tcbox = [&](const QString& cap, QLabel** val, const char* name, QLabel** tot) {
        auto* f = new QFrame;
        f->setProperty("class", "tcbox");
        f->setObjectName(QString("box_") + name);
        auto* fh = new QHBoxLayout(f);
        fh->setContentsMargins(6, 1, 6, 1);
        fh->setSpacing(4);
        auto* c = new QLabel(cap);
        c->setProperty("class", "tcsmall");
        auto* x = new QLabel("0:00.00");
        x->setObjectName(name);
        x->setMinimumWidth(76);
        const bool isSrc = QString(name) == "tc";
        x->setToolTip(isSrc ? "元素材の時刻。クリックで入力してジャンプ" : "カット後(仕上がり)の時刻。クリックで入力してジャンプ");
        *val = x;
        fh->addWidget(c);
        fh->addWidget(x);
        if (tot) { auto* t = new QLabel; t->setProperty("class", "tcsmall"); *tot = t; fh->addWidget(t); }
        auto* cp = mkBtn("⧉", isSrc ? "元素材の時刻をコピー" : "カット後の時刻をコピー");
        cp->setProperty("class", "tccp");
        connect(cp, &QPushButton::clicked, this, [this, cp, isSrc] {
            copyToClipboard((isSrc ? tc_ : tcout_)->text());
            cp->setText("✓");
            QTimer::singleShot(900, cp, [cp] { cp->setText("⧉"); });
        });
        x->installEventFilter(this);
        fh->addWidget(cp);
        h->addWidget(f);
    };
    tcbox("元素材", &tc_, "tc", nullptr);
    tcbox("カット後", &tcout_, "tcout", &tctot_);
    h->addStretch(1);
    cProc_ = new QCheckBox("処理後の音で再生");
    cProc_->setChecked(true);
    cProc_->setFocusPolicy(Qt::NoFocus);
    h->addWidget(cProc_);
    bRaw_ = mkBtn("⚠ 元の音で再生中", "処理済みの音が古い/無いため、元の録音(コンプ・音量補正なし)で再生しています。書き出しとは音量が違います。クリックで音声処理を実行", "rawwarn");
    bRaw_->hide();
    h->addWidget(bRaw_);
    bMute_ = mkBtn("🔊", "M : ミュート");
    h->addWidget(bMute_);
    vol_ = new QSlider(Qt::Horizontal);
    vol_->setRange(0, 100);
    bool ok = false;
    const int vv = prefGet("vedit.vol", "80").toInt(&ok);
    vol_->setValue(ok ? std::clamp(vv, 0, 100) : 80);
    vol_->setToolTip("プレビュー音量(書き出しには影響しません)");
    vol_->setFixedWidth(90);
    vol_->setFocusPolicy(Qt::NoFocus);
    h->addWidget(vol_);
    connect(cProc_, &QCheckBox::toggled, this, [this](bool on) { updAudioWarn(); emit processedAudioChanged(on); });
    connect(bRaw_, &QPushButton::clicked, this, [this] {
        if (askYes(QStringLiteral("音声処理を実行して、書き出しと同じ音で再生できるようにしますか?(長い動画は10分ほどかかります)"))) runAudio();
    });
    connect(bMute_, &QPushButton::clicked, this, [this] { doAction("mute"); });
    connect(vol_, &QSlider::valueChanged, this, [this](int val) { prefSet("vedit.vol", QString::number(val)); applyVolume(); });
    v->addWidget(tr);
    return mon;
}

QWidget* EditorWindow::buildTimeline() {
    auto* tl = new QWidget;
    tl->setObjectName("tl");
    tl->setFixedHeight(335);
    auto* v = new QVBoxLayout(tl);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    auto* tools = new QWidget;
    tools->setObjectName("tltools");
    auto* h = new QHBoxLayout(tools);
    h->setContentsMargins(8, 5, 8, 5);
    h->setSpacing(6);
    tRazor_ = mkBtn("🔪 分割", "C : クリックで分割(Premiereのレーザー)");
    tSnap_ = mkBtn("🧲 スナップ", "S : スナップ(端や再生ヘッドに吸着)");
    tLink_ = mkBtn("🔗 リンク", "映像と音声のリンク(オフ=別々に選択・削除・音声だけ移動)。Alt+クリックで一時的に片方だけ選択");
    tCollapse_ = mkBtn("⇥ カット詰め", "カット部分を詰めて表示(Premiere同様)/ 元素材の時間軸で表示");
    tTool_ = mkBtn("✂ 範囲カット", "Shift+C : ドラッグでカット範囲を作る");
    auto* tIn = mkBtn("イン", "I");
    auto* tOut = mkBtn("アウト", "O");
    auto* tX = mkBtn("範囲をカット", "X : イン〜アウトをカット");
    auto* addT = mkBtn("+ テロップ", "T", "t-addt");
    auto* addB = mkBtn("+ トピック", "B", "t-addb");
    auto* addO = mkBtn("+ 差し込み", QString(), "t-addo");
    auto* addS = mkBtn("+ 効果音", "効果音(SE)を再生ヘッドの位置に置く", "t-adds");
    auto* ps = mkBtn("🎨 画像編集", "画像編集(軽量 Photoshop)で画像・テロップ画像を作る/直す(保存すると自動で反映)", "b-ps");
    connect(tRazor_, &QPushButton::clicked, this, [this] { doAction("razorTool"); });
    connect(tSnap_, &QPushButton::clicked, this, [this] { doAction("snapToggle"); });
    connect(tLink_, &QPushButton::clicked, this, [this] { doAction("linkToggle"); });
    connect(tCollapse_, &QPushButton::clicked, this, [this] { setCollapsed(!m_->collapsed); });
    connect(tTool_, &QPushButton::clicked, this, [this] { doAction("cutTool"); });
    connect(tIn, &QPushButton::clicked, this, [this] { doAction("markIn"); });
    connect(tOut, &QPushButton::clicked, this, [this] { doAction("markOut"); });
    connect(tX, &QPushButton::clicked, this, [this] { doAction("cutRange"); });
    connect(addT, &QPushButton::clicked, this, [this] { addTelop(); });
    connect(addB, &QPushButton::clicked, this, [this] { addTopic(); });
    connect(addO, &QPushButton::clicked, this, [this] { addOverlay(); });
    connect(addS, &QPushButton::clicked, this, [this] { addSfx(); });
    connect(ps, &QPushButton::clicked, this, [this, ps] { imageMenu(ps->mapToGlobal(QPoint(0, ps->height() + 2))); });
    for (auto* b : {tRazor_, tSnap_, tLink_, tCollapse_, tTool_, tIn, tOut, tX}) h->addWidget(b);
    h->addStretch(1);
    for (auto* b : {addT, addB, addO, addS, ps}) h->addWidget(b);
    h->addStretch(1);
    auto* zl = new QLabel("拡大");
    zl->setProperty("class", "dim");
    h->addWidget(zl);
    zoom_ = new QSlider(Qt::Horizontal);
    zoom_->setRange(0, 100);
    zoom_->setValue(40);
    zoom_->setFixedWidth(140);
    zoom_->setFocusPolicy(Qt::NoFocus);
    h->addWidget(zoom_);
    v->addWidget(tools);
    v->addSpacing(6);
    auto* row = new QWidget;
    row->setObjectName("tlrow");
    auto* rh = new QHBoxLayout(row);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(0);
    tl_ = new TimelineView(m_);
    tl_->setFocusPolicy(Qt::ClickFocus);
    rh->addWidget(tl_, 1);
    vscroll_ = new VScrollBar(tl_);
    rh->addWidget(vscroll_);
    meter_ = new MeterWidget;
    rh->addWidget(meter_);
    v->addWidget(row, 1);
    meter_->source = [this](double pk[2], double pw[2]) {
        if (levelSource) {   // 本番: AudioPlayer が実際に出している音(音量を掛けた後)。app.js と同じく音量分を差し引いた(書き出し相当の)値で出す
            double a[2], r[2];
            if (!levelSource(a, r)) return false;
            const double mv = muted_ ? 0.0 : volume() * volume();
            for (int c = 0; c < 2; ++c) {
                if (mv <= 0) { pk[c] = -200; pw[c] = 0; continue; }
                pk[c] = a[c] > 1e-6 ? 20 * std::log10(a[c] / mv) : -200;
                pw[c] = r[c] * r[c] / (mv * mv);
            }
            return true;
        }
        const double lv = tl_->levelAt(m_->tcur());   // 音声が無い時(テスト・書き出しだけ)は wave.json の値で近似
        if (lv < 0) return false;
        for (int c = 0; c < 2; ++c) { pk[c] = lv > 1e-6 ? 20 * std::log10(lv) : -200; pw[c] = lv * lv * 0.25; }
        return true;
    };
    tl_->confirm = [this](const QString& s) { return askYes(s); };
    tl_->askText = [this](const QString& t, const QString& i) { return askLine(t, i); };
    connect(tl_, &TimelineView::scrubStarted, this, [this] { stopForEdit(); });
    connect(tl_, &TimelineView::seekRequested, this, [this](double t) { seekTo(t); });
    connect(tl_, &TimelineView::seekOutRequested, this, [this](double o) { seekOutTo(o); });
    connect(tl_, &TimelineView::splitRequested, this, [this](double t) { m_->splitAll(t); });
    connect(tl_, &TimelineView::imageEditRequested, this, [this] { imageEditSel(); });
    connect(tl_, &TimelineView::photoshopRequested, this, [this](const QString& f) {
        const bridge::Cmd c = bridge::photoshopCmd(m_->resolve(f));
        lastImageCmd = c.line();
        if (!bridge::launch(c)) showToast(QStringLiteral("Photoshop: 起動できませんでした"), 5000);
    });
    connect(tl_, &TimelineView::bgmPanelRequested, this, [this] { setLeftTab(0); });
    connect(zoom_, &QSlider::valueChanged, this, [this](int val) {
        if (!inPreview_) tl_->setZoom(0.5 * std::pow(600.0, val / 100.0));
    });
    connect(tl_, &TimelineView::zoomChanged, this, [this](double z) {
        QSignalBlocker b(zoom_);
        zoom_->setValue((int)std::lround(100 * std::log(z / 0.5) / std::log(600.0)));
    });
    return tl;
}

void EditorWindow::setMonitor(QWidget* w) { stage_->setContent(w); }
void EditorWindow::setMonitorImage(const QImage& img) { stage_->setImage(img); }
void EditorWindow::setStageAspect(double w, double h) { stage_->setAspect(w, h); }
QImage EditorWindow::grabMonitor() { return stage_->grabStage(); }

void EditorWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (toast_ && toast_->isVisible()) showToast(lastToast_, 1);
}

bool EditorWindow::eventFilter(QObject* o, QEvent* e) {
    if (o == rvWin_ && e->type() == QEvent::Close && rvPopped_) {   // 別窓を閉じた = 修正点を閉じる(review.js の "closed")
        QTimer::singleShot(0, this, [this] {
            if (!rvPopped_) return;
            rvPopped_ = false;
            review_->setParent(centralWidget());
            review_->setFixedWidth(300);
            review_->hide();
            mainLay_->addWidget(review_);
            prefSet("vedit.review", "0");
            updateButtons();
            reviewDraw();
        });
        return false;
    }
    if (e->type() == QEvent::MouseButtonRelease && (o == tc_ || o == tcout_)) {
        const bool src = o == tc_;
        const QString s = askLine(src ? QStringLiteral("元素材の時刻へジャンプ(例 1:23.5)") : QStringLiteral("カット後の時刻へジャンプ(例 1:23.5)"), (src ? tc_ : tcout_)->text());
        if (!s.isEmpty()) {
            bool numeric = false;
            const double n = s.toDouble(&numeric);
            const double t = parseT(numeric ? OVal::num(n) : OVal::str(s));
            if (std::isfinite(t)) { if (src) seekTo(t); else seekOutTo(t); }
        }
        return true;
    }
    return QMainWindow::eventFilter(o, e);
}

// ---- 画面の更新 ----
void EditorWindow::refresh() {
    pname_->setText(m_->name());
    savestate_->setText(m_->saveState());
    bUndo_->setEnabled(true);
    updateButtons();
    updateTc();
    updateWarn(); updAudioWarn();
    if (!panelEditing_) {
        if (const QString s = panelSig(true); s.isEmpty() || s != leftSig_) fillLeft();
        if (const QString s = panelSig(false); s.isEmpty() || s != rightSig_) fillRight();
    }
    else if (leftBody_) {   // 入力中は欄を作り直さない。音声の注意書きだけ更新する(app.js commit)
        if (auto* an = leftBody_->findChild<QLabel*>("audio-note")) {
            an->setText(audioNoteText());
            an->setProperty("class", audioStale() ? "notewarn" : "note");
            an->style()->unpolish(an);
            an->style()->polish(an);
        }
    }
    if (panelEditing_ && rightTab_ == 2) fillRight();
    if (reviewShown() && !panelEditing_) reviewDraw();
    tl_->update();
}

void EditorWindow::updateButtons() {
    setOn(tRazor_, m_->tool == "razor");
    setOn(tTool_, m_->tool == "cut");
    setOn(tSnap_, m_->snapOn);
    setOn(tCollapse_, m_->collapsed);
    setOn(tLink_, m_->linked);
    tLink_->setText(m_->linked ? "🔗 リンク" : "⛓ リンク解除中");
    tLink_->setProperty("warnoff", !m_->linked);
    tLink_->style()->unpolish(tLink_);
    tLink_->style()->polish(tLink_);
    bPlay_->setText(playing_ ? "⏸" : "▶");
    if (bReview_) setOn(bReview_, reviewShown());
    if (bMute_) bMute_->setText(muted_ ? "🔇" : "🔊");
}

void EditorWindow::updateTc() {
    tc_->setText(fmtT(m_->tcur()));
    tcout_->setText(fmtT(m_->tout()));
    tctot_->setText(" / " + fmtT(m_->totalOut()));
}

void EditorWindow::updateWarn() {
    const OVal& d = m_->d();
    QString t;
    if (d.strOr("timebase", "source") != "source") t = QStringLiteral("このプロジェクトは timebase が output です。GUI は source 基準のみ対応です。");
    else if (!QFileInfo::exists(m_->resolve(d.strOr("source", "")))) t = QStringLiteral("素材の動画が見つかりません: ") + d.strOr("source", "");
    warn_->setText(t);
    warn_->setVisible(!t.isEmpty());
}

void EditorWindow::showToast(const QString& msg, int ms) {
    lastToast_ = msg;
    toast_->setText(msg);
    toast_->adjustSize();
    toast_->move((width() - toast_->width()) / 2, height() - 24 - toast_->height());
    toast_->show();
    toast_->raise();
    toastTimer_->start(ms);
}

// ---- 再生・シーク ----
// Q・W・シークバーをつまんだ時に一度止める(右のクリップ欄の「Q・W・シークで再生を止める」で切り替え)
void EditorWindow::stopForEdit() {
    if (prefGet("vedit.stopOnTrim", "1") == "0") return;
    if (shuttle_) setShuttle(0);
    if (playing_) emit playToggleRequested();
}

void EditorWindow::seekTo(double t, bool fromPreview) {
    const bool was = inPreview_;
    inPreview_ = fromPreview;
    m_->seek(t);
    if (fromPreview) tl_->ensureVisible(m_->tcur());
    inPreview_ = was;
    if (!fromPreview) emit previewSeek(m_->tout());
}

void EditorWindow::seekOutTo(double o, bool fromPreview) {
    const bool was = inPreview_;
    inPreview_ = fromPreview;
    m_->seekOut(o);
    if (fromPreview) tl_->ensureVisible(m_->tcur());
    inPreview_ = was;
    if (!fromPreview) emit previewSeek(m_->tout());
}

void EditorWindow::onPreviewTime(double outT) {
    if (!std::isfinite(outT)) return;
    seekOutTo(outT, true);
}

void EditorWindow::onPreviewPlaying(bool on) {
    playing_ = on;
    if (on && shuttle_ < 0) { shuttle_ = 0; if (shTimer_) shTimer_->stop(); }
    if (!on && rate_ != 1) { rate_ = 1; emit rateRequested(1.0); }
    meter_->setPlaying(on || shuttle_ < 0);
    updateButtons();
}

bool EditorWindow::copyToClipboard(const QString& text) {
    if (auto* cb = QGuiApplication::clipboard()) { cb->setText(text); return true; }
    return false;
}
QString EditorWindow::clipboardText() const {
    auto* cb = QGuiApplication::clipboard();
    return cb ? cb->text() : QString();
}

double EditorWindow::volume() const { return vol_ ? vol_->value() / 100.0 : 0.8; }
void EditorWindow::applyVolume() {
    const double v = volume();
    emit volumeChanged(muted_ ? 0.0 : v * v, muted_);
}
bool EditorWindow::useProcessed() const { return cProc_ && cProc_->isChecked(); }
bool EditorWindow::rawWarnVisible() const { return bRaw_ && !bRaw_->isHidden(); }

void EditorWindow::setShuttle(int dir) {
    if (!shTimer_) {
        shTimer_ = new QTimer(this);
        shTimer_->setInterval(33);
        connect(shTimer_, &QTimer::timeout, this, [this] {
            if (shuttle_ >= 0) { shTimer_->stop(); return; }
            double t = m_->tcur() + shuttle_ * 0.033;   // J: 逆再生(音なし。映像は間引いてシーク)
            std::pair<double, double> c;
            if (m_->cutAt(t, &c) && t > c.first) t = c.first - 0.001;
            if (t <= 0) { t = 0; shuttle_ = 0; shTimer_->stop(); meter_->setPlaying(false); }
            seekTo(t);
        });
    }
    if (dir < 0) {
        if (playing_) emit playToggleRequested();
        shuttle_ = shuttle_ < 0 ? std::max(-8, shuttle_ * 2) : -1;
        meter_->setPlaying(true);
        shTimer_->start();
    } else {
        shuttle_ = 0;
        shTimer_->stop();
        meter_->setPlaying(playing_);
    }
}

// ---- キーマップ ----
void EditorWindow::loadKeymap() {
    keymap_.clear();
    auto add = [&](const char* a, std::initializer_list<const char*> ks) { QStringList l; for (auto* k : ks) l << k; keymap_[a] = l; };
    add("play", {"Space"}); add("frameBack", {"Left"}); add("frameFwd", {"Right"}); add("back5", {"Shift+Left"}); add("fwd5", {"Shift+Right"});
    add("prevEdit", {"Up"}); add("nextEdit", {"Down"}); add("home", {"Home"}); add("end", {"End"});
    add("markIn", {"I"}); add("markOut", {"O"}); add("gotoIn", {"Shift+I"}); add("gotoOut", {"Shift+O"});
    add("extract", {";"}); add("cutRange", {"X"}); add("rippleTrimPrev", {"Q"}); add("rippleTrimNext", {"W"});
    add("split", {"Ctrl+K"}); add("razorTool", {"C"}); add("selectTool", {"V"}); add("cutTool", {"Shift+C"});
    add("delete", {"Delete"}); add("rippleDelete", {"Shift+Delete"});
    add("undo", {"Ctrl+Z"}); add("redo", {"Ctrl+Shift+Z", "Ctrl+Y"}); add("save", {"Ctrl+S"}); add("saveAs", {"Ctrl+Shift+S"});
    add("zoomOut", {"-"}); add("zoomIn", {"="}); add("zoomFit", {"\\"});
    add("shuttleBack", {"J"}); add("shuttleStop", {"K"}); add("shuttleFwd", {"L"});
    add("addTelop", {"T"}); add("addTopic", {"B"}); add("mute", {"Shift+M"});
    add("snapToggle", {"S"}); add("nudgeLeft", {"Alt+Left"}); add("nudgeRight", {"Alt+Right"});
    add("nudgeLeft5", {"Alt+Shift+Left"}); add("nudgeRight5", {"Alt+Shift+Right"});
    add("copy", {"Ctrl+C"}); add("paste", {"Ctrl+V"}); add("selectAll", {"Ctrl+A"}); add("deselectAll", {"Ctrl+Shift+A"}); add("linkToggle", {"Ctrl+L"});
    // %APPDATA%/vedit/keymap.json(Python 版 keys.load と同じ: 既定の上にユーザーの設定を重ねる)
    const QString ud = qEnvironmentVariable("APPDATA", QDir::homePath());
    QFile f(ud + "/vedit/keymap.json");
    if (qEnvironmentVariableIsSet("VEDIT_KEYMAP")) f.setFileName(qEnvironmentVariable("VEDIT_KEYMAP"));
    if (f.open(QIODevice::ReadOnly)) {
        OVal v;
        if (OVal::parse(f.readAll(), &v) && v.isObj())
            for (int i = 0; i < v.size(); ++i) {
                QStringList l;
                if (v.at(i).isArr()) for (const OVal& k : v.at(i).items()) l << k.asStr();
                keymap_[v.keyAt(i)] = l;
            }
    }
    keyIndex_.clear();
    for (auto it = keymap_.begin(); it != keymap_.end(); ++it)
        for (const QString& k : it.value()) keyIndex_[k.toLower()] = it.key();
    if (!keyIndex_.contains("ctrl+y")) keyIndex_["ctrl+y"] = "redo";
}

QString EditorWindow::comboOf(QKeyEvent* e) const {
    QString k;
    switch (e->key()) {
    case Qt::Key_Space: k = "Space"; break;
    case Qt::Key_Left: k = "Left"; break;
    case Qt::Key_Right: k = "Right"; break;
    case Qt::Key_Up: k = "Up"; break;
    case Qt::Key_Down: k = "Down"; break;
    case Qt::Key_Home: k = "Home"; break;
    case Qt::Key_End: k = "End"; break;
    case Qt::Key_Delete: k = "Delete"; break;
    case Qt::Key_Backspace: k = "Backspace"; break;
    case Qt::Key_Return: case Qt::Key_Enter: k = "Enter"; break;
    case Qt::Key_Escape: k = "Escape"; break;
    case Qt::Key_PageUp: k = "PageUp"; break;
    case Qt::Key_PageDown: k = "PageDown"; break;
    case Qt::Key_Tab: k = "Tab"; break;
    default:
        if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) k = QChar(e->key());
        else if (!e->text().isEmpty() && e->text()[0].unicode() >= 0x20) k = e->text().left(1).toUpper();
        else k = QKeySequence(e->key()).toString();
    }
    const bool letter = k.size() == 1 && k[0].isLetter();
    QString m;
    if (e->modifiers() & Qt::ControlModifier) m += "Ctrl+";
    if (e->modifiers() & Qt::AltModifier) m += "Alt+";
    if ((e->modifiers() & Qt::ShiftModifier) && (k.size() > 1 || letter)) m += "Shift+";
    return (m + k).toLower();
}

static bool textFocus(QWidget* w) {
    return qobject_cast<QLineEdit*>(w) || qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QTextEdit*>(w) || qobject_cast<QAbstractSpinBox*>(w) ||
           (qobject_cast<QComboBox*>(w) && static_cast<QComboBox*>(w)->isEditable());
}

bool EditorWindow::handleKey(QKeyEvent* e) {
    // 入力欄にフォーカスがある間はキー操作を奪わない(app.js: INPUT/TEXTAREA/SELECT は素通し。保存だけは効く)
    const QString combo = comboOf(e);
    const QString act = keyIndex_.value(combo);
    if (act.isEmpty()) return false;
    if (textFocus(QApplication::focusWidget()) && act != "save" && act != "saveAs") return false;
    doAction(act);
    return true;
}

void EditorWindow::keyPressEvent(QKeyEvent* e) {
    if (!handleKey(e)) QMainWindow::keyPressEvent(e);
}

void EditorWindow::setTool(const QString& t) {
    m_->tool = t;
    updateButtons();
}

void EditorWindow::setCollapsed(bool v) {
    m_->collapsed = v;
    updateButtons();
    emit m_->changed();
    tl_->ensureVisible(m_->tcur());
}

void EditorWindow::doAction(const QString& a) {
    const double fps = m_->fps() > 0 ? m_->fps() : 30;
    auto cur = [&] { return m_->tcur(); };
    if (a == "play") {
        if (rate_ != 1) { rate_ = 1; emit rateRequested(1.0); }
        if (shuttle_) { setShuttle(0); seekTo(cur()); }
        else emit playToggleRequested();
    }
    else if (a == "frameBack") seekTo(cur() - 1 / fps);
    else if (a == "frameFwd") seekTo(cur() + 1 / fps);
    else if (a == "back5") seekTo(cur() - 5 / fps);
    else if (a == "fwd5") seekTo(cur() + 5 / fps);
    else if (a == "prevEdit") {
        auto pts = m_->edgePoints();
        for (auto it = pts.rbegin(); it != pts.rend(); ++it) if (*it < cur() - 0.02) { seekTo(*it); break; }
    } else if (a == "nextEdit") {
        for (double x : m_->edgePoints()) if (x > cur() + 0.02) { seekTo(x); break; }
    } else if (a == "home") seekTo(0);
    else if (a == "end") seekTo(m_->dur());
    else if (a == "markIn") { m_->markIn = cur(); m_->markOut = -1; tl_->update(); }
    else if (a == "markOut") { if (m_->markIn >= 0) { m_->markOut = cur(); tl_->update(); } }
    else if (a == "gotoIn") { if (m_->markIn >= 0) seekTo(m_->markIn); }
    else if (a == "gotoOut") { if (m_->markOut >= 0) seekTo(m_->markOut); }
    else if (a == "extract") { if (m_->markIn >= 0 && m_->markOut >= 0) m_->doMarkX(); else m_->editDelete(true); }
    else if (a == "cutRange") m_->doMarkX();
    else if (a == "rippleTrimPrev" || a == "rippleTrimNext") {
        stopForEdit();
        m_->trimTo(a == "rippleTrimPrev" ? -1 : 1);
    }
    else if (a == "split") m_->splitAll(cur());
    else if (a == "razorTool") setTool(m_->tool == "razor" ? "select" : "razor");
    else if (a == "selectTool") setTool("select");
    else if (a == "cutTool") setTool(m_->tool == "cut" ? "select" : "cut");
    else if (a == "delete") { if (!m_->editDelete(false) && m_->markIn >= 0 && m_->markOut >= 0) m_->doMarkX(); }
    else if (a == "rippleDelete") m_->editDelete(true);
    else if (a == "undo") m_->undo();
    else if (a == "redo") m_->redo();
    else if (a == "save") save();
    else if (a == "saveAs") saveAs();
    else if (a == "zoomOut") tl_->setZoom(tl_->zoom() / 1.5);
    else if (a == "zoomIn") tl_->setZoom(tl_->zoom() * 1.5);
    else if (a == "zoomFit") tl_->zoomFit();
    else if (a == "shuttleFwd") {
        if (shuttle_ < 0) { setShuttle(0); seekTo(cur()); }
        if (!playing_) { rate_ = 1; emit playToggleRequested(); emit rateRequested(rate_); }
        else { rate_ = std::min(8.0, rate_ * 2); emit rateRequested(rate_); }
    }
    else if (a == "shuttleBack") setShuttle(-1);
    else if (a == "shuttleStop") {
        setShuttle(0);
        if (playing_) emit playToggleRequested();
        rate_ = 1;
        emit rateRequested(rate_);
        seekTo(cur());
    }
    else if (a == "snapToggle") { m_->snapOn = !m_->snapOn; updateButtons(); showToast(QString("スナップ ") + (m_->snapOn ? "オン" : "オフ"), 900); }
    else if (a == "nudgeLeft") m_->nudge(-1);
    else if (a == "nudgeRight") m_->nudge(1);
    else if (a == "nudgeLeft5") m_->nudge(-5);
    else if (a == "nudgeRight5") m_->nudge(5);
    else if (a == "copy") m_->copySel();
    else if (a == "paste") m_->pasteClip();
    else if (a == "selectAll") m_->selectAll();
    else if (a == "deselectAll") m_->clearSel();
    else if (a == "linkToggle") { m_->linked = !m_->linked; prefSet("vedit.linked", m_->linked ? "1" : "0"); updateButtons(); tl_->update(); }
    else if (a == "addTelop") addTelop();
    else if (a == "addTopic") addTopic();
    else if (a == "mute") { muted_ = !muted_; updateButtons(); applyVolume(); }
}

void EditorWindow::save() {
    if (m_->save(false) == EditModel::Failed) showToast(m_->saveState());
}

// 閉じてよいか。未保存の編集があれば保存するか聞く。保存できなかった時は閉じない
bool EditorWindow::confirmClose() {
    if (!m_->dirty()) return true;
    const auto b = QMessageBox::question(this, "V-edit", QStringLiteral("保存していない編集があります。保存しますか?"),
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (b == QMessageBox::Cancel) return false;
    if (b == QMessageBox::Discard) return true;
    if (m_->save(false) != EditModel::Failed) return !m_->dirty();   // 競合の時は上書き/読み込みを選んだ後
    const QString bak = m_->writeBackup();
    QMessageBox::warning(this, "V-edit", m_->saveState() + (bak.isEmpty() ? QStringLiteral("\n別のファイルにも書けませんでした。") : QStringLiteral("\n編集は次のファイルに残しました:\n") + bak) + QStringLiteral("\n閉じずに残します。"));
    return false;
}

void EditorWindow::closeEvent(QCloseEvent* e) {
    if (!confirmClose()) { e->ignore(); return; }
    if (rvWin_) { rvPopped_ = false; rvWin_->close(); }
    e->accept();
}

// ---- 追加(ボタンと T / B キー) ----
void EditorWindow::addTelop() {
    m_->addTelop();
    if (rightTab_ != 0 && rightTab_ != 1) setRightTab(0);
}
void EditorWindow::addTopic() {
    m_->addTopic();
}
void EditorWindow::addOverlay() {
    const QString f = pickPath("media");
    if (f.isEmpty()) return;
    m_->addOverlay(f);
}
void EditorWindow::addSfx() {
    const QString f = pickPath("audio");
    if (f.isEmpty()) return;
    const vn::MediaProps mp = vn::probeMedia(f);
    m_->addSfx(f, mp.ok && mp.duration > 0 ? mp.duration : 0);
}

}  // namespace vui
