// vaudio: オフスクリーン GUI テスト。チェーン編集 → 保存 → 読み直し、audio 以外のキーが変わらないこと、
// 内蔵エフェクトだけの処理実行(VST3 のエディタは開かない)。使い方: gui_test <作業フォルダ> <元音声.wav>
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

#include "engine.h"
#include "gui.h"

using namespace vaudio;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "NG: %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

// 窓を出さない: Qt の致命エラー(プラグインが無い等)はメッセージボックスでなく標準エラーへ出して終わる。
// 子プロセスにも引き継がれるよう、Windows のエラーダイアログ(DLL が無い等)も止める
#include <QLibraryInfo>
#include <QFileInfo>
#include <QCoreApplication>
#include <cstdlib>
#include <windows.h>
static void noWindowGuard(char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& m) {
        std::fprintf(stderr, "%s\n", m.toLocal8Bit().constData());
        if (t == QtFatalMsg) { std::fflush(stderr); std::_Exit(3); }
    });
    // exe の隣に platforms/qoffscreen.dll が無ければ Qt 本体のプラグインを使う
    const QString exeDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (!QFileInfo::exists(exeDir + "/platforms/qoffscreen.dll") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH"))
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", "C:/Libs/Qt/6.10.3/mingw_64/plugins/platforms");
}

int main(int argc, char** argv) {
    noWindowGuard(argv);
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (argc < 3) { fprintf(stderr, "usage: gui_test <dir> <src.wav>\n"); return 2; }
    std::string dir = argv[1], src = argv[2];
    std::string proj = dir + "/gui.vedit.json";
    const char* base =
        "{\n \"version\": 1,\n \"source\": \"SRC\",\n \"cuts\": [[1.5, 2.5]],\n \"zeta\": {\"k\": [1, 2.0, \"日本語\"]},\n"
        " \"audio\": {\"chain\": [{\"type\": \"normalize\", \"peak_db\": -3}], \"other\": 7},\n \"output\": {\"cpu_percent\": 50}\n}\n";
    std::string txt = base;
    txt.replace(txt.find("SRC"), 3, src);
    CHECK(writeFileText(proj, txt));

    vj::Value before = vj::parse(txt);
    fprintf(stderr, "start\n");
    AudioWindow w;
    fprintf(stderr, "window ok\n");
    QString err;
    fprintf(stderr, "load\n");
    CHECK(w.loadProject(QString::fromUtf8(proj.c_str()), &err));
    CHECK(w.chain().size() == 1);

    // 編集: De-click 追加 → ゲイン追加 → HighpassFilter 追加 → ゲインを並べ替え → 値の編集 → ノーマライズ基準を LUFS へ
    w.addDeclick();
    w.addGain();
    w.addPlugin("HighpassFilter");
    CHECK(w.chain().size() == 4);
    CHECK(w.chain().at(3).getStr("type") == "normalize");   // 追加は normalize の手前
    w.moveRow(1, 1);                                         // ゲインと HighpassFilter を入れ替え
    CHECK(w.chain().at(1).getStr("type") == "plugin");
    CHECK(w.chain().at(2).getStr("type") == "gain");
    w.setStepNumber(2, "db", 3.5);
    w.setStepNumber(0, "threshold", 6);
    w.setStepNumber(1, "cutoff_frequency_hz", 100);
    w.setNormalizeMode(3, true);
    w.setStepNumber(3, "lufs", -16);
    w.addPlugin("Limiter");
    w.removeRow(3);                                          // 追加した Limiter を消す
    CHECK(w.chain().size() == 4);
    QString e2;
    CHECK(w.saveProject(&e2));

    // 読み直し
    AudioWindow w2;
    CHECK(w2.loadProject(QString::fromUtf8(proj.c_str()), &err));
    const vj::Value& c = w2.chain();
    CHECK(c.size() == 4);
    CHECK(c.at(0).getStr("type") == "declick" && c.at(0).find("params")->getNum("threshold", 0) == 6);
    CHECK(c.at(1).getStr("name") == "HighpassFilter" && c.at(1).find("params")->getNum("cutoff_frequency_hz", 0) == 100);
    CHECK(c.at(2).getStr("type") == "gain" && c.at(2).getNum("db", 0) == 3.5);
    CHECK(c.at(3).getStr("type") == "normalize" && c.at(3).getNum("lufs", 0) == -16 && c.at(3).getNum("peak_db", 0) == -1);

    // audio 以外は変わっていない / audio の chain 以外のキーも残っている
    std::string after;
    CHECK(readFileText(proj, after));
    vj::Value av = vj::parse(after);
    for (const char* k : {"version", "source", "cuts", "zeta", "output"}) CHECK(vj::equal(av.get(k), before.get(k)));
    CHECK(av.find("audio")->getNum("other", 0) == 7);
    CHECK(av.o->size() == before.o->size());
    for (size_t i = 0; i < av.o->size(); i++) CHECK((*av.o)[i].first == (*before.o)[i].first);   // キーの順序

    // 処理実行(内蔵のみ)→ 完了を待つ → audio.wav ができる
    fprintf(stderr, "run\n");
    w2.runProcess();
    bool ok = false;
    QEventLoop loop;
    QObject::connect(&w2, &AudioWindow::processFinished, &loop, [&](bool o, QString m) { ok = o; fprintf(stderr, "finished: %s\n", m.toUtf8().constData()); loop.quit(); });
    QTimer::singleShot(120000, &loop, &QEventLoop::quit);
    loop.exec();
    CHECK(ok);
    std::string wavTxt;
    CHECK(readFileText(defaultCacheDir(proj) + "/audio.wav", wavTxt) && wavTxt.size() > 1000);

    fprintf(stderr, fails ? "FAILED %d\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
