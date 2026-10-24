// 外部ツール呼び出し(書き出し・コマ確認・無音検出・修正点・画像編集・音声処理・システム使用率)
// 子プロセスは必ず CREATE_NO_WINDOW。コマンドラインの組み立てと実行を分けてあり、テストは組み立てだけを検査できる
#pragma once
#include <QJsonObject>
#include <QObject>
#include <QProcessEnvironment>
#include <QStringList>
#include <functional>

class QProcess;

namespace vui::bridge {

struct Cmd {
    QString prog;
    QStringList args;
    QString workDir;
    QString line() const;          // 表示・検査用
};

// 場所(環境変数で差し替え可)
QString pythonExe();               // VEDIT_PYTHON / 既定 "python"
QString veditRoot();               // VEDIT_ROOT / 既定 C:/vedit(vedit_main.py のある所)
QString vaudioExe();               // VEDIT_VAUDIO / 既定 <root>/vaudio/dist/vaudio.exe
QString vimgExe();                 // VEDIT_VIMG / 既定 <root>/vimg-native/dist/vimg.exe
QString photoshopExe();            // Program Files/Adobe/Adobe Photoshop*/Photoshop.exe の新しいもの

// コマンドの組み立て
Cmd jobCmd(const QString& kind, const QString& proj, const QJsonObject& args);   // kind: render / frame / silence / review / vimg
Cmd audioCmd(const QString& proj, bool force);
Cmd vimgOpenCmd(const QString& file);          // vimg.exe [file]
Cmd photoshopCmd(const QString& file);
Cmd explorerCmd(const QString& path);          // 書き出し先のフォルダを開く(ファイルは選択状態で)

// 起動(テストは差し替えて起動しない)。子プロセスを切り離して起動する
extern std::function<bool(const Cmd&)> launcher;
bool launch(const Cmd& c);

// 同期実行(短い処理用)
struct Result { bool ok = false; int code = -1; QByteArray out, err; QJsonObject obj; QString text; QString error; };
Result runSync(const Cmd& c, int timeoutMs = 120000);

// 非同期ジョブ。python 側は P<進捗>/R<json>/E<メッセージ> を1行ずつ標準出力に書く。vaudio は標準エラーの "[ 42%]" を進捗として読む
class Job : public QObject {
    Q_OBJECT
public:
    explicit Job(const Cmd& c, bool isAudio = false, QObject* parent = nullptr);
    ~Job() override;
    void start();
    bool running() const;
    void cancel();
signals:
    void progress(double p, const QString& msg);
    void finished(bool ok, const QJsonObject& result, const QString& error);
private:
    void readOut();
    void readErr();
    void onFinished(int code);
    Cmd c_;
    bool audio_;
    QProcess* p_ = nullptr;
    QByteArray bufOut_, bufErr_, all_;
    QJsonObject result_;
    QString error_, errTail_;
    bool gotResult_ = false, gotError_ = false, cancelled_ = false;
};

// CPU / GPU / VRAM / メモリ(sysmon.py 相当)。-1 = 取れない
struct SysStats { double cpu = -1, gpu = -1, vramUsed = -1, vramTotal = -1, mem = -1, memUsed = -1, memTotal = -1; };
class SysMon : public QObject {
    Q_OBJECT
public:
    explicit SysMon(QObject* parent = nullptr);
    SysStats sample();              // CPU・メモリは即時。GPU は直近の取得結果(nvidia-smi は非同期)
    static QString format(const SysStats& s);   // 画面に出す文字列
signals:
    void updated(const vui::bridge::SysStats& s);
public slots:
    void tick();
private:
    void pollGpu();
    unsigned long long idle0_ = 0, total0_ = 0;
    SysStats last_;
    QProcess* smi_ = nullptr;
    QString smiPath_;
    bool smiBad_ = false;
};

QString jobScript();               // python 側の実行内容(検査用)

}  // namespace vui::bridge

Q_DECLARE_METATYPE(vui::bridge::SysStats)
