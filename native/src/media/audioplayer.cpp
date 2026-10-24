#include "audioplayer.h"
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <mutex>
#include <thread>
#include "media/qtthread.h"
#include <vector>
#include "audiomixer.h"

namespace vn {

namespace {
constexpr int kRing = 1 << 16;      // フレーム数(約1.4秒)
constexpr int kChunk = 2048;        // ミキサーから一度に取る数
constexpr int kRamp = 480;          // 10ms
constexpr int kStartFill = 4096;    // 流し始める前にためる数(約85ms)
}

struct AudioPlayer::Impl {
    std::unique_ptr<AudioMixer> mixer = std::make_unique<AudioMixer>();
    int64_t total = 0;

    // ---- 共有状態(m で守る)
    mutable std::mutex m;
    std::condition_variable_any cv;
    std::vector<float> ring = std::vector<float>((size_t)kRing * 2);
    int rhead = 0, rsize = 0;
    uint64_t gen = 0;               // シークのたびに増える(古い生成結果を捨てるため)
    bool seekReq = false;
    int64_t seekTarget = 0;
    int64_t prodPos = 0;            // 次にミキサーから出るサンプル位置(リングの末尾の次)
    bool mixEof = false;            // ミキサーが終端まで出し終えた(シーク待ちの間も true にして生成を止める)
    bool wantPlay = false;
    bool streamingNow = false;      // バッファがたまって流れている
    bool ended = false;
    int64_t pos = 0;                // 出力が消費した位置(次に鳴るサンプル)
    std::vector<float> tail;        // シーク前の音の尾(フェードアウト済み)
    size_t tailPos = 0;
    float gain = 0;
    float volGain = 1, volCur = 1;   // 音量(2乗済み)・ミュート反映後の目標と現在値(出力側で 10ms 程度かけて近づける)
    double rate = 1, frac = 0;       // 倍速(速度に応じて音程も変わる補間再生)と、読み位置の小数部
    int nUnder = 0;
    float peak = 0;                  // 直近 pull の最大振幅(確認用)
    std::atomic<bool> run{false};
    std::atomic<bool> prodRun{false};   // ミキサースレッドを動かす(run は出力側と共通。reinit ではこちらだけ止める)
    // メーター用: 出力へ渡した音(音量・フェードを掛けた後)の L/R の最大振幅と二乗和。takeLevels で取り出して 0 に戻す
    float lvPeak[2] = {0, 0};
    double lvSq[2] = {0, 0};
    int64_t lvN = 0;
    AudioPlayer::Levels lastLv;
    std::atomic<bool> procWant{true};   // 処理後の音を使いたい(UI)
    bool procCur = true;                // ミキサーに反映済み(ミキサースレッドだけが触る)
    vn::Thread prod;
    std::unique_ptr<AudioMixer> nextMixer;   // reinit で作った新しいミキサー。ミキサースレッドが受け取って入れ替える
    QString mixInfo;

    // ---- 出力
    Sink kind = Sink::Null;
    vn::Thread nullThr;
    std::unique_ptr<QAudioSink> sink;
    QIODevice* sinkDev = nullptr;
    std::atomic<int64_t> pulled{0};      // 出力に渡した総フレーム数
    std::chrono::steady_clock::time_point lastPull{};

    // ミキサースレッド
    void producer() {
        std::vector<float> l(kChunk), r(kChunk);
        for (;;) {
            bool doSeek = false;
            int64_t tgt = 0;
            uint64_t g = 0;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait_for(lk, std::chrono::milliseconds(4), [&] { return !prodRun || nextMixer || seekReq || (rsize + kChunk <= kRing && !mixEof); });
                if (!prodRun) return;
                if (nextMixer) {   // 入れ替えはこのスレッドでやる(スレッドを止めて作り直すと、終わる時に DLL の鍵待ちで 10 秒ほど固まった)
                    std::unique_ptr<AudioMixer> old = std::move(mixer);
                    mixer = std::move(nextMixer);
                    procCur = true;   // 新しいミキサーは処理後の音で始まる。seek の前に procWant へ合わせる
                    lk.unlock();
                    old.reset();      // 古いミキサーの後始末も UI を止めないようこちらで
                    continue;
                }
                if (seekReq) { doSeek = true; tgt = seekTarget; seekReq = false; g = gen; }
                else if (rsize + kChunk > kRing || mixEof) continue;
                else g = gen;
            }
            if (doSeek) {
                if (procWant.load() != procCur) { procCur = procWant.load(); mixer->setProcessed(procCur); }
                mixer->seek(tgt);
                std::lock_guard<std::mutex> lk(m);
                if (g == gen) { prodPos = tgt; mixEof = tgt >= total; }
                continue;
            }
            mixer->read(l.data(), r.data(), kChunk);
            std::lock_guard<std::mutex> lk(m);
            if (g != gen) continue;   // 読んでいる間にシークされた → 捨てる(次の周でシーク処理)
            for (int i = 0; i < kChunk; ++i) {
                const int w = (rhead + rsize + i) % kRing;
                ring[(size_t)w * 2] = l[i];
                ring[(size_t)w * 2 + 1] = r[i];
            }
            rsize += kChunk;
            prodPos += kChunk;
            if (prodPos >= total) mixEof = true;
        }
    }

    // 出力側: n フレームぶんを out(インターリーブ)に出す
    void pull(float* out, int n) {
        std::lock_guard<std::mutex> lk(m);
        lastPull = std::chrono::steady_clock::now();
        int i = 0;
        peak = 0;
        // シーク前の尾
        while (i < n && tailPos < tail.size() / 2) {
            out[i * 2] = tail[tailPos * 2];
            out[i * 2 + 1] = tail[tailPos * 2 + 1];
            ++tailPos; ++i; ++pos;
        }
        if (!tail.empty() && tailPos >= tail.size() / 2) { tail.clear(); tailPos = 0; }
        const bool tailing = !tail.empty();
        if (!streamingNow && wantPlay && !tailing && !seekReq) {
            if (rsize >= kStartFill || (mixEof && !seekReq && prodPos >= total)) streamingNow = true;
        }
        for (; i < n; ++i) {
            if (tailing) break;
            const float tg = (wantPlay && streamingNow) ? 1.0f : 0.0f;
            if (gain < tg) gain = std::min(tg, gain + 1.0f / kRamp);
            else if (gain > tg) gain = std::max(tg, gain - 1.0f / kRamp);
            float a = 0, b = 0;
            if ((gain > 0 || tg > 0) && streamingNow) {
                if (rsize > 0) {
                    a = ring[(size_t)rhead * 2];
                    b = ring[(size_t)rhead * 2 + 1];
                    if (rate != 1.0) {
                        if (rsize > 1) {
                            const int n1 = (rhead + 1) % kRing;
                            const float f = (float)frac;
                            a += (ring[(size_t)n1 * 2] - a) * f;
                            b += (ring[(size_t)n1 * 2 + 1] - b) * f;
                        }
                        frac += rate;
                        while (frac >= 1.0 && rsize > 0) { frac -= 1.0; rhead = (rhead + 1) % kRing; --rsize; ++pos; }
                    } else {
                        rhead = (rhead + 1) % kRing; --rsize; ++pos;
                    }
                } else if (prodPos >= total) {
                    ended = true;   // 終端まで流した
                    wantPlay = false; streamingNow = false;
                } else if (tg > 0) {
                    ++nUnder;       // ミキサーが間に合わなかった → 無音を挟み、たまるまで待つ(位置は進めない)
                    streamingNow = false;
                }
            }
            if (volCur < volGain) volCur = std::min(volGain, volCur + 1.0f / kRamp);
            else if (volCur > volGain) volCur = std::max(volGain, volCur - 1.0f / kRamp);
            out[i * 2] = a * gain * volCur;
            out[i * 2 + 1] = b * gain * volCur;
        }
        for (; i < n; ++i) { out[i * 2] = out[i * 2 + 1] = 0; }
        for (int k = 0; k < n * 2; ++k) peak = std::max(peak, std::fabs(out[k]));
        for (int k = 0; k < n; ++k) {
            for (int c = 0; c < 2; ++c) {
                const float v = out[k * 2 + c];
                lvPeak[c] = std::max(lvPeak[c], std::fabs(v));
                lvSq[c] += (double)v * v;
            }
        }
        lvN += n;
        cv.notify_all();
    }

    double clockNow() const {
        std::lock_guard<std::mutex> lk(m);
        double p = (double)pos;
        if (kind == Sink::Null) {
            if (wantPlay && streamingNow) {
                const double e = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastPull).count();
                p += std::min(e, 0.02) * kSampleRate * rate;
            }
        } else if (sink) {
            const int64_t proc = (int64_t)((double)sink->processedUSecs() * 1e-6 * kSampleRate);
            p -= (double)std::max<int64_t>(0, pulled.load() - proc) * rate;
        }
        return std::max(0.0, std::min(p, (double)total)) / kSampleRate;
    }
};

namespace {
class PullDevice : public QIODevice {
public:
    explicit PullDevice(AudioPlayer::Impl* d) : d_(d) { open(QIODevice::ReadOnly); }
    qint64 readData(char* data, qint64 maxlen) override {
        const int n = (int)(maxlen / 8);
        d_->pull(reinterpret_cast<float*>(data), n);
        d_->pulled += n;
        return (qint64)n * 8;
    }
    qint64 writeData(const char*, qint64) override { return -1; }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 16; }
private:
    AudioPlayer::Impl* d_;
};
}  // namespace

AudioPlayer::AudioPlayer() : d(new Impl) {}
AudioPlayer::~AudioPlayer() { close(); }

bool AudioPlayer::init(const Project& p, const Timeline& tl, QString* err) {
    if (!d->mixer->init(p, tl, err)) return false;
    d->total = d->mixer->totalSamples();
    d->mixInfo = d->mixer->info();
    d->prodPos = 0;
    d->mixEof = d->total <= 0;
    d->run = true;
    d->prodRun = true;
    d->prod = vn::Thread([this] { d->producer(); });
    return true;
}

// 編集で project が変わった: 新しいミキサーを作って入れ替える(出力は開いたまま。位置はそのまま、リングは空にして流し直す)。
// 重い処理(ミキサーの組み立て)は古いミキサーを動かしたまま先にやる。再生中なら呼び出し側で pause しておくこと
bool AudioPlayer::reinit(const Project& p, const Timeline& tl, QString* err) {
    auto nm = std::make_unique<AudioMixer>();
    if (!nm->init(p, tl, err)) return false;
    const double keep = clock();
    d->mixInfo = nm->info();
    {
        std::lock_guard<std::mutex> lk(d->m);
        d->total = nm->totalSamples();
        d->nextMixer = std::move(nm);   // ミキサースレッドが次に起きた時に入れ替える(下の seek で古い音は捨てられる)
    }
    d->cv.notify_all();
    if (!d->prod.joinable()) { d->prodRun = true; d->prod = vn::Thread([this] { d->producer(); }); }
    seek(std::min(keep, (double)d->total / kSampleRate));
    return true;
}

AudioPlayer::Levels AudioPlayer::takeLevels() {
    std::lock_guard<std::mutex> lk(d->m);
    if (d->lvN > 0) {
        Levels L;
        L.valid = true;
        for (int c = 0; c < 2; ++c) {
            L.peak[c] = d->lvPeak[c];
            L.rms[c] = std::sqrt(d->lvSq[c] / (double)d->lvN);
            d->lvPeak[c] = 0;
            d->lvSq[c] = 0;
        }
        d->lvN = 0;
        d->lastLv = L;
    }
    return d->lastLv;
}

bool AudioPlayer::open(Sink kind) {
    d->kind = kind;
    if (kind == Sink::Null) {
        d->nullThr = vn::Thread([this] {
            // 実時間で出力要求を満たす(デバイスは開かない。PCM は捨てる)
            std::vector<float> buf(4800 * 2);
            const auto t0 = std::chrono::steady_clock::now();
            int64_t delivered = 0;
            while (d->run) {
                const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const int64_t need = (int64_t)(el * kSampleRate) - delivered;
                if (need >= 240) {
                    const int n = (int)std::min<int64_t>(need, 4800);
                    d->pull(buf.data(), n);
                    delivered += n;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        });
        return true;
    }
    QAudioFormat f;
    f.setSampleRate(kSampleRate);
    f.setChannelCount(2);
    f.setSampleFormat(QAudioFormat::Float);
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return false;
    d->sink = std::make_unique<QAudioSink>(dev, f);
    d->sink->setBufferSize(kSampleRate / 10 * 8);   // 約100ms
    d->sinkDev = new PullDevice(d.get());
    d->sink->start(d->sinkDev);
    return true;
}

void AudioPlayer::close() {
    if (d->sink) { d->sink->stop(); d->sink.reset(); }
    delete d->sinkDev;
    d->sinkDev = nullptr;
    d->run = false;
    d->prodRun = false;
    d->cv.notify_all();
    if (d->nullThr.joinable()) d->nullThr.join();
    if (d->prod.joinable()) d->prod.join();
}

bool AudioPlayer::processed() const { return d->procWant.load(); }

void AudioPlayer::setProcessed(bool on) {
    if (d->procWant.exchange(on) == on) return;
    seek(clock());   // ミキサースレッドがシークの前に音源を入れ替える
}

void AudioPlayer::seek(double t) {
    std::lock_guard<std::mutex> lk(d->m);
    const int64_t s = std::max<int64_t>(0, std::min<int64_t>(d->total, (int64_t)std::llround(t * kSampleRate)));
    d->tail.clear();
    d->tailPos = 0;
    d->pos = s;
    // 鳴っている最中なら、直前の音の尾を短くフェードアウトさせる(尾を出し終えたところで位置が s になる)
    if (d->streamingNow && d->wantPlay && d->gain > 0 && d->rsize > 0) {
        const int k = std::min(kRamp, d->rsize);
        d->tail.resize((size_t)k * 2);
        for (int i = 0; i < k; ++i) {
            const int idx = (d->rhead + i) % kRing;
            const float g = d->gain * (float)(k - i) / (float)k;
            d->tail[(size_t)i * 2] = d->ring[(size_t)idx * 2] * g;
            d->tail[(size_t)i * 2 + 1] = d->ring[(size_t)idx * 2 + 1] * g;
        }
        d->pos = s - k;
    }
    d->rhead = 0;
    d->rsize = 0;
    d->frac = 0;
    ++d->gen;
    d->seekReq = true;
    d->seekTarget = s;
    d->prodPos = s;
    d->mixEof = true;   // シーク完了までは生成させない(producer が戻す)
    d->gain = 0;
    d->streamingNow = false;
    d->ended = false;
    d->cv.notify_all();
}

void AudioPlayer::play(double t) {
    seek(t);
    std::lock_guard<std::mutex> lk(d->m);
    d->wantPlay = true;
    d->cv.notify_all();
}

void AudioPlayer::pause() {
    std::lock_guard<std::mutex> lk(d->m);
    d->wantPlay = false;   // 出力側が 10ms でゲインを下げ、0 になったら消費を止める
}

void AudioPlayer::setVolume(double gain, bool muted) {
    std::lock_guard<std::mutex> lk(d->m);
    d->volGain = muted ? 0.0f : (float)std::max(0.0, std::min(4.0, gain));
}
void AudioPlayer::setRate(double r) {
    std::lock_guard<std::mutex> lk(d->m);
    d->rate = std::max(0.25, std::min(8.0, r));
    d->frac = 0;
}
double AudioPlayer::peak() const { std::lock_guard<std::mutex> lk(d->m); return d->peak; }
double AudioPlayer::rate() const { std::lock_guard<std::mutex> lk(d->m); return d->rate; }
double AudioPlayer::volume() const { std::lock_guard<std::mutex> lk(d->m); return d->volGain; }

bool AudioPlayer::playing() const { std::lock_guard<std::mutex> lk(d->m); return d->wantPlay; }
bool AudioPlayer::streaming() const { std::lock_guard<std::mutex> lk(d->m); return d->wantPlay && d->streamingNow; }
bool AudioPlayer::finished() const { std::lock_guard<std::mutex> lk(d->m); return d->ended; }
double AudioPlayer::clock() const { return d->clockNow(); }
int AudioPlayer::underruns() const { std::lock_guard<std::mutex> lk(d->m); return d->nUnder; }
double AudioPlayer::totalSeconds() const { return (double)d->total / kSampleRate; }
QString AudioPlayer::info() const { return d->mixInfo; }

}  // namespace vn
