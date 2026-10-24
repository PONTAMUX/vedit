// vaudio: pedalboard の process() ループと、JUCE dsp の移植(float のまま)。
// 移植元は dsp.h を参照。式の順序・型(float/double の別)は元のソースに合わせてある。
#include "dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace vaudio {

namespace {

constexpr float kPi = 3.141592653589793238f;  // juce::MathConstants<float>::pi

// ---- juce::Decibels ----
inline float decibelsToGain(float db, float minusInf = -100.0f) {
    return db > minusInf ? std::pow(10.0f, db * 0.05f) : 0.0f;
}
inline float gainToDecibels(float g, float minusInf = -100.0f) {
    return g > 0.0f ? std::max(minusInf, std::log10(g) * 20.0f) : minusInf;
}

// JUCE_SNAP_TO_ZERO
inline void snapToZero(float& n) {
    if (!(n < -1.0e-8f || n > 1.0e-8f)) n = 0;
}

// ---------------------------------------------------------------- Gain
class GainPlugin : public Plugin {
public:
    explicit GainPlugin(float db) : gain(decibelsToGain(db)) {}
    void prepare(const Spec&) override {}
    int process(float** ch, int numCh, int n) override {
        for (int c = 0; c < numCh; c++)
            for (int i = 0; i < n; i++) ch[c][i] = ch[c][i] * gain;
        return n;
    }
    void reset() override {}
    float gain;
};

// ---------------------------------------------------------------- BallisticsFilter
struct Ballistics {
    enum Level { Peak, RMS };
    Level levelType = Peak;
    double sampleRate = 44100.0, expFactor = -0.142;
    float attackTime = 1.0f, releaseTime = 100.0f, cteAT = 0, cteRL = 0;
    std::vector<float> yold;

    float cte(float timeMs) const {
        return timeMs < 1.0e-3f ? 0.0f : static_cast<float>(std::exp(expFactor / timeMs));
    }
    void setAttack(float ms) { attackTime = ms; cteAT = cte(ms); }
    void setRelease(float ms) { releaseTime = ms; cteRL = cte(ms); }
    void prepare(const Spec& s) {
        sampleRate = s.sampleRate;
        expFactor = -2.0 * 3.141592653589793238462643383279502884 * 1000.0 / sampleRate;
        setAttack(attackTime);
        setRelease(releaseTime);
        yold.assign(s.numChannels, 0.0f);
    }
    void reset() { for (auto& y : yold) y = 0.0f; }
    float processSample(int ch, float in) {
        if (levelType == RMS) in *= in;
        else in = std::abs(in);
        float c = (in > yold[ch] ? cteAT : cteRL);
        float result = in + c * (yold[ch] - in);
        yold[ch] = result;
        if (levelType == RMS) return std::sqrt(result);
        return result;
    }
};

// ---------------------------------------------------------------- Compressor
struct Comp {
    float threshold = 1, thresholdInverse = 1, ratioInverse = 1;
    Ballistics env;
    double sampleRate = 44100.0;
    float thresholddB = 0.0f, ratio = 1.0f, attackTime = 1.0f, releaseTime = 100.0f;

    Comp() { update(); }
    void update() {
        threshold = decibelsToGain(thresholddB, -200.0f);
        thresholdInverse = 1.0f / threshold;
        ratioInverse = 1.0f / ratio;
        env.setAttack(attackTime);
        env.setRelease(releaseTime);
    }
    void setThreshold(float v) { thresholddB = v; update(); }
    void setRatio(float v) { ratio = v; update(); }
    void setAttack(float v) { attackTime = v; update(); }
    void setRelease(float v) { releaseTime = v; update(); }
    void prepare(const Spec& s) {
        sampleRate = s.sampleRate;
        env.prepare(s);
        update();
        reset();
    }
    void reset() { env.reset(); }
    float processSample(int ch, float in) {
        float e = env.processSample(ch, in);
        float gain = (e < threshold) ? 1.0f : std::pow(e * thresholdInverse, ratioInverse - 1.0f);
        return gain * in;
    }
    void processBlock(float** chs, int numCh, int n) {
        for (int c = 0; c < numCh; c++)
            for (int i = 0; i < n; i++) chs[c][i] = processSample(c, chs[c][i]);
    }
};

class CompressorPlugin : public Plugin {
public:
    CompressorPlugin(float th, float ratio, float att, float rel) {
        setThreshold(th); setRatio(ratio); setAttack(att); setRelease(rel);
    }
    void setThreshold(float v) { thr = v; c.setThreshold(v); }
    void setRatio(float v) {
        if (v < 1.0f) throw std::runtime_error("Compressor ratio must be a value >= 1.0.");
        ratio = v; c.setRatio(v);
    }
    void setAttack(float v) { att = v; c.setAttack(v); }
    void setRelease(float v) { rel = v; c.setRelease(v); }
    void prepare(const Spec& s) override {
        if (last.sampleRate != s.sampleRate || last.maximumBlockSize < s.maximumBlockSize || s.numChannels != last.numChannels) {
            c.prepare(s);
            last = s;
        }
    }
    int process(float** ch, int numCh, int n) override { c.processBlock(ch, numCh, n); return n; }
    void reset() override { c.reset(); }
    Comp c;
    Spec last;
    float thr = 0, ratio = 1, att = 1, rel = 100;
};

// ---------------------------------------------------------------- NoiseGate
class NoiseGatePlugin : public Plugin {
public:
    NoiseGatePlugin(float th, float ratio, float att, float rel) {
        rms.levelType = Ballistics::RMS;
        rms.setAttack(0.0f);
        rms.setRelease(50.0f);
        setThreshold(th); setRatio(ratio); setAttack(att); setRelease(rel);
    }
    void update() {
        threshold = decibelsToGain(thresholddB, -200.0f);
        thresholdInverse = 1.0f / threshold;
        currentRatio = ratio;
        env.setAttack(attackTime);
        env.setRelease(releaseTime);
    }
    void setThreshold(float v) { thresholddB = v; update(); }
    void setRatio(float v) { ratio = v; update(); }
    void setAttack(float v) { attackTime = v; update(); }
    void setRelease(float v) { releaseTime = v; update(); }
    void prepare(const Spec& s) override {
        if (last.sampleRate != s.sampleRate || last.maximumBlockSize < s.maximumBlockSize || s.numChannels != last.numChannels) {
            rms.prepare(s);
            env.prepare(s);
            update();
            rms.reset(); env.reset();
            last = s;
        }
    }
    float sample(int ch, float x) {
        float e = rms.processSample(ch, x);
        e = env.processSample(ch, e);
        float gain = (e > threshold) ? 1.0f : std::pow(e * thresholdInverse, currentRatio - 1.0f);
        return gain * x;
    }
    int process(float** ch, int numCh, int n) override {
        for (int c = 0; c < numCh; c++)
            for (int i = 0; i < n; i++) ch[c][i] = sample(c, ch[c][i]);
        return n;
    }
    void reset() override { rms.reset(); env.reset(); }

    Ballistics env, rms;
    Spec last;
    float threshold = 1, thresholdInverse = 1, currentRatio = 10;
    float thresholddB = -100, ratio = 10.0f, attackTime = 1.0f, releaseTime = 100.0f;
};

// ---------------------------------------------------------------- Limiter
class LimiterPlugin : public Plugin {
public:
    LimiterPlugin(float th, float rel) { setThreshold(th); setRelease(rel); }
    void update() {
        first.setThreshold(-10.0f);
        first.setRatio(4.0f);
        first.setAttack(2.0f);
        first.setRelease(200.0f);
        second.setThreshold(thresholddB);
        second.setRatio(1000.0f);
        second.setAttack(0.001f);
        second.setRelease(releaseTime);
        float ratioInverse = (float)(1.0 / 4.0);
        float gain = (float)std::pow(10.0, 10.0 * (1.0 - ratioInverse) / 40.0);
        gain *= decibelsToGain(-thresholddB, -100.0f);
        outputGain = gain;
    }
    void setThreshold(float v) { thresholddB = v; update(); }
    void setRelease(float v) { releaseTime = v; update(); }
    void prepare(const Spec& s) override {
        if (last.sampleRate != s.sampleRate || last.maximumBlockSize < s.maximumBlockSize || s.numChannels != last.numChannels) {
            first.prepare(s);
            second.prepare(s);
            update();
            reset();
            last = s;
        }
    }
    int process(float** ch, int numCh, int n) override {
        first.processBlock(ch, numCh, n);
        second.processBlock(ch, numCh, n);
        for (int c = 0; c < numCh; c++)
            for (int i = 0; i < n; i++) {
                float v = ch[c][i] * outputGain;  // SmoothedValue は reset 直後で動かない(一定値)
                ch[c][i] = std::min(1.0f, std::max(-1.0f, v));
            }
        return n;
    }
    void reset() override { first.reset(); second.reset(); }

    Comp first, second;
    Spec last;
    float thresholddB = -10.0f, releaseTime = 100.0f, outputGain = 1.0f;
};

// ---------------------------------------------------------------- IIR(ProcessorDuplicator<IIR::Filter>)
struct Coeffs {
    int order = 1;
    float c[5] = {0, 0, 0, 0, 0};  // order1: b0 b1 a1 / order2: b0 b1 b2 a1 a2
    void assign4(float b0, float b1, float a0, float a1) {
        float a0Inv = a0 != 0.0f ? 1.0f / a0 : 0.0f;
        order = 1;
        c[0] = b0 * a0Inv; c[1] = b1 * a0Inv; c[2] = a1 * a0Inv;
    }
    void assign6(float b0, float b1, float b2, float a0, float a1, float a2) {
        float a0Inv = a0 != 0.0f ? 1.0f / a0 : 0.0f;
        order = 2;
        c[0] = b0 * a0Inv; c[1] = b1 * a0Inv; c[2] = b2 * a0Inv; c[3] = a1 * a0Inv; c[4] = a2 * a0Inv;
    }
};

class IIRPlugin : public Plugin {
public:
    enum Kind { HighPass1, LowShelf, HighShelf, Peak };
    explicit IIRPlugin(Kind k, float cutoff, float gainDb, float q) : kind(k), cutoffHz(cutoff), Q(q) {
        gainFactor = decibelsToGain(gainDb);
    }
    void setCutoff(float f) {
        if (kind != HighPass1 && f <= 0) throw std::runtime_error("Cutoff frequency must be greater than 0Hz.");
        cutoffHz = f;
    }
    void setQ(float q) {
        if (q <= 0) throw std::runtime_error("Q value must be greater than 0.");
        Q = q;
    }
    void setGainDb(float db) { gainFactor = decibelsToGain(db); }

    // juce::jlimit(1e-2f, sr/2 - 1e2f, f)
    static float clampCutoff(float f, float sr) {
        float lo = 1e-2f, hi = (sr / 2) - 1e2f;
        return f < lo ? lo : (hi < f ? hi : f);
    }

    void makeCoeffs(double sr) {
        float srf = static_cast<float>(sr);
        if (kind == HighPass1) {
            float n = std::tan(kPi * cutoffHz / srf);
            co.assign4(1, -1, n + 1, n - 1);
            return;
        }
        float f = clampCutoff(cutoffHz, srf);
        float A = std::max(0.0f, std::sqrt(gainFactor));
        if (kind == LowShelf) {
            float aminus1 = A - 1, aplus1 = A + 1;
            float omega = (2 * kPi * std::max(f, 2.0f)) / srf;
            float coso = std::cos(omega);
            float beta = std::sin(omega) * std::sqrt(A) / Q;
            float aminus1TimesCoso = aminus1 * coso;
            co.assign6(A * (aplus1 - aminus1TimesCoso + beta),
                       A * 2 * (aminus1 - aplus1 * coso),
                       A * (aplus1 - aminus1TimesCoso - beta),
                       aplus1 + aminus1TimesCoso + beta,
                       -2 * (aminus1 + aplus1 * coso),
                       aplus1 + aminus1TimesCoso - beta);
        } else if (kind == HighShelf) {
            float aminus1 = A - 1, aplus1 = A + 1;
            float omega = (2 * kPi * std::max(f, 2.0f)) / srf;
            float coso = std::cos(omega);
            float beta = std::sin(omega) * std::sqrt(A) / Q;
            float aminus1TimesCoso = aminus1 * coso;
            co.assign6(A * (aplus1 + aminus1TimesCoso + beta),
                       A * -2 * (aminus1 + aplus1 * coso),
                       A * (aplus1 + aminus1TimesCoso - beta),
                       aplus1 - aminus1TimesCoso + beta,
                       2 * (aminus1 - aplus1 * coso),
                       aplus1 - aminus1TimesCoso - beta);
        } else {
            float omega = (2 * kPi * std::max(f, 2.0f)) / srf;
            float alpha = std::sin(omega) / (Q * 2);
            float c2 = -2 * std::cos(omega);
            float alphaTimesA = alpha * A;
            float alphaOverA = alpha / A;
            co.assign6(1 + alphaTimesA, c2, 1 - alphaTimesA, 1 + alphaOverA, c2, 1 - alphaOverA);
        }
    }
    void prepare(const Spec& s) override {
        makeCoeffs(s.sampleRate);  // 派生クラスの prepare は毎回係数を作り直す(pedalboard と同じ)
        if (last.sampleRate != s.sampleRate || last.maximumBlockSize < s.maximumBlockSize || s.numChannels != last.numChannels) {
            state.assign(s.numChannels, {0.0f, 0.0f});
            last = s;
        }
    }
    int process(float** ch, int numCh, int n) override {
        for (int c = 0; c < numCh; c++) {
            float* x = ch[c];
            float* st = state[c].data();
            if (co.order == 1) {
                float b0 = co.c[0], b1 = co.c[1], a1 = co.c[2];
                float lv1 = st[0];
                for (int i = 0; i < n; i++) {
                    float input = x[i];
                    float output = input * b0 + lv1;
                    x[i] = output;
                    lv1 = (input * b1) - (output * a1);
                }
                snapToZero(lv1); st[0] = lv1;
            } else {
                float b0 = co.c[0], b1 = co.c[1], b2 = co.c[2], a1 = co.c[3], a2 = co.c[4];
                float lv1 = st[0], lv2 = st[1];
                for (int i = 0; i < n; i++) {
                    float input = x[i];
                    float output = (input * b0) + lv1;
                    x[i] = output;
                    lv1 = (input * b1) - (output * a1) + lv2;
                    lv2 = (input * b2) - (output * a2);
                }
                snapToZero(lv1); st[0] = lv1;
                snapToZero(lv2); st[1] = lv2;
            }
        }
        return n;
    }
    void reset() override { for (auto& s : state) s = {0.0f, 0.0f}; }

    Kind kind;
    float cutoffHz, Q, gainFactor = 1.0f;
    Coeffs co;
    Spec last;
    std::vector<std::array<float, 2>> state;
};

// ---------------------------------------------------------------- pedalboard::process
struct IoBuffer {
    int nch = 0, n = 0;
    std::vector<std::vector<float>>* own = nullptr;  // 所有していればサイズ変更できる
    std::vector<float*> ptr;
    void bind() { ptr.resize(nch); for (int c = 0; c < nch; c++) ptr[c] = (*own)[c].data(); }
    void setSize(int newN) {
        if (newN == n) return;
        if (!own) throw std::runtime_error("internal: cannot resize external buffer");
        for (auto& v : *own) v.resize((size_t)newN, 0.0f);
        n = newN;
        bind();
    }
};

class Chain : public Plugin {
public:
    explicit Chain(std::vector<Plugin*> p) : plugins(std::move(p)) {}
    void prepare(const Spec& spec) override {
        for (auto p : plugins) p->prepare(spec);
        lastSpec = spec;
    }
    int process(float** ch, int numCh, int n) override;
    void reset() override { for (auto p : plugins) p->reset(); }
    int getLatencyHint() override {
        int h = 0;
        for (auto p : plugins) h += p->getLatencyHint();
        return h;
    }
    std::vector<Plugin*> plugins;
    Spec lastSpec;
};

int processBuf(IoBuffer& io, const Spec& spec, const std::vector<Plugin*>& plugins, bool isProbablyLastProcessCall) {
    int totalOutputLatencySamples = 0;
    int expectedOutputLatency = 0;
    for (auto p : plugins) expectedOutputLatency += p->getLatencyHint();

    int intendedOutputBufferSize = io.n;
    if (expectedOutputLatency > 0 && isProbablyLastProcessCall) io.setSize(io.n + expectedOutputLatency);

    int startOfOutputInBuffer = 0;
    int lastSampleInBuffer = 0;

    for (auto plugin : plugins) {
        int pluginSamplesReceived = 0;
        unsigned blockSize = spec.maximumBlockSize;
        for (unsigned blockStart = startOfOutputInBuffer; blockStart < (unsigned)intendedOutputBufferSize; blockStart += blockSize) {
            unsigned blockEnd = std::min(blockStart + spec.maximumBlockSize, (unsigned)intendedOutputBufferSize);
            blockSize = blockEnd - blockStart;

            std::vector<float*> chp(io.nch);
            for (int c = 0; c < io.nch; c++) chp[c] = io.ptr[c] + blockStart;
            int outputSamples = plugin->process(chp.data(), io.nch, (int)blockSize);
            if (outputSamples < 0) throw std::runtime_error("A plugin returned a negative number of output samples");
            pluginSamplesReceived += outputSamples;

            int missingSamples = (int)blockSize - outputSamples;
            if (missingSamples < 0) throw std::runtime_error("A plugin returned more samples than were asked for");

            if (missingSamples > 0 && pluginSamplesReceived > 0) {
                for (int c = 0; c < io.nch; c++) {
                    int samplesToMove = pluginSamplesReceived - outputSamples;
                    float* outputStart = io.ptr[c] + totalOutputLatencySamples;
                    float* expectedOutputEnd = io.ptr[c] + blockEnd - outputSamples;
                    float* expectedOutputStart = expectedOutputEnd - samplesToMove;
                    std::memmove(expectedOutputStart, outputStart, sizeof(float) * (size_t)samplesToMove);
                }
            }
            lastSampleInBuffer = std::max(lastSampleInBuffer, (int)(blockStart + outputSamples));
            startOfOutputInBuffer += missingSamples;
            totalOutputLatencySamples += missingSamples;

            if (missingSamples && isProbablyLastProcessCall) {
                intendedOutputBufferSize += missingSamples;
                if (intendedOutputBufferSize > io.n) io.setSize(intendedOutputBufferSize);
            }
        }
    }
    io.setSize(intendedOutputBufferSize);  // 縮める(中身は残る)
    return intendedOutputBufferSize - totalOutputLatencySamples;
}

int Chain::process(float** ch, int numCh, int n) {
    IoBuffer io;
    io.nch = numCh;
    io.n = n;
    io.ptr.assign(ch, ch + numCh);
    return processBuf(io, lastSpec, plugins, false);
}

}  // namespace

Audio runBoard(const Audio& in, double sampleRate, const std::vector<Plugin*>& plugins, unsigned bufferSize) {
    Audio buf = in;
    int nch = (int)buf.size();
    int n = nch ? (int)buf[0].size() : 0;
    if (nch == 0) return buf;
    if (plugins.empty()) return buf;

    Chain chain(plugins);
    std::vector<Plugin*> top{&chain};

    IoBuffer io;
    io.nch = nch;
    io.n = n;
    io.own = &buf;
    io.bind();

    bufferSize = std::min(bufferSize, (unsigned)n);
    chain.reset();  // reset=True
    Spec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = bufferSize;
    spec.numChannels = (uint32_t)nch;
    chain.prepare(spec);

    int samplesReturned = processBuf(io, spec, top, true);
    int totalLatency = io.n - samplesReturned;
    Audio out(nch);
    for (int c = 0; c < nch; c++) out[c].assign(buf[c].begin() + totalLatency, buf[c].end());
    return out;
}

std::unique_ptr<Plugin> makeGain(float gainDb) { return std::make_unique<GainPlugin>(gainDb); }

const std::vector<std::string>& builtinNames() {
    static const std::vector<std::string> v = {"HighpassFilter", "NoiseGate", "Compressor", "LowShelfFilter", "HighShelfFilter", "PeakFilter", "Limiter"};
    return v;
}

namespace {
float asFloat(const std::string& pname, const vj::Value& v) {
    if (v.type == vj::Value::Bool) return v.b ? 1.0f : 0.0f;
    if (!v.isNum()) throw std::runtime_error("パラメータ " + pname + " は数値で指定してください");
    return static_cast<float>(v.num());
}
}  // namespace

std::unique_ptr<Plugin> makeBuiltin(const std::string& name, const vj::Value& params) {
    const float defQ = static_cast<float>(1.41421356237309504880f / 2.0);
    std::unique_ptr<Plugin> pl;
    auto bad = [&](const std::string& k) -> std::runtime_error {
        return std::runtime_error("内蔵エフェクト " + name + " に不明なパラメータ: " + k);
    };
    auto each = [&](auto&& fn) {
        if (params.isObj())
            for (auto& e : *params.o) fn(e.first, asFloat(e.first, e.second));
    };
    if (name == "HighpassFilter") {
        auto p = std::make_unique<IIRPlugin>(IIRPlugin::HighPass1, 50.0f, 0.0f, defQ);
        each([&](const std::string& k, float v) {
            if (k == "cutoff_frequency_hz") p->setCutoff(v); else throw bad(k);
        });
        pl = std::move(p);
    } else if (name == "NoiseGate") {
        float th = -100.0f, ra = 10.0f, at = 1.0f, re = 100.0f;
        auto p = std::make_unique<NoiseGatePlugin>(th, ra, at, re);
        each([&](const std::string& k, float v) {
            if (k == "threshold_db") p->setThreshold(v);
            else if (k == "ratio") p->setRatio(v);
            else if (k == "attack_ms") p->setAttack(v);
            else if (k == "release_ms") p->setRelease(v);
            else throw bad(k);
        });
        pl = std::move(p);
    } else if (name == "Compressor") {
        auto p = std::make_unique<CompressorPlugin>(0.0f, 1.0f, 1.0f, 100.0f);
        each([&](const std::string& k, float v) {
            if (k == "threshold_db") p->setThreshold(v);
            else if (k == "ratio") p->setRatio(v);
            else if (k == "attack_ms") p->setAttack(v);
            else if (k == "release_ms") p->setRelease(v);
            else throw bad(k);
        });
        pl = std::move(p);
    } else if (name == "Limiter") {
        auto p = std::make_unique<LimiterPlugin>(-10.0f, 100.0f);
        each([&](const std::string& k, float v) {
            if (k == "threshold_db") p->setThreshold(v);
            else if (k == "release_ms") p->setRelease(v);
            else throw bad(k);
        });
        pl = std::move(p);
    } else if (name == "LowShelfFilter" || name == "HighShelfFilter" || name == "PeakFilter") {
        auto kind = name == "LowShelfFilter" ? IIRPlugin::LowShelf : name == "HighShelfFilter" ? IIRPlugin::HighShelf : IIRPlugin::Peak;
        auto p = std::make_unique<IIRPlugin>(kind, 440.0f, 0.0f, defQ);
        each([&](const std::string& k, float v) {
            if (k == "cutoff_frequency_hz") p->setCutoff(v);
            else if (k == "gain_db") p->setGainDb(v);
            else if (k == "q") p->setQ(v);
            else throw bad(k);
        });
        pl = std::move(p);
    } else {
        throw std::runtime_error("不明な audio ステップ: " + name);
    }
    return pl;
}

}  // namespace vaudio
