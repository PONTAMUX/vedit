// vedit-native: 窓あり再生 / --frame / --bench(窓なし)
#include "stallwatch.h"
#include <QOpenGLExtraFunctions>
#include <deque>
#include <QApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QThread>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QSurfaceFormat>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include "mainwindow.h"
#include "media/audiomixer.h"
#include "media/audioplayer.h"
#include "renderer.h"

#ifdef _WIN32
#include <windows.h>
#endif

static double cpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto f = [](FILETIME t) { return (((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime) * 1e-7; };
    return f(k) + f(u);
#else
    return 0;
#endif
}

// 次のコマの時刻まで寝る(高精度タイマー。無ければ msleep)。vsync 待ちの代わり
static void sleepSec(double sec) {
    if (sec <= 0) return;
#ifdef _WIN32
    static HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (tm) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(sec * 1e7);
        if (SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(tm, 100); return; }
    }
#endif
    QThread::msleep((unsigned long)std::max(1.0, sec * 1000));
}

struct OffGL {
    QOpenGLContext ctx;
    QOffscreenSurface surf;
    bool init() {
        surf.setFormat(QSurfaceFormat::defaultFormat());
        surf.create();
        ctx.setFormat(QSurfaceFormat::defaultFormat());
        if (!ctx.create()) return false;
        return ctx.makeCurrent(&surf);
    }
};

static int runFrame(const QString& proj, double t, const QString& out) {
    OffGL gl;
    if (!gl.init()) { fprintf(stderr, "GL コンテキストを作れません\n"); return 2; }
    vn::Renderer r;
    QString err;
    if (!r.load(proj, &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    if (!r.initGL()) { fprintf(stderr, "initGL 失敗\n"); return 4; }
    auto& tl = r.timeline();
    const double last = std::max(0.0, tl.total - 1.0 / tl.fps);
    const double T = std::min(last, std::ceil(t * tl.fps - 1e-6) / tl.fps);
    if (qEnvironmentVariableIsSet("VN_DEBUG")) {
        for (auto& p : tl.pieces) fprintf(stderr, "piece gap=%d a=%.3f b=%.3f o=%.3f..%.3f\n", p.gap, p.a, p.b, p.o0, p.o1);
        auto v = tl.v1At(T);
        fprintf(stderr, "v1 black=%d piece=%d srcT=%.4f fps=%.3f srcfps=%.3f\n", v.black, v.piece, v.srcT, tl.fps, tl.src.fps);
    }
    r.render(T, true);
    QImage im = r.grab().convertToFormat(QImage::Format_RGB32);
    const bool ok = im.save(out);
    printf("{\"ok\":%s,\"T\":%.5f,\"w\":%d,\"h\":%d,\"out\":\"%s\"}\n", ok ? "true" : "false", T, im.width(), im.height(),
           qPrintable(out));
    r.releaseGL();
    return ok ? 0 : 5;
}


// 32bit float stereo wav を書く(確認用。デバイスは開かない)
static bool writeWavF32(const QString& path, const std::vector<float>& inter, int sr) {
    FILE* f = fopen(path.toLocal8Bit().constData(), "wb");
    if (!f) return false;
    const unsigned dataBytes = (unsigned)(inter.size() * 4);
    auto w32 = [&](unsigned v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](unsigned short v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
    w32(16); w16(3); w16(2); w32(sr); w32(sr * 8); w16(8); w16(32);
    fwrite("data", 1, 4, f); w32(dataBytes);
    fwrite(inter.data(), 4, inter.size(), f);
    fclose(f);
    return true;
}

static int runAudio(const QString& proj, double start, double secs, const QString& out) {
    vn::Renderer r;
    QString err;
    if (!r.load(proj, &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    vn::AudioMixer m;
    if (!m.init(r.project(), r.timeline(), &err)) { fprintf(stderr, "audio: %s\n", qPrintable(err)); return 6; }
    fprintf(stderr, "%s\n", qPrintable(m.info()));
    const int64_t s0 = (int64_t)std::llround(start * vn::kSampleRate);
    int64_t n = (int64_t)std::llround(secs * vn::kSampleRate);
    n = std::max<int64_t>(0, std::min<int64_t>(n, m.totalSamples() - s0));
    QElapsedTimer t;
    t.start();
    m.seek(s0);
    std::vector<float> inter((size_t)n * 2), l(4096), rr(4096);
    for (int64_t i = 0; i < n; i += 4096) {
        const int k = (int)std::min<int64_t>(4096, n - i);
        m.read(l.data(), rr.data(), k);
        for (int j = 0; j < k; ++j) { inter[(i + j) * 2] = l[j]; inter[(i + j) * 2 + 1] = rr[j]; }
    }
    const double ms = t.nsecsElapsed() * 1e-6;
    const bool ok = writeWavF32(out, inter, vn::kSampleRate);
    printf("{\"ok\":%s,\"start\":%.4f,\"samples\":%lld,\"make_ms\":%.1f,\"out\":\"%s\"}\n", ok ? "true" : "false", (double)s0 / vn::kSampleRate,
           (long long)n, ms, qPrintable(out));
    return ok ? 0 : 5;
}

// G4/G5 の確認: Null 出力で音量・ミュート・倍速を切り替え、位置と最大振幅を見る(デバイスは開かない)
static int runAudioTest(const QString& proj, double start) {
    vn::Renderer r;
    QString err;
    if (!r.load(proj, &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    vn::AudioPlayer ap;
    if (!ap.init(r.project(), r.timeline(), &err) || !ap.open(vn::AudioPlayer::Sink::Null)) { fprintf(stderr, "audio: %s\n", qPrintable(err)); return 4; }
    printf("%s\n", qPrintable(ap.info()));
    ap.play(start);
    QElapsedTimer w;
    w.start();
    while (!ap.streaming() && w.elapsed() < 5000) QThread::msleep(2);
    auto peakOver = [&](int ms) { double mx = 0; QElapsedTimer q; q.start(); while (q.elapsed() < ms) { mx = std::max(mx, ap.peak()); QThread::msleep(2); } return mx; };
    auto rateOf = [&](double r) { ap.setRate(r); QThread::msleep(100); const double c0 = ap.clock(); QElapsedTimer q; q.start(); QThread::msleep(1000); return (ap.clock() - c0) / (q.nsecsElapsed() * 1e-9); };
    const double pk1 = peakOver(400);
    ap.setVolume(0.25, false);  QThread::msleep(100);
    const double pkQ = peakOver(400);
    ap.setVolume(1.0, true);    QThread::msleep(100);
    const double pkM = peakOver(400);
    ap.setVolume(1.0, false);   QThread::msleep(100);
    const double r2 = rateOf(2.0), r05 = rateOf(0.5), r1 = rateOf(1.0);
    printf("{\"peak_full\":%.4f,\"peak_quarter\":%.4f,\"peak_muted\":%.4f,\"rate2\":%.3f,\"rate0.5\":%.3f,\"rate1\":%.3f,\"underruns\":%d}\n",
           pk1, pkQ, pkM, r2, r05, r1, ap.underruns());
    // c-proc: 処理後 → 生 → 処理後 に切り替えて、位置が飛ばずに流れ続け音が出ることを見る
    const double cA = ap.clock();
    ap.setProcessed(false);
    QThread::msleep(300);
    const double pkRaw = peakOver(500);
    const double cB = ap.clock();
    ap.setProcessed(true);
    QThread::msleep(300);
    const double pkProc = peakOver(500);
    const double cC = ap.clock();
    printf("{\"proc_clock\":[%.3f,%.3f,%.3f],\"peak_raw\":%.4f,\"peak_proc\":%.4f,\"processed\":%d,\"underruns2\":%d}\n", cA, cB, cC, pkRaw, pkProc, (int)ap.processed(), ap.underruns());
    ap.close();
    return 0;
}

// 編集後の最初の再生で音を作り直す時間(reinit)と、鳴り始めるまでの時間を測る。音は出さない(Null)
static int runReinitBench(const QString& proj, double start) {
    vn::Renderer r;
    QString err;
    if (!r.load(proj, &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    vn::AudioPlayer ap;
    if (!ap.init(r.project(), r.timeline(), &err) || !ap.open(vn::AudioPlayer::Sink::Null)) { fprintf(stderr, "audio: %s\n", qPrintable(err)); return 4; }
    for (int k = 0; k < 3; ++k) {
        QElapsedTimer q; q.start();
        if (!ap.reinit(r.project(), r.timeline(), &err)) { fprintf(stderr, "reinit: %s\n", qPrintable(err)); return 5; }
        const double ri = q.nsecsElapsed() * 1e-6;
        q.restart();
        ap.play(start + k * 60);
        while (!ap.streaming() && q.elapsed() < 20000) QThread::msleep(2);
        printf("reinit %.1f ms / 鳴り始め %.1f ms\n", ri, q.nsecsElapsed() * 1e-6);
        ap.pause();
    }
    ap.close();
    return 0;
}

static int runBench(const QString& proj, double start, double secs) {
    OffGL gl;
    if (!gl.init()) { fprintf(stderr, "GL コンテキストを作れません\n"); return 2; }
    vn::Renderer r;
    QString err;
    if (!r.load(proj, &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    if (!r.initGL()) { fprintf(stderr, "initGL 失敗\n"); return 4; }
    r.waitDecodersWarm(15000);   // 再生中に NVDEC を作って止まらないよう、差し込みのデコーダを先に作り終えるのを待つ
    auto& tl = r.timeline();
    const double fps = tl.fps;
    start = std::min(std::max(0.0, start), std::max(0.0, tl.total - secs));
    const int s0 = (int)std::ceil(start * fps - 1e-6);
    const int n = (int)(secs * fps);
    QOpenGLFramebufferObject dst(1280, 720);
    auto* f = gl.ctx.functions();
    auto* ef = gl.ctx.extraFunctions();
    const bool g_fenceMode = !qEnvironmentVariableIsSet("VN_BENCH_FINISH");   // 既定は 2 コマ先行(fence)。VN_BENCH_FINISH=1 で従来の glFinish
    std::deque<GLsync> fences;
    const int kInFlight = qEnvironmentVariableIntValue("VN_BENCH_INFLIGHT") > 0 ? qEnvironmentVariableIntValue("VN_BENCH_INFLIGHT") : 2;   // スワップチェーンと同じ 2 コマ先まで
    const bool tr = qEnvironmentVariableIsSet("VN_TRACE");
    const bool nullDraw = qEnvironmentVariableIsSet("VN_BENCH_NULLDRAW");
    QElapsedTimer tq;
    tq.start();
    auto draw = [&](int idx, bool wait) {
        const double q0 = tq.nsecsElapsed() * 1e-6;
        if (nullDraw) { dst.bind(); f->glClearColor(0.1f * (idx & 7), 0, 0, 1); f->glClear(GL_COLOR_BUFFER_BIT); }   // 比較用: 何も描かずクリアだけ(環境の GPU 待ちの切り分け)
        else r.render((s0 + idx) / fps, wait);
        const double q1 = tq.nsecsElapsed() * 1e-6;
        dst.bind();
        r.blit(dst.handle(), 0, 0, 1280, 720);
        const double q2 = tq.nsecsElapsed() * 1e-6;
        // 実アプリのスワップチェーン相当: 2 コマ先まで GPU に積んでよい(glFinish で毎コマ待つと、他のアプリが GPU を使った時の 10〜60 ms の待ちをそのまま「落ち」に数えてしまう)
        if (g_fenceMode) {
            f->glFlush();
            GLsync fs = ef->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            fences.push_back(fs);
            if ((int)fences.size() > kInFlight) { ef->glClientWaitSync(fences.front(), GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull); ef->glDeleteSync(fences.front()); fences.pop_front(); }
        } else f->glFinish();
        const double q3 = tq.nsecsElapsed() * 1e-6;
        if (tr && q3 - q0 > 12) fprintf(stderr, "BSLOW idx=%d render=%.1f blit=%.1f finish=%.1f\n", idx, q1 - q0, q2 - q1, q3 - q2);
    };
    QElapsedTimer w;
    w.start();
    r.setPrewarm(true);
    draw(0, true);   // 暖機(最初の1コマはデコーダ起動を待つ。次の区間・差し込みの先読みもここで始まる)
    QThread::msleep(700);   // 再生ボタンを押すまでの間(先読みの仕込み時間)
    printf("warmup %.0f ms\n", w.nsecsElapsed() * 1e-6);
    // 音声(出力は Null: デバイスを開かず実時間で捨てる)。音声の時計が親
    vn::AudioPlayer ap;
    bool hasAudio = false;
    {
        QElapsedTimer ai;
        ai.start();
        QString aerr;
        if (!qEnvironmentVariableIsSet("VN_NOAUDIO") && ap.init(r.project(), tl, &aerr) && ap.open(vn::AudioPlayer::Sink::Null)) {
            hasAudio = true;
            printf("audio init %.0f ms (%s)\n", ai.nsecsElapsed() * 1e-6, qPrintable(ap.info()));
        } else {
            printf("audio: なし (%s)\n", qPrintable(aerr));
        }
    }
    const double tS = s0 / fps;
    if (hasAudio) {
        ap.play(tS);
        QElapsedTimer wt;
        wt.start();
        while (!ap.streaming() && wt.elapsed() < 5000) QThread::msleep(2);
    }
    const int stale0 = r.staleCount();
    const double cpu0 = cpuSeconds();
    QElapsedTimer clk;
    clk.start();
    int shown = 0, dropped = 0, last = 0;
    double sumMs = 0, maxMs = 0, skewMax = 0, skewSum = 0;
    auto nowSec = [&] { return hasAudio ? ap.clock() - tS : clk.nsecsElapsed() * 1e-9; };
    while (true) {
        const int want = (int)(nowSec() * fps + 1e-9);
        if (want >= n) break;
        if (want <= last && shown > 0) {
            sleepSec((last + 1) / fps - nowSec());
            continue;
        }
        if (shown > 0) dropped += want - last - 1;
        last = want;
        QElapsedTimer one;
        one.start();
        draw(want, false);
        const double ms = one.nsecsElapsed() * 1e-6;
        const double skew = (nowSec() - want / fps) * 1e3;   // 描き終わりの音声の時計 − そのコマの時刻(遅れが正)
        if (tr && (skew > 20 || skew < -5)) fprintf(stderr, "SKEW idx=%d skew=%.1f draw=%.1f\n", want, skew, ms);
        skewMax = std::max(skewMax, std::fabs(skew));
        skewSum += std::fabs(skew);
        sumMs += ms;
        maxMs = std::max(maxMs, ms);
        ++shown;
    }
    const double el = clk.nsecsElapsed() * 1e-9;
    const double cpu = cpuSeconds() - cpu0;
    const int under = hasAudio ? ap.underruns() : -1;
    printf("{\"audio\":%s,\"underruns\":%d,\"av_skew_max_ms\":%.1f,\"av_skew_avg_ms\":%.1f}\n", hasAudio ? "true" : "false", under, skewMax,
           shown ? skewSum / shown : 0);
    printf("{\"project_fps\":%.3f,\"start\":%.2f,\"seconds\":%.2f,\"shown\":%d,\"avg_fps\":%.2f,\"dropped\":%d,"
           "\"stale\":%d,\"avg_ms\":%.2f,\"max_ms\":%.1f,\"cpu_s\":%.2f,\"cpu_pct\":%.1f,\"cpu_pct_all\":%.2f,\"interop\":%d}\n",
           fps, start, el, shown, shown / el, dropped, r.staleCount() - stale0, shown ? sumMs / shown : 0, maxMs, cpu,
           100.0 * cpu / el, 100.0 * cpu / el / std::max(1u, std::thread::hardware_concurrency()), r.interopFrames());
    ap.close();
    r.releaseGL();
    return 0;
}

// シークバーのドラッグの再現: 出力時刻を step 秒ずつ動かして、1回ごとに止め絵(wait=true)を描く時間を測る。proxy を渡すとプレビュー用の軽い映像で
static int runScrub(const QString& proj, double start, double step, int n, const QString& proxy) {
    OffGL gl;
    if (!gl.init()) { fprintf(stderr, "GL コンテキストを作れません\n"); return 2; }
    vn::Renderer r;
    QString err;
    if (!r.load(proj, &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    if (!r.initGL()) { fprintf(stderr, "initGL\n"); return 4; }
    if (!proxy.isEmpty()) r.setPreviewSource(r.sourcePath(), proxy);
    r.render(start, true);
    double sum = 0, mx = 0;
    std::vector<double> v;
    for (int i = 1; i <= n; ++i) {
        QElapsedTimer t;
        t.start();
        r.render(std::min(start + i * step, r.timeline().total - 0.1), true);
        const double ms = t.nsecsElapsed() * 1e-6;
        v.push_back(ms); sum += ms; mx = std::max(mx, ms);
    }
    std::sort(v.begin(), v.end());
    printf("{\"proxy\":%s,\"n\":%d,\"avg_ms\":%.1f,\"p50_ms\":%.1f,\"p90_ms\":%.1f,\"max_ms\":%.1f}\n", proxy.isEmpty() ? "false" : "true", n,
           sum / n, v[v.size() / 2], v[v.size() * 9 / 10], mx);
    r.releaseGL();
    return 0;
}

int main(int argc, char** argv) {
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    fmt.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(fmt);
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);

    QStringList args;
    for (int i = 1; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    const bool cli = !args.isEmpty() && (args[0] == "--frame" || args[0] == "--bench" || args[0] == "--audio" || args[0] == "--audiotest" || args[0] == "--reinitbench" || args[0] == "--scrub");
    if (cli) {
        QGuiApplication app(argc, argv);
        if (args[0] == "--frame" && args.size() >= 4) return runFrame(args[1], args[2].toDouble(), args[3]);
        if (args[0] == "--bench" && args.size() >= 4) return runBench(args[1], args[2].toDouble(), args[3].toDouble());
        if (args[0] == "--scrub" && args.size() >= 5) return runScrub(args[1], args[2].toDouble(), args[3].toDouble(), args[4].toInt(), args.value(5));
        if (args[0] == "--reinitbench" && args.size() >= 3) return runReinitBench(args[1], args[2].toDouble());
        if (args[0] == "--audiotest" && args.size() >= 3) return runAudioTest(args[1], args[2].toDouble());
        if (args[0] == "--audio" && args.size() >= 5) return runAudio(args[1], args[2].toDouble(), args[3].toDouble(), args[4]);
        fprintf(stderr, "usage: vedit-native --frame <project.json> <sec> <out.png>\n"
                        "       vedit-native --bench <project.json> <startSec> <seconds>\n"
                        "       vedit-native --audio <project.json> <startSec> <seconds> <out.wav>\n");
        return 1;
    }
    QApplication app(argc, argv);
    if (args.isEmpty()) {
        fprintf(stderr, "usage: vedit-native <project.json>\n");
        return 1;
    }
    vn::Renderer r;
    QString err;
    if (!r.load(args[0], &err)) { fprintf(stderr, "load: %s\n", qPrintable(err)); return 3; }
    MainWindow w(&r, args[0]);
    w.show();
    startStallWatch();
    return app.exec();
}
