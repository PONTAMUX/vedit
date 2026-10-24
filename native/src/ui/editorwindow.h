// 編集画面(index.html + style.css の見た目)。モニターは外から差し込む(本番は PreviewWidget、テストは静止画)
#pragma once
#include <QHash>
#include <QLabel>
#include <QJsonObject>
#include <QMainWindow>
#include <functional>
#include "editmodel.h"
#include "pybridge.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QSlider;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class QFileSystemWatcher;
class QHBoxLayout;

namespace vui {

class TimelineView;
class StageHost;
class VScrollBar;
class MeterWidget;
class ModalOverlay;
class ReviewPlayer;

class EditorWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit EditorWindow(EditModel* m, QWidget* parent = nullptr);
    ~EditorWindow() override;
    void setMonitor(QWidget* w);               // 本番: PreviewWidget。無ければ黒い画面
    void setMonitorImage(const QImage& img);   // テスト/スクリーンショット用の静止画
    void setStageAspect(double w, double h);
    EditModel* model() { return m_; }
    TimelineView* timeline() { return tl_; }
    bool handleKey(QKeyEvent* e);              // キー → アクション
    void doAction(const QString& act);         // keymap のアクション名
    QString comboOf(QKeyEvent* e) const;
    QString actionForKey(const QString& combo) const { return keyIndex_.value(combo.toLower()); }
    void loadKeymap();
    QHash<QString, QStringList> keymap() const { return keymap_; }
    // 上書き/取り込みの確認(既定は QMessageBox)。true = 上書き
    std::function<bool()> askOverwrite;
    QString lastToast() const { return lastToast_; }
    bool playing() const { return playing_; }
    void showToast(const QString& msg, int ms = 2600);

    // ---- 第2部: 外とのやりとり(テストは差し替える) ----
    std::function<QString(const QString& kind)> pickFile;                        // kind: media / audio / vst3。選んだパス(キャンセルは空)
    std::function<QString(const QString& defName, const QString& dir)> pickSave;   // 書き出し先・別名保存の保存先
    std::function<bool(const QString&)> confirm;                                 // はい/いいえ(既定は QMessageBox)
    std::function<QString(const QString& title, const QString& init)> askText;   // 1行入力(既定は QInputDialog)
    std::function<QStringList()> fontList;                                       // フォント名の一覧(既定はレジストリ)
    std::function<QString(const QString& cur)> pickColor;                       // 色の選択(既定は QColorDialog)。選んだ #rrggbb(キャンセルは空)
    bool copyToClipboard(const QString& text);
    QString clipboardText() const;

    // ---- パネル ----
    int leftTab() const { return leftTab_; }
    int rightTab() const { return rightTab_; }
    void setLeftTab(int i);
    void setRightTab(int i);
    QWidget* leftBody() const { return leftBody_; }
    QWidget* rightBody() const { return rightBody_; }
    QString styleTarget() const { return styleTarget_; }
    void setStyleTarget(const QString& n) { styleTarget_ = n; fillRight(); }
    bool audioStale() const;
    QString audioNoteText() const;
    QJsonObject silenceResult() const { return silRes_; }

    // ---- 追加・編集 ----
    void addTelop();
    void addTopic();
    void addOverlay();                       // pickFile("media")
    void addSfx();                           // pickFile("audio")
    void imageNew(const QString& kind);      // image / telop / frame
    void imageEditSel();
    void imageMenu(const QPoint& global);
    QString lastImageCmd;                    // 直近に組み立てた画像編集のコマンドライン(検査用)

    // ---- 保存・書き出し ----
    void saveAs();
    void exportDialog();
    void startExport(bool draft, const QJsonObject& range, bool openAfter);
    void checkFrame();
    void runAudio();
    void runSilence();
    void reviewToggle();
    bool reviewShown() const;
    void reviewRefresh();
    void reviewOp(const QJsonObject& op);
    QWidget* reviewPanel() const { return review_; }
    ReviewPlayer* reviewPlayer() const { return rvPlayer_; }
    bool reviewPopped() const { return rvPopped_; }
    QWidget* reviewWindow() const { return rvWin_; }
    void reviewPopout();                       // 別窓に出す(review.js popout)
    void reviewDock();                         // 右に戻す
    void reviewTick();                         // 再生ヘッドに合わせて修正前の動画を動かす(80ms ごと・テストからも呼べる)
    QJsonObject reviewData() const { return reviewState_; }
    bool reviewBusy() const { return reviewPending_ > 0; }
    bool exporting() const { return exporting_; }
    ModalOverlay* modal() const { return modal_; }
    bridge::Job* exportJob() const { return exportJob_; }
    bridge::Job* audioJob() const { return audioJob_; }
    bool checkVisible() const;
    QImage checkImage() const;
    QString lastExportOutput;                // 直近に書き出した先(検査用)
    QJsonObject lastExportResult;
    QString lastError;
    bridge::SysMon* sysmon() const { return sys_; }
    QString sysText() const;
    MeterWidget* meter() const { return meter_; }
    // 実際に出している音の L/R の peak と RMS(線形振幅。音量を掛けた後)を返す。false = 取れない。未設定なら wave.json から近似
    std::function<bool(double peak[2], double rms[2])> levelSource;
    VScrollBar* vscroll() const { return vscroll_; }
    double volume() const;                   // 0..1(2乗前)
    bool muted() const { return muted_; }
    bool useProcessed() const;
    QString projectStatePath() const;
    bool rawWarnVisible() const;
    int shuttleRate() const { return shuttle_; }
    void tickSys() { if (sys_) sys_->tick(); }
    void setWaveJobEnabled(bool on) { waveJobOn_ = on; }
    QImage grabMonitor();                    // モニターに写っている絵(中身+重ね表示)
    QPushButton* rawWarnButton() const { return bRaw_; }
    QWidget* warnBar() const { return warn_; }
    double rate() const { return rate_; }

public slots:
    void onPreviewTime(double outT);           // プレビューが進んだ(出力時刻)
    void onPreviewPlaying(bool on);
    void save();
    bool confirmClose();   // 未保存なら保存するか聞く。閉じてよければ true

signals:
    void previewSeek(double outT);             // 止まっている時のシーク先(出力時刻)
    void playToggleRequested();
    void contentChanged();                     // 編集内容が変わった(プレビュー作り直しの合図)
    void volumeChanged(double gain, bool muted);
    void processedAudioChanged(bool on);
    void projectSwitched(const QString& path);   // 別名で保存した
    void rateRequested(double rate);             // L を重ねて押した時の倍速(プレビュー側が対応していれば使う)

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    void build();
    QWidget* buildTop();
    QWidget* buildPanel(bool left);
    QWidget* buildMonitor();
    QWidget* buildTimeline();
    void refresh();
    void stopForEdit();
    void seekTo(double srcT, bool fromPreview = false);
    void seekOutTo(double outT, bool fromPreview = false);   // 表示上の秒でシーク(空白の中でも位置を保つ)
    void updateTc();
    void updateButtons();
    void setTool(const QString& t);
    void setCollapsed(bool v);
    void fillLeft();
    void fillRight();
    void mediaTab(QVBoxLayout* v);
    void audioTab(QVBoxLayout* v);
    void cutTab(QVBoxLayout* v);
    void clipTab(QVBoxLayout* v);
    void styleTab(QVBoxLayout* v);
    void historyTab(QVBoxLayout* v);
    void settingsTab(QVBoxLayout* v);
    void setBody(bool left, QWidget* w);
    void updateWarn();
    void updAudioWarn();
    void startVolume();
    void applyVolume();
    void setShuttle(int dir);
    void saveAsAndSwitch(const QString& dst);
    void showModal(const QList<QWidget*>& w);
    void closeModal();
    void ensureWave();
    void jobFail(const QString& title, const QString& msg);
    void exportDone(bool draft, bool openAfter, const QJsonObject& r);
    bool askYes(const QString& msg);
    QString askLine(const QString& title, const QString& init);
    QStringList fonts();
    QString pickPath(const QString& kind);
    QStringList styleNames() const;
    OVal effStyle(const QString& name) const;
    void editStyle(const QString& name, const std::function<void(OVal&)>& fn);
    void buildReview(QVBoxLayout* v);
    void reviewDraw();
    // フォーム部品(app.js の bind / fNum など)
    friend struct PanelForm;

    EditModel* m_;
    TimelineView* tl_ = nullptr;
    StageHost* stage_ = nullptr;
    QLabel *savestate_ = nullptr, *pname_ = nullptr, *tc_ = nullptr, *tcout_ = nullptr, *tctot_ = nullptr, *toast_ = nullptr, *sysLabel_ = nullptr, *warn_ = nullptr;
    QPushButton *bUndo_ = nullptr, *bRedo_ = nullptr, *bPlay_ = nullptr, *tRazor_ = nullptr, *tSnap_ = nullptr, *tLink_ = nullptr, *tCollapse_ = nullptr, *tTool_ = nullptr;
    QPushButton *bReview_ = nullptr, *bRaw_ = nullptr, *bMute_ = nullptr, *bExport_ = nullptr, *bDraft_ = nullptr, *bCheck_ = nullptr, *bSaveAs_ = nullptr;
    QSlider *zoom_ = nullptr, *vol_ = nullptr;
    QCheckBox* cProc_ = nullptr;
    QWidget *leftBody_ = nullptr, *rightBody_ = nullptr;
    QScrollArea *leftScroll_ = nullptr, *rightScroll_ = nullptr;
    QList<QPushButton*> leftTabs_, rightTabs_;
    QTimer* toastTimer_ = nullptr;
    QString lastToast_;
    QHash<QString, QStringList> keymap_;
    QHash<QString, QString> keyIndex_;
    bool playing_ = false;
    bool inPreview_ = false;
    int leftTab_ = 0, rightTab_ = 0;
    // 第2部
    VScrollBar* vscroll_ = nullptr;
    MeterWidget* meter_ = nullptr;
    ModalOverlay* modal_ = nullptr;
    QWidget* review_ = nullptr;
    QHBoxLayout* mainLay_ = nullptr;     // 左・モニター・右・修正点の4列
    ReviewPlayer* rvPlayer_ = nullptr;   // 修正前の動画(画面の作り直しをまたいで同じものを使い回す)
    QTimer* rvTimer_ = nullptr;
    QWidget* rvWin_ = nullptr;           // 別窓
    bool rvPopped_ = false;
    bool dying_ = false;   // 破棄中(子が消える途中に来る更新を無視する)
    QSet<QString> pvAsked_;              // 軽い版を作らせ始めた動画
    QVBoxLayout* reviewLay_ = nullptr;
    QJsonObject reviewState_;
    int reviewRound_ = -1;
    bridge::SysMon* sys_ = nullptr;
    QTimer* sysTimer_ = nullptr;
    QString styleTarget_ = QStringLiteral("telop");
    // 左右の欄に出ている中身の印。編集のたびの refresh で、印が同じなら欄を作り直さない(作り直しは 1 回 10ms ほど)
    QString leftSig_, rightSig_;
    QString panelSig(bool left) const;
    bool panelEditing_ = false;        // 入力欄を触っている間は右/左の欄を作り直さない(app.js commit({inspector:true}))
    bool audioBusy_ = false, silBusy_ = false, exporting_ = false, waveJobOn_ = true;
    struct Sil { double min = 0.7, noise = -40, kb = 0.2, ka = 0.35; } sil_;
    QJsonObject silRes_;
    bool silHave_ = false;
    bridge::Job *audioJob_ = nullptr, *exportJob_ = nullptr, *silJob_ = nullptr, *frameJob_ = nullptr, *waveJob_ = nullptr;
    QStringList fonts_;
    bool fontsLoaded_ = false;
    bool muted_ = false;
    int shuttle_ = 0;
    QJsonObject imgPending_;
    bool waveAsked_ = false;
    double rate_ = 1;
    QTimer* shTimer_ = nullptr;
    QString pendingStyleNew_;
    int reviewPending_ = 0;
    QFileSystemWatcher* imgWatch_ = nullptr;
    void watchImage(const QString& abs);
};

}  // namespace vui
