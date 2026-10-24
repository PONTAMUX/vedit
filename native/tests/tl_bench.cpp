// タイムラインの再生中の重さを測る(offscreen・音なし・窓なし)。usage: vedit-tl-bench <project.vedit.json のコピー>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <cstdio>
#include <windows.h>
#include "editorwindow.h"
#include "timelineview.h"
using namespace vui;
int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& m) { std::fprintf(stderr, "%s\n", m.toLocal8Bit().constData()); if (t == QtFatalMsg) { std::fflush(stderr); std::_Exit(3); } });
    const QString exeDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (!QFileInfo::exists(exeDir + "/platforms/qoffscreen.dll") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH"))
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", "C:/Libs/Qt/6.10.3/mingw_64/plugins/platforms");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2) return 2;
    qputenv("QT_QPA_PLATFORM", "offscreen");
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR")) qputenv("QT_QPA_FONTDIR", "C:/Windows/Fonts");
    QApplication app(argc, argv);
    EditModel m; QString err;
    if (!m.open(QString::fromLocal8Bit(argv[1]), &err)) { std::printf("open: %s\n", qPrintable(err)); return 3; }
    m.startAutosave(false);
    bool playing = false;
    m.isPlaying = [&] { return playing; };
    EditorWindow w(&m);
    w.setWaveJobEnabled(false);
    w.resize(1900, 1000);
    QImage stub(1280, 720, QImage::Format_RGB32); stub.fill(Qt::black);
    w.setMonitorImage(stub);
    w.show();
    for (int i = 0; i < 20; ++i) app.processEvents();
    TimelineView* tl = w.timeline();
    auto bench = [&](const char* name, auto fn) {
        QElapsedTimer q; q.start(); const int N = 60;
        for (int i = 0; i < N; ++i) fn(i);
        std::printf("%-28s %.2f ms/回\n", name, q.nsecsElapsed() * 1e-6 / N);
    };
    {   // JSON(undo の控え)の重さ
        QString js; OVal v;
        bench("dump(undo の控え)", [&](int) { js = m.d().dump(false); });
        const QByteArray u = js.toUtf8();
        bench("parse", [&](int) { OVal::parse(u, &v); });
        bench("OVal の写し", [&](int) { v = m.d(); });
        std::printf("控えの大きさ %d KB\n", int(u.size() / 1024));
    }
    if (qEnvironmentVariableIsSet("TLB_SHOT")) {   // 見た目の比べ用
        const QString d = qEnvironmentVariable("TLB_SHOT");
        tl->grab().save(d + "/fit.png");
        const double zz = tl->zoom(); tl->setZoom(zz * 8); app.processEvents(); tl->grab().save(d + "/z8.png"); tl->setZoom(zz); app.processEvents();
        m.collapsed = true; tl->update(); app.processEvents(); tl->grab().save(d + "/col.png"); m.collapsed = false; tl->update(); app.processEvents();
    }
    bench("timeline repaint", [&](int) { tl->repaint(); });
    bench("repaint 全描き直し", [&](int) { tl->update(); tl->repaint(); });
    m.collapsed = true; tl->update(); app.processEvents();
    bench("repaint カット詰め", [&](int) { tl->repaint(); });
    m.collapsed = false;
    const double z0 = tl->zoom();
    tl->zoomFit(); app.processEvents();
    bench("repaint 全体表示", [&](int) { tl->repaint(); });
    tl->setZoom(z0 * 8); app.processEvents();
    bench("repaint 8倍ズーム", [&](int) { tl->repaint(); });
    tl->setZoom(z0); app.processEvents();
    w.resize(3800, 2000); for (int i = 0; i < 5; ++i) app.processEvents();
    bench("repaint 4K窓", [&](int) { tl->repaint(); });
    w.resize(1900, 1000); for (int i = 0; i < 5; ++i) app.processEvents();
    w.onPreviewPlaying(true); playing = true;
    double t = 300;
    bench("再生1コマ(onPreviewTime+描画)", [&](int) { t += 1.0 / 30; w.onPreviewTime(t); app.processEvents(); });
    bench("window repaint", [&](int) { w.repaint(); });
    w.onPreviewPlaying(false); playing = false;
    // 編集1回(分割・Q)の重さ。毎回 undo で戻して同じ状態から測る
    auto once = [&](const char* name, auto fn) {
        for (int k = 0; k < 3; ++k) {
            QElapsedTimer q; q.start();
            fn(); const double op = q.nsecsElapsed() * 1e-6;
            for (int i = 0; i < 5; ++i) app.processEvents();
            const double a = q.nsecsElapsed() * 1e-6;
            q.restart(); m.undo(); const double uop = q.nsecsElapsed() * 1e-6;
            for (int i = 0; i < 5; ++i) app.processEvents();
            std::printf("%-28s %.1f ms [処理 %.1f] (undo %.1f ms [処理 %.1f])\n", name, a, op, q.nsecsElapsed() * 1e-6, uop);
        }
    };
    m.seek(400.3);
    once("分割 E", [&] { m.splitAll(m.tcur()); });
    once("Q", [&] { m.trimTo(-1); });
    once("W", [&] { m.trimTo(1); });
    return 0;
}
