// 窓を出さない描画・音量メーターの自動テスト(QGuiApplication + QOffscreenSurface + FBO。QWidget は作らない・音デバイスは開かない)
// 使い方: vedit-gl-test <デモのあるフォルダ> <作業フォルダ(C:/Temp の下)> <vedit_main.py>
//  1) 編集すると次の描画が変わる(元に戻すと元の絵に戻る)  2) 編集後の絵が Python 版 (vedit frame) と一致(PSNR)  3) 反映の遅れを測る
//  4) 音量メーター: Null 出力で takeLevels が実際の音(AudioMixer の直接出力)と合う
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QProcess>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include "media/audiomixer.h"
#include "media/audioplayer.h"
#include "editmodel.h"
#include "renderer.h"

using namespace vui;

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { ++fails; std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } else std::printf("ok  : %s\n", msg); } while (0)

static void noWindowGuard(char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    qInstallMessageHandler([](QtMsgType t, const QMessageLogContext&, const QString& m) {
        std::fprintf(stderr, "%s\n", m.toLocal8Bit().constData());
        if (t == QtFatalMsg) { std::fflush(stderr); std::_Exit(3); }
    });
    const QString exeDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (!QFileInfo::exists(exeDir + "/platforms/qwindows.dll") && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM_PLUGIN_PATH"))
        qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", "C:/Libs/Qt/6.10.3/mingw_64/plugins/platforms");
}

static double psnr(const QImage& a0, const QImage& b0) {
    QImage a = a0.convertToFormat(QImage::Format_RGB32), b = b0.convertToFormat(QImage::Format_RGB32);
    // 大きいほうを小さいほうへ縮小して比べる(小さい画像を拡大すると細部が無い分だけ PSNR が不当に下がる)
    if (b.size() != a.size()) {
        if ((long long)b.width() * b.height() > (long long)a.width() * a.height()) b = b.scaled(a.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        else a = a.scaled(b.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    double s = 0; long long n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* p = (const QRgb*)a.constScanLine(y); const QRgb* q = (const QRgb*)b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) {
            const int d[3] = {qRed(p[x]) - qRed(q[x]), qGreen(p[x]) - qGreen(q[x]), qBlue(p[x]) - qBlue(q[x])};
            for (int c : d) s += (double)c * c;
            n += 3;
        }
    }
    const double m = s / (double)n;
    return m == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / m);
}
static double meanAbsDiff(const QImage& a0, const QImage& b0) {
    QImage a = a0.convertToFormat(QImage::Format_RGB32), b = b0.convertToFormat(QImage::Format_RGB32);
    if (a.size() != b.size()) return 255;
    double s = 0; long long n = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* p = (const QRgb*)a.constScanLine(y); const QRgb* q = (const QRgb*)b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) { s += std::abs(qRed(p[x]) - qRed(q[x])) + std::abs(qGreen(p[x]) - qGreen(q[x])) + std::abs(qBlue(p[x]) - qBlue(q[x])); n += 3; }
    }
    return s / (double)n;
}

int main(int argc, char** argv) {
    noWindowGuard(argv);
    if (argc < 4) { std::printf("usage: vedit-gl-test <src> <work> <vedit_main.py>\n"); return 2; }
    const QString exeDir = QFileInfo(QString::fromLocal8Bit(argv[0])).absolutePath();
    if (!QFileInfo::exists(exeDir + "/platforms/qoffscreen.dll")) { std::printf("FAIL: platforms/qoffscreen.dll が無い\n"); return 2; }
    QGuiApplication app(argc, argv);   // 既定の windows プラグイン。窓は作らない
    const QString src = QString::fromLocal8Bit(argv[1]), work = QString::fromLocal8Bit(argv[2]), pyMain = QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(work);
    for (const char* f : {"demo.json", "bg.mp4", "bgm.wav"}) { QFile::remove(work + "/" + f); QFile::copy(src + "/" + f, work + "/" + f); }
    QFile::remove(work + "/demo.cache");
    const QString proj = work + "/demo.json";

    QOffscreenSurface surf; surf.setFormat(QSurfaceFormat::defaultFormat()); surf.create();
    QOpenGLContext ctx; ctx.setFormat(QSurfaceFormat::defaultFormat());
    if (!ctx.create() || !ctx.makeCurrent(&surf)) { std::printf("FAIL: GL コンテキスト\n"); return 2; }

    EditModel model;
    QString err;
    if (!model.open(proj, &err)) { std::printf("FAIL: open %s\n", qPrintable(err)); return 2; }
    {   // この環境に無い画像(D: の素材)を使う差し込みは外す(Python 版の ffmpeg が開けず比較できないため)
        OVal* ov = model.d().get("overlays");
        for (int i = ov->size() - 1; i >= 0; --i) {
            const QString f = ov->at(i).strOr("file", "");
            if (!QFileInfo::exists(model.resolve(f))) ov->erase(i);
        }
        model.save(true);
    }
    vn::Renderer r;
    if (!r.load(proj, &err) || !r.initGL()) { std::printf("FAIL: renderer %s\n", qPrintable(err)); return 2; }
    r.setLiveSource([&] { return model.serialize(); });
    QObject::connect(&model, &EditModel::changed, [&] { r.invalidate(); });
    const double T = 5.0;   // テロップ2(3〜8s)と差し込み1(3〜9s)が見える所

    auto draw = [&](bool wait) { r.render(T, wait); return r.grab().convertToFormat(QImage::Format_RGB32); };
    QImage base = draw(true);
    CHECK(!base.isNull() && base.width() > 0, "元の絵が描ける");
    CHECK(draw(true) == base, "同じ内容なら同じ絵(再現する)");

    // ---- 1) 編集 → 次の描画が変わる ----
    model.mutate([&] {
        model.d().get("telops")->at(1).set("text", OVal::str(QStringLiteral("編集した文字 ABC")));
        model.d().get("overlays")->at(0).set("in", OVal::num(40));
    });
    CHECK(r.hasPending(), "編集で再読み込み待ちになる");
    QImage edited = draw(true);
    CHECK(!r.hasPending() && r.reloadCount() == 1, "次の描画で1回だけ反映される");
    const double dEdit = meanAbsDiff(base, edited);
    CHECK(dEdit > 0.05, "編集後の絵が変わる");
    std::printf("  編集前後の平均差 %.3f\n", dEdit);
    model.undo();
    QImage undone = draw(true);
    CHECK(meanAbsDiff(base, undone) < 0.01, "元に戻すと元の絵に戻る");
    std::printf("  元に戻した後の差 %.4f / 再読み込み %d 回\n", meanAbsDiff(base, undone), r.reloadCount());
    model.redo();
    edited = draw(true);

    // ---- 2) Python 版との一致 ----
    const QString edJson = work + "/edit.json";
    { QFile f(edJson); f.open(QIODevice::WriteOnly); f.write(model.serialize()); }
    for (const char* f : {"bg.mp4", "bgm.wav"}) { (void)f; }
    const QString pyPng = work + "/p_edit.png";
    QFile::remove(pyPng);
    QProcess pr;
    pr.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
    pr.start("python", {pyMain, "frame", edJson, QString::number(T), "-o", pyPng});
    pr.waitForFinished(120000);
    if (QFileInfo::exists(pyPng)) {
        QImage py(pyPng);
        edited.save(work + "/n_edit.png");   // 差の調べ用に残す
        const double ps = psnr(edited, py);
        std::printf("  Python 版との PSNR %.2f dB\n", ps);
        CHECK(ps >= 35, "編集後の絵が Python 版と一致(PSNR>=35)");
        edited.save(work + "/n_edit.png");
    } else { CHECK(false, "Python 版の frame が出せた"); std::printf("%s\n", pr.readAllStandardError().constData()); }

    // ---- 3) 反映の遅れ(編集 → serialize → reload → 描画 → 取り出し) ----
    std::vector<double> msText, msDecode;
    for (int i = 0; i < 12; ++i) {
        QElapsedTimer t; t.start();
        model.mutate([&] { model.d().get("telops")->at(1).set("text", OVal::str(QStringLiteral("文字%1").arg(i))); });
        r.render(T, false); r.grab();
        msText.push_back(t.nsecsElapsed() * 1e-6);
    }
    for (int i = 0; i < 6; ++i) {
        QElapsedTimer t; t.start();
        model.mutate([&] { model.d().get("overlays")->at(0).set("in", OVal::num(10 + i * 7)); });
        r.render(T, true); r.grab();
        msDecode.push_back(t.nsecsElapsed() * 1e-6);
    }
    auto med = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
    auto mx = [](const std::vector<double>& v) { return *std::max_element(v.begin(), v.end()); };
    std::printf("  文字の編集→次の絵: 中央 %.1f ms / 最大 %.1f ms (うち reload %.1f ms)\n", med(msText), mx(msText), r.lastReloadMs());
    std::printf("  素材の位置の編集→次の絵(新しいコマを待つ): 中央 %.1f ms / 最大 %.1f ms\n", med(msDecode), mx(msDecode));
    CHECK(med(msText) < 60, "文字の編集は 1〜2 コマ内(中央 60ms 未満)で絵に出る");
    CHECK(mx(msText) < 250, "文字の編集の最悪でも 250ms 未満(旧: 700ms 待ち)");
    CHECK(med(msDecode) < 300, "素材の位置を変えても中央 300ms 未満");
    r.releaseGL();

    // ---- 4) 音量メーター(Null 出力。デバイスは開かない) ----
    {
        vn::Renderer rr;
        QString e2;
        rr.load(proj, &e2);
        vn::AudioPlayer ap;
        if (!ap.init(rr.project(), rr.timeline(), &e2) || !ap.open(vn::AudioPlayer::Sink::Null)) { CHECK(false, "AudioPlayer(Null)が開ける"); }
        else {
            ap.play(1.0);
            QElapsedTimer w; w.start();
            while (!ap.streaming() && w.elapsed() < 5000) QThread::msleep(2);
            auto collect = [&](int ms, double* rmsMean) {
                double pk = 0, sq = 0; int n = 0;
                (void)ap.takeLevels();
                QElapsedTimer q; q.start();
                while (q.elapsed() < ms) {
                    QThread::msleep(16);
                    auto lv = ap.takeLevels();
                    if (!lv.valid) continue;
                    pk = std::max({pk, lv.peak[0], lv.peak[1]});
                    sq += 0.5 * (lv.rms[0] * lv.rms[0] + lv.rms[1] * lv.rms[1]); ++n;
                }
                *rmsMean = n ? std::sqrt(sq / n) : 0;
                return pk;
            };
            double rms1 = 0, rmsQ = 0, rmsM = 0;
            const double c0 = ap.clock();
            const double pk1 = collect(800, &rms1);
            const double c1 = ap.clock();
            ap.setVolume(0.25, false); QThread::msleep(100);
            const double pkQ = collect(500, &rmsQ);
            ap.setVolume(1.0, true); QThread::msleep(100);
            const double pkM = collect(300, &rmsM);
            ap.close();
            // 基準: 同じ区間を AudioMixer から直接読んだ実際の音
            vn::AudioMixer m;
            m.init(rr.project(), rr.timeline(), &e2);
            const int64_t s0 = (int64_t)std::llround(c0 * vn::kSampleRate), n = (int64_t)std::llround((c1 - c0) * vn::kSampleRate);
            m.seek(s0);
            std::vector<float> l(4096), rg(4096);
            double refPk = 0, refSq = 0; int64_t cnt = 0;
            for (int64_t i = 0; i < n; i += 4096) {
                const int k = (int)std::min<int64_t>(4096, n - i);
                m.read(l.data(), rg.data(), k);
                for (int j = 0; j < k; ++j) { refPk = std::max({refPk, (double)std::fabs(l[j]), (double)std::fabs(rg[j])}); refSq += 0.5 * ((double)l[j] * l[j] + (double)rg[j] * rg[j]); ++cnt; }
            }
            const double refRms = cnt ? std::sqrt(refSq / cnt) : 0;
            std::printf("  メーター peak %.4f (基準 %.4f) / rms %.4f (基準 %.4f)  音量1/4: peak %.4f rms %.4f  ミュート: peak %.5f\n", pk1, refPk, rms1, refRms, pkQ, rmsQ, pkM);
            CHECK(refPk > 0.01, "基準の音が鳴っている区間");
            CHECK(std::fabs(pk1 - refPk) <= 0.12 * refPk + 0.003, "メーターの peak が実際の音と合う(±12%)");
            CHECK(std::fabs(rms1 - refRms) <= 0.25 * refRms + 0.002, "メーターの rms が実際の音と合う(±25%)");
            CHECK(pkQ > 0 && pkQ < pk1 * 0.5, "音量を下げるとメーターが下がる");
            CHECK(pkM < 0.001, "ミュートでメーターが 0");
        }
    }
    std::printf(fails ? "\nFAILED: %d\n" : "\nALL OK\n", fails);
    return fails ? 1 : 0;
}
