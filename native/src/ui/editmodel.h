// 編集の中身(app.js の状態・時刻計算・編集操作・保存の移植)。画面を持たない(Qt Core のみ)ので offscreen テストから直接操作できる
// 保存は Python 版(GUI → /api/save → Project.save)と同じバイト列: キー順そのまま・indent=2・CRLF・整数値は整数
#pragma once
#include <QHash>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <map>
#include <vector>
#include "ojson.h"

namespace vui {

using Rg = std::vector<std::pair<double, double>>;

struct Piece {           // 出力順の並び(app.js pieces())。gap=true なら空白(長さ len, 位置 at)
    bool gap = false;
    double s = 0, e = 0;  // 素材の区間(gap でない時)
    double at = 0, len = 0;
    double o = 0;         // 出力上の開始
    double span() const { return gap ? len : e - s; }
};

struct SegSel { double a, b; QChar trk; };   // trk: 0=映像+音声(リンク) / 'v' / 'a'

struct LaneDef { QString id, name; int h; QString kind; int vn; };   // app.js LANES

class EditModel : public QObject {
    Q_OBJECT
public:
    void invalidateCache() { invalidate(); }
    explicit EditModel(QObject* parent = nullptr);

    bool open(const QString& path, QString* err);   // 読み込み + normalize(app.js load)
    bool reloadFromDisk(QString* err);             // 外部の変更を取り込む(履歴は捨てる)
    const QString& path() const { return path_; }
    QString name() const;                           // 拡張子(.vedit.json / .json)を除いた名前
    QString dir() const;
    OVal& d() { return p_; }
    const OVal& d() const { return p_; }
    double dur() const { return dur_; }
    double fps() const { return fps_; }
    QString resolve(const QString& rel) const;      // プロジェクトからの相対 → 絶対
    QString srcFile() const { return p_.strOr("source", ""); }

    // ---- 時刻の計算(app.js の「カット計算」) ----
    const Rg& mergedCuts();
    const Rg& gapsN();
    const std::vector<Piece>& pieces();
    double outTime(double t);
    double outEnd(double t);
    double totalOut();
    double inDelta(const OVal& it, double a, double b);
    bool cutAt(double t, std::pair<double, double>* out = nullptr);
    std::vector<std::pair<double, double>> segments();   // V1/A1 の区間(分割点・カット・空白で区切る)
    double srcOf(double o, bool collapsed);              // 表示上の秒 → 元素材の時刻
    int pieceIdx(double o);
    const Piece* gapPieceAt(double o);
    static bool isGapIt(const OVal& it) { const OVal* g = it.get("gap_at"); return g && !g->isNull(); }
    double gapKey(double at0);
    bool gapOf(const OVal& it, double* at, double* d, double* o);
    // 画面上の左右(秒)。itX は collapsed なら出力時刻
    void itRange(const OVal& it, bool collapsed, double* x0, double* x1);

    // ---- トラック ----
    int vCount() const;
    QString vName(int n) const;
    std::vector<LaneDef> lanes() const;
    static int trackOf(const OVal& it);
    QString laneOf(const QString& kind, const OVal& it) const;
    bool isHidden(const QString& id) const { return arrHas(p_.get("hide"), id); }
    bool isMuted(const QString& id) const { return arrHas(p_.get("mute"), id); }
    bool isLocked(const QString& id) const { return locks_.contains(id); }
    void setLocks(const QStringList& ids, bool on);
    void toggleLock(const QString& id, bool all);
    void toggleHide(const QString& id, bool all);
    void toggleMute(const QString& id, bool all);
    QStringList sameSide(const QString& id) const;
    static bool isAudioLane(const QString& id) { return id == "a1" || id == "se" || id == "bgm"; }

    // ---- 再生ヘッド・イン/アウト ----
    double tcur() const { return tcur_; }
    void seek(double t);
    // 表示上の秒(カット後)で再生ヘッドを置く。空白の中は元素材の時刻が1点(空白の位置)になり、outTime では空白の後ろへ飛んでしまうため、
    // 表示上の秒も覚えておく(10/20 空白の章カードを再生すると再生ヘッドが空白の右端へ飛んでいた)
    void seekOut(double o);
    double tout();   // 再生ヘッドの表示上の秒
    double markIn = -1, markOut = -1;   // <0 = 未設定
    bool snapOn = true, linked = true, collapsed = true;
    QString tool = "select";            // select / razor / cut

    // ---- 選択 ----
    QSet<QString> selSet;               // "kind:i"
    QString selKind; int selI = -1;     // 最後に選んだもの(cuts も)
    std::vector<SegSel> segSel;
    std::vector<double> gapSel;
    bool bgmSel = false;
    QStringList trkSel;                 // "vmute:i" 等
    void select(const QString& kind, int i);   // kind 空 = 選択解除
    void toggleSel(const QString& kind, int i);
    void clearSel();                    // deselectAll
    void selectAll();
    bool hasAnySel() const;

    // ---- 編集 ----
    void snap();
    void commit(bool keepOverlap = false);
    void mutate(const std::function<void()>& fn);
    bool undo();
    bool redo();
    bool canUndo() const { return !undoS_.empty(); }
    bool canRedo() const { return !redoS_.empty(); }
    int undoCount() const { return (int)undoS_.size(); }
    bool splitAll(double t);
    bool editDelete(bool ripple);
    void cutRange(double a, double b);
    void trimTo(int dir);
    void nudge(int frames);
    bool doMarkX();                     // X: イン〜アウトをカット
    std::vector<double> edgePoints();
    std::vector<double> trimPoints();
    void copySel();
    void pasteClip();

    // ---- 保存 ----
    bool dirty() const { return dirty_; }
    QString saveState() const { return saveState_; }
    OVal projOut() const;               // 保存する内容(効果音の自動 end を省く)
    QByteArray serialize() const;       // 保存するバイト列(CRLF・末尾改行なし)
    enum SaveResult { Saved, Conflict, Failed };
    SaveResult save(bool force = false);   // Conflict の時は何も書かない
    QString writeBackup() const;           // 今の内容を <名前>.local-bak.json に書く(書けたらそのパス)
    void setBackupMinutes(int min) { if (min > 0) backupTimer_.start(min * 60 * 1000); else backupTimer_.stop(); }   // 分ごとに <名前>.backup/ へ写しを残す(未保存の分も)。0 で止める
    QString writeTimedBackup();            // 前の写しから変わっていれば1つ書く(書いたらそのパス)
    void startAutosave(bool on) { autosaveOn_ = on; }
    void startWatch(int ms = 2000);     // 外部変更の監視(dirty でなく playing でない時だけ)
    std::function<bool()> isPlaying;    // 監視の抑止用
    bool checkExternal();               // 外部が書き換えていたら取り込んで true
    double lastMtime() const { return lastMtime_; }
    QString histBase = QStringLiteral("開く");

    // ---- 第2部: 項目の追加・削除・トラック操作・ドラッグの計算・ヒストリー・保存先 ----
    int vTarget = 2;                                  // 差し込みの追加先トラック
    static QString locksFile();                       // %APPDATA%/vedit/locks.json(環境変数 VEDIT_LOCKS で差し替え可)
    QString lockKey() const { return "vedit.locks:" + dir() + "/" + name(); }
    void loadLocks();
    void saveLocks() const;
    QString relPath(const QString& p) const;          // プロジェクトの中なら相対、外なら絶対(app.js relPath)
    int addItem(const QString& kind, const OVal& item);   // 追加して並べ替え、選ぶ。追加後の添字
    int addTelop();
    int addTopic();
    int addOverlay(const QString& file);
    int addSfx(const QString& file, double duration);
    void deleteSel();
    void shiftVTracks(int n, int d);
    OVal* vtList();
    void addVTrack(int n);
    int overlaysOn(int n) const;
    bool deleteVTrack(int n);                         // 最後の1本は消さない。差し込みも一緒に消える(確認は呼ぶ側)
    bool deleteEmptyVTracks();
    void renameVTrack(int n, const QString& name);
    bool moveToTrack(int d);                          // 選択中の差し込みを d 本上/下へ
    void remapVTracks(const std::map<int, int>& map);
    void moveVTrack(int vn, int slot);
    struct MovePlan { Rg cuts; Rg gaps; bool ok = false; };
    MovePlan planMove(double a, double b, double dx, double snapTol);   // snapTol<=0 で吸着なし(秒)
    struct HistRow { int idx; QString label; bool cur, fut; };
    std::vector<HistRow> histRows();
    bool histJump(int k);
    static QString histDescribe(const OVal& a, const OVal& b);
    bool saveAs(const QString& newPath, QString* err);   // 内容を新しい場所へ書き、以後そちらを編集する
    double seekSnapT(double t, double pxPerSec, bool force);   // シーク吸着(区切り線へ)
    std::vector<double> seekPts();

    // 効果音の長さ(秒)。probe 結果をキャッシュ
    double sfxFull(const OVal& it) const;
    double sfxLen(const OVal& it) const;     // 音の長さ - 頭を飛ばす分
    bool sfxAuto(const OVal& it) const;     // 終了が「音の長さ」に合っている(自動)
    int redoCount() const { return (int)redoS_.size(); }

signals:
    void changed();                     // 内容が変わった(タイムライン・右パネルの作り直し)
    void selectionChanged();
    void playheadChanged(double t);
    void toast(const QString& msg);
    void externalLoaded();
    void saveStateChanged(const QString& s);
    void conflict();                    // 保存時に外部の書き換えを見つけた(画面側が上書き/取り込みを聞く)

private:
    static bool arrHas(const OVal* a, const QString& id);
    void normalize();
    void ensureTemplate();
    void invalidate();
    void resolveOverlaps();
    void normGaps();
    void setSaveState(const QString& s);
    bool loadFile(QString* err);
    OVal readParsed(QString* err, double* mtime) const;
    void doUndo(std::vector<QString>& from, std::vector<QString>& to);
    void setTrackFlag(const QString& key, const QStringList& ids, bool on);
    void sortList(const QString& k);

    QString path_;
    OVal p_;
    OVal prevVal_;      // 最後に snap() した時の p_(undoS_.back() と同じ中身)
    QString prevStr_;
    double dur_ = 60, fps_ = 30;
    double tcur_ = 0;
    double outHint_ = -1;
    bool dirty_ = false;
    double lastMtime_ = 0;
    QString saveState_ = QStringLiteral("保存済み");
    std::vector<QString> undoS_, redoS_;
    QSet<QString> locks_;
    QTimer saveTimer_, watchTimer_, backupTimer_;
    QByteArray lastBackup_;
    bool autosaveOn_ = false;   // 保存は Ctrl+S(2026-10-23 ユーザー指定で自動保存をやめた)
    mutable QHash<QString, double> sfxDur_;
    std::vector<std::pair<QString, OVal>> clip_;   // コピー(kind, item)

    // 計算結果のキャッシュ(app.js の cutsCache)
    struct Cache {
        bool mcOk = false, gapsOk = false, piecesOk = false, totOk = false, mcPreOk = false, gapPreOk = false;
        Rg mc, gaps;
        std::vector<double> mcPre, gapPre;
        std::vector<Piece> pieces;
        double tot = 0;
    } c_;
};

// 画面の設定(Web 版の localStorage 相当)。%APPDATA%/vedit/native.json(環境変数 VEDIT_PREFS で差し替え)
QString prefGet(const QString& key, const QString& def = QString());
void prefSet(const QString& key, const QString& val);

QString fmtT(double t, bool cs = true);     // app.js fmtT(0:00.00)
double parseT(const OVal& v);               // 数 or "1:23.5"
double clampd(double v, double a, double b);

}  // namespace vui
