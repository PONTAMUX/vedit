// 第5段階 第1部の自動テスト(QT_QPA_PLATFORM=offscreen)。音は鳴らさない・窓を表示しない
// 使い方: vedit-ui-test <デモのあるフォルダ> <作業フォルダ(C:/Temp/vui の下)> <出力フォルダ>
// 作業フォルダの demo.json を編集して保存し、<出力>/cpp_noop.json / cpp_ops.json を書く。Python 版の ui_ref.py の出力とバイト比較する(run_ui_test.py)
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QProcess>
#include <QThread>
#include <cstdio>
#include "editorwindow.h"
#include "reviewplayer.h"
#include <cmath>
#include <QPushButton>
#include "timelineview.h"

using namespace vui;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { ++fails; std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } else std::printf("ok  : %s\n", msg); } while (0)

static void copyFile(const QString& a, const QString& b) { QFile::remove(b); QFile::copy(a, b); }
static QByteArray readAll(const QString& p) { QFile f(p); f.open(QIODevice::ReadOnly); return f.readAll(); }
static void wait(int ms) { QElapsedTimer t; t.start(); while (t.elapsed() < ms) { QApplication::processEvents(); QThread::msleep(10); } }

static void key(EditorWindow& w, int k, Qt::KeyboardModifiers m = Qt::NoModifier, const QString& text = QString()) {
    QKeyEvent e(QEvent::KeyPress, k, m, text);
    w.handleKey(&e);
}

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
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) { std::printf("usage: vedit-ui-test <src> <work> <out>\n"); return 2; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("VEDIT_KEYMAP", "");
    qputenv("VEDIT_PREFS", QByteArray(argv[2]) + "/prefs.json");
    qputenv("VEDIT_LOCKS", QByteArray(argv[2]) + "/locks.json");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");   // offscreen は文字が出ないので   // 既定のキー割り当てで試す(空 = 既定のみ)
    QApplication app(argc, argv);
    const QString src = QString::fromLocal8Bit(argv[1]), work = QString::fromLocal8Bit(argv[2]), out = QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(work); QDir().mkpath(out);
    for (const char* n : {"demo.json", "bg.mp4", "bgm.wav"}) copyFile(src + "/" + n, work + "/" + n);
    QFile::remove(work + "/demo.review.json");
    { QStringList pp; for (const QString& e : QString::fromLocal8Bit(qgetenv("PATH")).split(';')) if (!e.contains("ffmpeg9")) pp << e; qputenv("PATH", pp.join(';').toLocal8Bit()); }   // Python 側が ffmpeg9(shared) を拾わないように
    const QString proj = work + "/demo.json";

    // ---- 1) 何もしないで保存 ----
    {
        EditModel m; QString err;
        CHECK(m.open(proj, &err), "open demo");
        CHECK(m.save() == EditModel::Saved, "save noop");
        copyFile(proj, out + "/cpp_noop.json");
        copyFile(src + "/demo.json", proj);   // 元に戻す
    }

    // ---- 2) 編集して保存 ----
    {
        EditModel m; QString err;
        m.open(proj, &err);
        m.startAutosave(false);
        m.markIn = 40; m.markOut = 41.5;
        CHECK(m.doMarkX(), "X: mark range cut");
        m.seek(30);
        CHECK(m.splitAll(30), "split at 30");
        m.select("telops", 1);
        CHECK(m.editDelete(false), "delete telop");
        m.toggleHide("v2", false);
        CHECK(m.isHidden("v2"), "hide v2");
        m.segSel.clear(); m.segSel.push_back({30, 40, QChar(0)});
        CHECK(m.editDelete(false), "delete segment leaving gap");
        const QByteArray before = m.serialize();
        CHECK(m.undo(), "undo");
        CHECK(m.serialize() != before, "undo changes");
        CHECK(m.redo(), "redo");
        CHECK(m.serialize() == before, "redo restores");
        m.select("telops", 1);
        m.nudge(3);
        CHECK(m.save() == EditModel::Saved, "save ops");
        copyFile(proj, out + "/cpp_ops.json");
        CHECK(!m.dirty(), "clean after save");
    }

    // ---- 3) 画面: キー操作・自動保存・外部変更・競合・スクリーンショット ----
    copyFile(src + "/demo.json", proj);
    {
        EditModel m; QString err;
        m.open(proj, &err);
        m.startAutosave(true);
        m.isPlaying = [] { return false; };
        m.startWatch(300);
        EditorWindow w(&m);
        w.resize(1400, 860);
        QImage stub(1280, 720, QImage::Format_RGB32); stub.fill(QColor("#101018"));
        w.setMonitorImage(stub);
        w.show();   // offscreen なので実際の窓は出ない
        wait(100);
        { const QImage shot0 = w.grab().toImage(); shot0.save(out + "/cpp_window.png"); CHECK(shot0.width() >= 1000, "grab window (initial)"); }

        CHECK(w.actionForKey("space") == "play" || !w.actionForKey("space").isEmpty(), "keymap: space bound");
        w.doAction("home");
        CHECK(m.tcur() == 0, "home -> 0");
        key(w, Qt::Key_Right);
        CHECK(m.tcur() > 0, "right steps a frame");
        const double t1 = m.tcur();
        key(w, Qt::Key_Left);
        CHECK(m.tcur() < t1, "left steps back");

        // 分割 → 自動保存(500ms 後)
        m.seek(30);
        w.doAction("split");
        CHECK(m.dirty(), "split marks dirty");
        wait(900);
        CHECK(!m.dirty(), "autosave cleared dirty");
        CHECK(readAll(proj).contains("30"), "autosave wrote splits");

        // undo/redo をキーで
        key(w, Qt::Key_Z, Qt::ControlModifier);
        CHECK(m.canRedo(), "ctrl+z undo");
        key(w, Qt::Key_Y, Qt::ControlModifier);
        wait(900);

        // 外部変更の監視: 別内容を書いて mtime を進める → 取り込み
        const QByteArray cur = readAll(proj);
        QThread::msleep(50);
        QByteArray ext = cur; ext.replace("\"splits\": [", "\"splits\": [\r\n    7.5,");
        { QFile f(proj); f.open(QIODevice::WriteOnly | QIODevice::Truncate); f.write(ext); }
        wait(1000);
        CHECK(m.checkExternal() || m.d().get("splits") != nullptr, "external watch ran");
        bool has75 = false;
        if (const OVal* s = m.d().get("splits")) for (int i = 0; i < s->size(); ++i) if (s->at(i).asNum() == 7.5) has75 = true;
        CHECK(has75, "external change loaded");

        // 競合: 未保存の編集がある状態で外部が書き換え → askOverwrite
        m.startAutosave(false);
        bool asked = false;
        w.askOverwrite = [&] { asked = true; return true; };
        m.seek(20); m.splitAll(20);
        QThread::msleep(50);
        { QFile f(proj); f.open(QIODevice::WriteOnly | QIODevice::Append); f.write(" "); }
        w.save();
        CHECK(asked, "conflict asks overwrite");
        CHECK(!m.dirty(), "overwrite saved");

        // 未保存の編集がある間は外部の変更を取り込まない(保存の時に競合として聞く)
        m.seek(25); m.splitAll(25);
        QThread::msleep(50);
        { QFile f(proj); f.open(QIODevice::WriteOnly | QIODevice::Append); f.write(" "); }
        CHECK(!m.checkExternal() && m.dirty(), "dirty blocks external reload");
        CHECK(m.save(true) == EditModel::Saved, "save after external");

        // 写し: 変わった時だけ <名前>.backup/ に書く
        const QString b1 = m.writeTimedBackup();
        CHECK(!b1.isEmpty() && QFileInfo::exists(b1) && b1.contains(".backup/"), "timed backup written");
        CHECK(m.writeTimedBackup().isEmpty(), "no backup when unchanged");
        const QString lb = m.writeBackup();
        CHECK(lb.endsWith(".local-bak.json") && readAll(lb) == readAll(proj), "local backup same as saved");
        QDir(QFileInfo(b1).absolutePath()).removeRecursively(); QFile::remove(lb);

        // 一時ファイルが書けない時は失敗を返し、元のファイルに触らない
        {
            const QByteArray before = readAll(proj);
            const QString tmpDir = QFileInfo(proj).absolutePath() + "/" + QFileInfo(proj).completeBaseName() + ".tmp";
            QDir().mkpath(tmpDir);   // 同じ名前のフォルダがあると書けない
            m.seek(27); m.splitAll(27);
            CHECK(m.save(true) == EditModel::Failed && m.dirty(), "save fails when tmp unwritable");
            CHECK(readAll(proj) == before, "original kept on failed save");
            QDir(tmpDir).removeRecursively();
            CHECK(m.save(true) == EditModel::Saved && readAll(proj) != before, "save ok after fix");
        }

    }

    // ================= 第2部 =================
    // ---- 4) ロックの保存 ----
    {
        copyFile(src + "/demo.json", proj);
        QFile::remove(work + "/locks.json");
        { EditModel m; QString e; m.open(proj, &e); m.setLocks({"v2", "a1"}, true); CHECK(m.isLocked("v2"), "lock set"); }
        { EditModel m; QString e; m.open(proj, &e); CHECK(m.isLocked("v2") && m.isLocked("a1") && !m.isLocked("v3"), "locks persisted (VEDIT_LOCKS)"); m.setLocks({"v2", "a1"}, false); }
        { EditModel m; QString e; m.open(proj, &e); CHECK(!m.isLocked("v2"), "unlock persisted"); }
    }

    // ---- 5) 画面の操作: ドラッグ・メニュー・パネル・追加 ----
    {
        copyFile(src + "/demo.json", proj);
        EditModel m; QString err; m.open(proj, &err);
        m.startAutosave(false);
        m.isPlaying = [] { return false; };
        EditorWindow w(&m);
        w.setWaveJobEnabled(false);
        w.resize(1400, 860);
        QImage stub(1280, 720, QImage::Format_RGB32); stub.fill(QColor("#101018"));
        w.setMonitorImage(stub);
        w.confirm = [](const QString&) { return true; };
        w.askText = [](const QString&, const QString& i) { return i; };
        w.show();
        wait(200);
        TimelineView* tl = w.timeline();
        std::vector<MenuItem> menu;
        tl->menuHandler = [&](const QPoint&, const std::vector<MenuItem>& it) { menu = it; };
        auto mouse = [&](QEvent::Type t, QPoint p, Qt::MouseButton b, Qt::KeyboardModifiers mod = Qt::NoModifier) {
            QMouseEvent e(t, QPointF(p), QPointF(tl->mapToGlobal(p)), b, t == QEvent::MouseButtonRelease ? Qt::NoButton : b, mod);
            QApplication::sendEvent(tl, &e);
        };
        // テロップをドラッグで動かす
        const QString tlane = m.laneOf("telops", m.d().get("telops")->at(1));
        const double s0 = m.d().get("telops")->at(1).numOr("start", 0);
        const QPoint p0 = tl->screenOf(s0 + 1.0, tlane);
        mouse(QEvent::MouseButtonPress, p0, Qt::LeftButton);
        mouse(QEvent::MouseMove, p0 + QPoint(30, 0), Qt::LeftButton);
        mouse(QEvent::MouseMove, p0 + QPoint(80, 0), Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, p0 + QPoint(80, 0), Qt::LeftButton);
        CHECK(m.selKind == "telops", "click selects telop");
        const double s1 = m.selI >= 0 ? m.d().get("telops")->at(m.selI).numOr("start", 0) : s0;
        CHECK(std::fabs(s1 - s0) > 0.05, "drag moves telop");
        CHECK(m.dirty(), "drag marks dirty");
        w.doAction("undo");
        // 右クリックメニュー
        menu.clear();
        mouse(QEvent::MouseButtonPress, tl->screenOf(m.d().get("telops")->at(m.selI >= 0 ? m.selI : 1).numOr("start", 0) + 0.5, tlane), Qt::RightButton);
        CHECK(!menu.empty(), "context menu on clip");
        menu.clear();
        mouse(QEvent::MouseButtonPress, QPoint(10, tl->laneTop("v2") + 6), Qt::RightButton);
        CHECK(!menu.empty(), "context menu on track label");
        // 範囲選択
        m.select(QString(), -1);
        mouse(QEvent::MouseButtonPress, tl->screenOf(20, "v1"), Qt::LeftButton, Qt::ControlModifier);
        mouse(QEvent::MouseMove, tl->screenOf(24, "v1"), Qt::LeftButton, Qt::ControlModifier);
        mouse(QEvent::MouseButtonRelease, tl->screenOf(24, "v1"), Qt::LeftButton, Qt::ControlModifier);
        CHECK(true, "marquee/range drag does not crash");

        // 追加(テロップ/トピック/差し込み/効果音)
        const int nt = m.d().get("telops")->size(), np = m.d().get("topics")->size(), no = m.d().get("overlays")->size();
        w.addTelop(); CHECK(m.d().get("telops")->size() == nt + 1 && m.selKind == "telops", "add telop");
        w.addTopic(); CHECK(m.d().get("topics")->size() == np + 1, "add topic");
        w.pickFile = [&](const QString& k) { return k == "audio" ? work + "/bgm.wav" : work + "/bg.mp4"; };
        w.addOverlay(); CHECK(m.d().get("overlays")->size() == no + 1, "add overlay");
        const int ns = m.d().get("sfx") ? m.d().get("sfx")->size() : 0;
        w.addSfx(); CHECK((m.d().get("sfx") ? m.d().get("sfx")->size() : 0) == ns + 1, "add sfx");

        // 左右パネル
        for (int i = 0; i < 3; ++i) { w.setLeftTab(i); w.setRightTab(i); }
        w.setRightTab(2);
        CHECK(w.rightBody() && w.leftBody(), "panels build (all tabs)");
        CHECK(m.histRows().size() > 1, "history has rows");
        w.setRightTab(0); w.setLeftTab(0);

        // メーター・縦スクロール・システム表示
        CHECK(w.meter() != nullptr && w.vscroll() != nullptr, "meter + vscroll exist");
        w.tickSys(); wait(2500);
        CHECK(w.sysText().contains("CPU"), "sysmon text has CPU");

        // 音声/画像編集のコマンドライン(実行しない)
        QStringList launched;
        auto oldL = bridge::launcher;
        bridge::launcher = [&](const bridge::Cmd& c) { launched << c.line(); return true; };
        const bridge::Cmd ac = bridge::audioCmd(proj, true);
        CHECK(ac.prog.endsWith("vaudio.exe") && ac.args.contains("--process") && ac.args.contains("--force"), "audio cmd = vaudio --process");
        const bridge::Cmd vc = bridge::vimgOpenCmd("C:/Temp/x.png");
        CHECK(vc.prog.endsWith("vimg.exe") && vc.args.contains("C:/Temp/x.png"), "vimg cmd line");
        bridge::launch(vc);
        CHECK(launched.size() == 1 && launched[0].contains("vimg.exe"), "vimg launched only via fake launcher");
        bridge::launcher = oldL;

        // 別名保存
        const QString dst = work + "/saved_as.vedit.json";
        QFile::remove(dst);
        w.pickSave = [&](const QString&, const QString&) { return dst; };
        w.saveAs();
        CHECK(QFileInfo::exists(dst), "saveAs wrote file");
        CHECK(QFileInfo(m.path()).absoluteFilePath() == QFileInfo(dst).absoluteFilePath(), "saveAs switched project");
        copyFile(src + "/demo.json", proj);
    }

    // ---- 6) プレビュー実画像・仕上がり確認・修正点・書き出し(子プロセス) ----
    {
        copyFile(src + "/demo.json", proj);
        const QString use = QStringLiteral("C:/vedit/native/dist/vedit-native.exe");
        const QString png = out + "/cpp_preview.png";
        QFile::remove(png);
        QProcess pr;
        { QProcessEnvironment pe = QProcessEnvironment::systemEnvironment(); pe.remove("QT_QPA_PLATFORM"); pr.setProcessEnvironment(pe); }
        pr.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= 0x08000000; });
        pr.start(use, {"--frame", proj, "5", png});
        pr.waitForFinished(120000);
        QImage frame(png);
        EditModel m; QString err; m.open(proj, &err);
        m.startAutosave(false);
        EditorWindow w(&m);
        w.setWaveJobEnabled(false);
        w.resize(1400, 860);
        if (!frame.isNull()) w.setMonitorImage(frame);
        w.confirm = [](const QString&) { return true; };
        w.show();
        wait(200);
        CHECK(!frame.isNull(), "preview frame rendered by vedit-native --frame");
        const QImage mon = w.grabMonitor();
        bool lit = false;
        for (int y = 0; y < mon.height() && !lit; y += 7) for (int x = 0; x < mon.width(); x += 7) if (qGray(mon.pixel(x, y)) > 40) { lit = true; break; }
        CHECK(lit, "monitor grab shows preview (not black)");
        w.grab().save(out + "/cpp_window_preview.png");

        // 仕上がり確認
        w.checkFrame();
        QElapsedTimer tm; tm.start();
        while (!w.checkVisible() && tm.elapsed() < 120000) wait(100);
        CHECK(w.checkVisible() && !w.checkImage().isNull(), "check frame (actual render) shown");
        std::printf("  [dbg] toast=%s err=%s\n", qPrintable(w.lastToast()), qPrintable(w.lastError));

        // 修正点: 控え → 足す
        w.reviewToggle();
        wait(300);
        tm.restart();
        while (w.reviewBusy() && tm.elapsed() < 60000) wait(50);
        QJsonObject o; o["op"] = "start";
        w.reviewOp(o);
        tm.restart();
        while (w.reviewBusy() && tm.elapsed() < 60000) wait(50);
        CHECK(w.reviewData().value("exists").toBool(), "review: baseline saved");
        QJsonObject a; a["op"] = "add"; a["text"] = "0:07 画像";
        w.reviewOp(a);
        tm.restart();
        while (w.reviewBusy() && tm.elapsed() < 60000) wait(50);
        CHECK(w.reviewData().value("points").toArray().size() == 1, "review: point added");
        std::printf("  [dbg] review=%s err=%s\n", QJsonDocument(w.reviewData()).toJson(QJsonDocument::Compact).left(300).constData(), qPrintable(w.lastError));
        w.reviewToggle();

        // 書き出し(デモのコピーを C:/Temp に。低画質)
        m.mutate([&] { m.d().get("output")->set("file", OVal::str(work + "/out_test.mp4")); });
        w.startExport(true, QJsonObject(), false);
        tm.restart();
        while (w.exporting() && tm.elapsed() < 540000) wait(200);
        const QString eo = w.lastExportOutput;
        CHECK(!eo.isEmpty() && QFileInfo::exists(QFileInfo(eo).isAbsolute() ? eo : work + "/" + eo), "export (draft) wrote output");
        std::printf("  [dbg] export out=%s err=%s\n", qPrintable(eo), qPrintable(w.lastError));
        w.grab().save(out + "/cpp_window_export.png");

        // ---- 修正点パネルに修正前の動画を埋め込む(窓は出さない=offscreen。音は出さない) ----
        {
            // 合わせ方の判断(review.js tick と同じ)
            auto A = decideSync(10.3, 10.0, true, 1.0, false, 1.0, false);
            CHECK(!A.seek && A.setRate && std::fabs(A.rate - 1.1) < 1e-9 && !A.play, "review sync: playing, 0.3s behind -> speed 1.1x, no seek");
            A = decideSync(9.9, 10.0, true, 1.0, false, 1.0, false);
            CHECK(!A.seek && A.setRate && std::fabs(A.rate - 0.95) < 1e-9, "review sync: playing, 0.1s ahead -> 0.95x");
            A = decideSync(10.0, 10.0, true, 2.0, true, 2.0, false);
            CHECK(!A.seek && !A.setRate && A.play, "review sync: in sync, paused video starts playing");
            A = decideSync(13.0, 10.0, true, 1.0, false, 1.0, false);
            CHECK(A.seek && A.seekTo == 13.0 && A.rate == 1.0, "review sync: >=1.5s gap while playing -> seek");
            A = decideSync(13.0, 10.0, true, 1.0, false, 1.0, true);
            CHECK(!A.seek, "review sync: no seek while a seek is pending");
            A = decideSync(11.4, 10.0, true, 1.0, false, 1.0, false);
            CHECK(!A.seek, "review sync: 1.4s gap while playing -> no seek (catch up by speed)");
            A = decideSync(5.0, 5.01, false, 1.0, false, 1.0, false);
            CHECK(!A.seek && A.pause, "review sync: stopped -> pause, <0.03s off -> no seek");
            A = decideSync(5.0, 5.2, false, 1.0, true, 1.0, false);
            CHECK(A.seek && A.seekTo == 5.0 && !A.pause, "review sync: stopped, off by 0.2s -> seek");

            if (!w.reviewShown()) w.reviewToggle();
            QElapsedTimer t2; t2.start();
            while (w.reviewBusy() && t2.elapsed() < 60000) wait(50);
            const QString vabs = QFileInfo(eo).isAbsolute() ? eo : work + "/" + eo;
            QJsonObject st; st["op"] = "start"; st["force"] = true; st["video"] = vabs;
            w.reviewOp(st);
            t2.restart();
            while (w.reviewBusy() && t2.elapsed() < 60000) wait(50);
            CHECK(w.reviewData().value("exists").toBool() && !w.reviewData().value("video").toString().isEmpty(), "review video: baseline with video saved");
            auto* pl = w.reviewPlayer();
            { QElapsedTimer q; q.start(); while (pl && !pl->isVisible() && q.elapsed() < 3000) wait(50); }
            CHECK(pl && pl->parentWidget() && pl->isVisible(), "review video: player embedded in the panel");
            // 軽い版(proxy.make_video と同じ)ができるまで待つ → 差し替わる
            t2.restart();
            while ((w.reviewBusy() || !pl->key().endsWith("|pv")) && t2.elapsed() < 180000) wait(100);
            CHECK(pl->key().endsWith("|pv") && w.reviewData().value("video_pv").toBool(), "review video: light 720p proxy built and switched to");
            std::printf("  [dbg] review video key=%s proxy=%s\n", qPrintable(pl->key()), qPrintable(w.reviewData().value("pv_path").toString()));
            CHECK(!pl->sound() && !pl->audioOpen(), "review video: sound off by default (no audio output opened)");
            t2.restart();
            while (!(pl->ready() && pl->duration() > 1) && t2.elapsed() < 20000) wait(50);
            CHECK(pl->ready() && pl->duration() > 1, "review video: loaded");
            CHECK(w.reviewPanel()->findChildren<QWidget*>("rv-video").size() == 1, "review video: one video screen");

            // 位置の対応: 修正前の控え = 今の状態なので、修正前の動画の秒 = 今の出力秒
            auto want = [&](double src) { return m.outTime(src); };
            auto settle = [&](double target, int ms) {
                QElapsedTimer q; q.start();
                while (q.elapsed() < ms) { w.reviewTick(); wait(60); if (std::fabs(pl->position() - target) < 0.12) break; }
                return pl->position();
            };
            const double srcA = m.srcOf(m.totalOut() * 0.5, true);
            m.seek(srcA);
            const double pa = settle(want(srcA), 6000);
            std::printf("  [dbg] paused map: src=%.3f want=%.3f video=%.3f\n", srcA, want(srcA), pa);
            CHECK(std::fabs(pa - want(srcA)) < 0.15, "review video: stopped playhead -> video at the mapped time");
            CHECK(!pl->isPlaying(), "review video: stays paused while stopped");
            { QElapsedTimer q; q.start(); while (!pl->hasFrame() && q.elapsed() < 3000) { w.reviewTick(); wait(50); } }
            CHECK(pl->hasFrame(), "review video: first frame is shown while paused");
            CHECK(pl->caption().startsWith("修正前 ") && pl->caption().contains("今 "), "review video: caption shows both times");
            const double srcB = m.srcOf(m.totalOut() * 0.2, true);
            m.seek(srcB);
            const double pb = settle(want(srcB), 6000);
            CHECK(std::fabs(pb - want(srcB)) < 0.15, "review video: seek -> video follows");

            // 再生中: シークせず速さで追いつく / 大きく飛んだ時だけシーク
            const int sk0 = pl->seekCount();
            double o = want(srcB);
            w.onPreviewPlaying(true);
            QElapsedTimer pt; pt.start();
            double minR = 9, maxR = 0;
            const double o0 = o;
            while (pt.elapsed() < 2500) {
                o = o0 + pt.elapsed() / 1000.0;
                w.onPreviewTime(o);
                w.reviewTick();
                if (pt.elapsed() > 600) { minR = std::min(minR, pl->playbackRate()); maxR = std::max(maxR, pl->playbackRate()); }
                wait(40);
            }
            CHECK(pl->isPlaying(), "review video: plays while the editor plays");
            CHECK(pl->seekCount() <= sk0 + 1, "review video: no repeated seeks during playback");
            CHECK(minR >= 0.89 && maxR <= 1.11, "review video: speed stays within +-10%");
            const double drift = std::fabs(pl->position() - o);
            std::printf("  [dbg] playing: video=%.3f editor=%.3f drift=%.3f seeks=%d rate=[%.3f,%.3f]\n", pl->position(), o, drift, pl->seekCount() - sk0, minR, maxR);
            CHECK(drift < 0.6, "review video: follows within 0.6s while playing");
            const int sk1 = pl->seekCount();
            const double jump = std::min(o + 6.0, m.totalOut() - 1.0);
            if (jump - o > 2.0) {
                w.onPreviewTime(jump);
                w.reviewTick();
                CHECK(pl->seekCount() == sk1 + 1, "review video: big jump (>=1.5s) while playing seeks once");
            }
            w.onPreviewPlaying(false);
            w.reviewTick();
            wait(200);
            w.reviewTick();
            CHECK(!pl->isPlaying(), "review video: pauses when the editor stops");

            // 修正で後ろがずれても(カットを足す)、先方が見た動画の秒は元素材の位置から正しく引ける
            const double srcC = m.srcOf(m.totalOut() * 0.8, true);
            const double baseC = want(srcC);
            const double baseA = want(srcA);
            m.cutRange(srcA - 1.0, srcA - 0.5);
            m.seek(srcC);
            const double curC = m.outTime(srcC);
            w.save();   // 保存 → 修正点を取り直す(cur が変わる)
            wait(300);
            t2.restart();
            while (w.reviewBusy() && t2.elapsed() < 30000) wait(50);
            wait(300);
            const double pc = settle(baseC, 6000);
            std::printf("  [dbg] after cut: base=%.3f cur=%.3f video=%.3f cap=%s\n", baseC, curC, pc, qPrintable(pl->caption()));
            CHECK(std::fabs(curC - baseC) > 0.1, "review video: the cut moved the later part (now != pre-revision)");
            CHECK(std::fabs(pc - baseC) < 0.2, "review video: after a cut, video stays at the pre-revision time of the same source position");
            CHECK(pl->caption().endsWith(QString("今 %1:%2").arg((int)curC / 60).arg(curC - ((int)curC / 60) * 60, 5, 'f', 2, QChar('0'))), "review video: caption shows the new time");

            // 別窓 / 右に戻す: 同じパネル・同じ動画のまま移る
            CHECK(!w.reviewPopped(), "review video: docked at first");
            CHECK(w.reviewPanel()->findChild<QPushButton*>("rv-pop") != nullptr, "review video: 別窓 button when docked");
            const QString key0 = pl->key();
            w.reviewPopout();
            wait(300);
            CHECK(w.reviewPopped() && w.reviewWindow() && w.reviewPanel()->window() == w.reviewWindow(), "review popout: panel moved to its own window");
            auto vis = [&](const char* nm) { for (auto* b : w.reviewPanel()->findChildren<QPushButton*>(nm)) if (b->isVisibleTo(w.reviewPanel())) return true; return false; };
            { QElapsedTimer q; q.start(); while (!vis("rv-dock") && q.elapsed() < 3000) wait(50); }
            CHECK(vis("rv-dock"), "review popout: 右に戻す button shown");
            CHECK(!vis("rv-pop"), "review popout: 別窓 button gone");
            CHECK(w.reviewPlayer() == pl && pl->key() == key0 && pl->window() == w.reviewWindow(), "review popout: same player and source");
            m.seek(srcA);
            const double pp = settle(baseA, 6000);
            std::printf("  [dbg] popout map: want=%.3f video=%.3f\n", baseA, pp);
            CHECK(std::fabs(pp - baseA) < 0.2, "review popout: video still follows the playhead in the popout");
            w.reviewDock();
            wait(300);
            CHECK(!w.reviewPopped() && w.reviewPanel()->window() == &w && w.reviewPanel()->isVisible(), "review dock: panel back on the right");
            CHECK(vis("rv-pop") && !vis("rv-dock") && w.reviewPlayer() == pl && pl->key() == key0, "review dock: 別窓 button back, same player");
            w.reviewToggle();
            CHECK(!w.reviewShown(), "review video: panel closed");
            for (int k = 0; k < 8; ++k) { wait(100); }
        }
    }
    {
        EditModel m; QString err;
        if (m.open(proj, &err)) {
            m.startAutosave(false);
            EditorWindow w(&m);
            w.show(); wait(50);
            w.setRightTab(3); wait(50);
            CHECK(w.rightTab() == 3, "settings tab opens");
            w.setRightTab(0); wait(20);
        }
    }
    std::printf(fails ? "NG: %d failures\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
