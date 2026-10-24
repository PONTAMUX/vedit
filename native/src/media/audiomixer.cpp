#include "audiomixer.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace vn {

static int64_t pyRound(double v) { return (int64_t)std::nearbyint(v); }   // Python の round(銀行丸め)

// ---------------------------------------------------------------- デコード(48k stereo float に変換)

struct Pcm {
    std::vector<float> l, r;
    int64_t n() const { return (int64_t)l.size(); }
};

class AudioFile {
public:
    ~AudioFile() { close(); }
    bool open(const QString& path) {
        close();
        const QByteArray u = path.toUtf8();
        if (avformat_open_input(&fmt_, u.constData(), nullptr, nullptr) < 0) { fmt_ = nullptr; return false; }
        if (avformat_find_stream_info(fmt_, nullptr) < 0) { close(); return false; }
        si_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (si_ < 0) { close(); return false; }
        AVStream* st = fmt_->streams[si_];
        const AVCodec* c = avcodec_find_decoder(st->codecpar->codec_id);
        if (!c) { close(); return false; }
        dec_ = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(dec_, st->codecpar);
        dec_->thread_count = 1;   // 編集のたびに作り直すので、裏スレッドを作らせない(作っては消すのが DLL の鍵を取り合って固まる)
        if (avcodec_open2(dec_, c, nullptr) < 0) { close(); return false; }
        tb_ = st->time_base;
        startOff_ = fmt_->start_time != AV_NOPTS_VALUE ? fmt_->start_time / (double)AV_TIME_BASE : 0.0;
        channels_ = st->codecpar->ch_layout.nb_channels;
        bytesPerSample_ = av_get_bytes_per_sample((AVSampleFormat)st->codecpar->format);
        pkt_ = av_packet_alloc();
        frm_ = av_frame_alloc();
        double dur = st->duration != AV_NOPTS_VALUE ? st->duration * av_q2d(tb_) : (fmt_->duration > 0 ? fmt_->duration / (double)AV_TIME_BASE : -1);
        lengthSamples_ = dur > 0 ? pyRound(dur * kSampleRate) : -1;
        pos_ = 0;
        posKnown_ = true;
        return true;
    }
    void close() {
        if (swr_) swr_free(&swr_);
        if (frm_) av_frame_free(&frm_);
        if (pkt_) av_packet_free(&pkt_);
        if (dec_) avcodec_free_context(&dec_);
        if (fmt_) avformat_close_input(&fmt_);
        si_ = -1;
        fl_.clear(); fr_.clear(); head_ = 0;
    }
    int channels() const { return channels_; }
    int bytesPerSample() const { return bytesPerSample_; }
    int64_t lengthSamples() const { return lengthSamples_; }   // 不明は -1

    // 先頭から最後まで(pts は見ず、出てきた順)
    bool readAll(Pcm* out) {
        out->l.clear(); out->r.clear();
        while (fill(1)) {
            const size_t n = fl_.size() - head_;
            out->l.insert(out->l.end(), fl_.begin() + head_, fl_.end());
            out->r.insert(out->r.end(), fr_.begin() + head_, fr_.end());
            head_ += n;
            compact();
        }
        return true;
    }

    // サンプル位置 start から n 個(ファイルの終わりを過ぎたぶんは 0)。連続して呼べば続きから読む
    void readAt(int64_t start, int64_t n, float* l, float* r) {
        if (!(posKnown_ && start >= pos_ && start - pos_ < kSampleRate * 2)) {
            if (!(posKnown_ && start == pos_)) seekTo(start);
        }
        while (posKnown_ ? pos_ < start : false) {
            if (avail() == 0 && !fill(1)) break;
            if (!posKnown_) break;
            const int64_t d = std::min<int64_t>(avail(), start - pos_);
            head_ += d; pos_ += d;
            compact();
        }
        int64_t done = 0;
        while (done < n) {
            if (avail() == 0 && !fill(1)) break;
            const int64_t k = std::min<int64_t>(avail(), n - done);
            std::memcpy(l + done, fl_.data() + head_, k * 4);
            std::memcpy(r + done, fr_.data() + head_, k * 4);
            head_ += k; pos_ += k; done += k;
            compact();
        }
        if (done < n) {
            std::memset(l + done, 0, (n - done) * 4);
            std::memset(r + done, 0, (n - done) * 4);
            pos_ += n - done;   // 終わりを過ぎた位置として進める(以後も 0)
        }
    }

    // ffmpeg の atrim=start=in と同じ: 元のサンプルレートのまま in の位置まで読み捨て、そこから変換を始める(変換の位相を合わせる)
    void readTrim(double inSec, int64_t n, float* l, float* r) {
        seekRaw(std::max(0.0, inSec - 0.5));
        trimMode_ = true; trimSec_ = inSec; nativeKnown_ = false;
        int64_t done = 0;
        while (done < n) {
            if (avail() == 0 && !fill(1)) break;
            const int64_t k = std::min<int64_t>(avail(), n - done);
            std::memcpy(l + done, fl_.data() + head_, k * 4);
            std::memcpy(r + done, fr_.data() + head_, k * 4);
            head_ += k; done += k;
            compact();
        }
        trimMode_ = false;
        if (done < n) {
            std::memset(l + done, 0, (n - done) * 4);
            std::memset(r + done, 0, (n - done) * 4);
        }
        posKnown_ = false; wantPosFromPts_ = false; pos_ = 0;
        fl_.clear(); fr_.clear(); head_ = 0;
        if (swr_) swr_free(&swr_);
    }

private:
    int64_t avail() const { return (int64_t)(fl_.size() - head_); }
    void compact() {
        if (head_ > (1 << 20)) {
            fl_.erase(fl_.begin(), fl_.begin() + head_);
            fr_.erase(fr_.begin(), fr_.begin() + head_);
            head_ = 0;
        }
    }
    void seekRaw(double sec) {
        if (swr_) swr_free(&swr_);
        fl_.clear(); fr_.clear(); head_ = 0;
        eof_ = false; flushed_ = false;
        const int64_t ts = (int64_t)((std::max(0.0, sec) + startOff_) / av_q2d(tb_));
        av_seek_frame(fmt_, si_, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dec_);
        posKnown_ = false;
        wantPosFromPts_ = true;
    }
    void seekTo(int64_t sample) {
        double margin = 0.5;
        for (int tries = 0; tries < 2; ++tries) {
            seekRaw(std::max(0.0, (double)sample / kSampleRate - margin));
            fill(1);   // 最初の1コマ → pos_ が決まる
            if (!posKnown_) { posKnown_ = true; pos_ = std::max<int64_t>(0, sample - (int64_t)(margin * kSampleRate)); }
            if (pos_ <= sample || sample == 0) break;
            margin = 1e9;   // 目標より後ろに着いた → 先頭から
        }
    }
    // fifo に最低 want サンプルを足す。足せなければ false
    bool fill(int64_t want) {
        while (avail() < want) {
            if (flushed_) return false;
            if (!decodeStep()) return avail() >= want;
        }
        return true;
    }
    void convert(AVFrame* f, int skip = 0) {
        if (!swr_) {
            AVChannelLayout in{}, out{};
            if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) av_channel_layout_default(&in, f->ch_layout.nb_channels);
            else av_channel_layout_copy(&in, &f->ch_layout);
            av_channel_layout_default(&out, 2);
            swr_alloc_set_opts2(&swr_, &out, AV_SAMPLE_FMT_FLTP, kSampleRate, &in, (AVSampleFormat)f->format, f->sample_rate, 0, nullptr);
            swr_init(swr_);
            av_channel_layout_uninit(&in);
            av_channel_layout_uninit(&out);
        }
        const int nb = f ? f->nb_samples - skip : 0;
        const uint8_t* ptrs[AV_NUM_DATA_POINTERS] = {};
        if (f) {
            const int bps = av_get_bytes_per_sample((AVSampleFormat)f->format);
            const bool planar = av_sample_fmt_is_planar((AVSampleFormat)f->format);
            const int nch = f->ch_layout.nb_channels;
            for (int c = 0; c < (planar ? std::min(nch, (int)AV_NUM_DATA_POINTERS) : 1); ++c)
                ptrs[c] = f->extended_data[c] + (int64_t)skip * bps * (planar ? 1 : nch);
        }
        const int cap = swr_get_out_samples(swr_, nb) + 64;
        append(f ? ptrs : nullptr, nb, cap);
    }
    void append(const uint8_t** in, int nin, int cap) {
        std::vector<float> a(cap), b(cap);
        uint8_t* o[2] = {(uint8_t*)a.data(), (uint8_t*)b.data()};
        const int got = swr_convert(swr_, o, cap, in, nin);
        if (got > 0) {
            fl_.insert(fl_.end(), a.begin(), a.begin() + got);
            fr_.insert(fr_.end(), b.begin(), b.begin() + got);
        }
    }
    bool decodeStep() {
        for (;;) {
            int rc = avcodec_receive_frame(dec_, frm_);
            if (rc == 0) {
                if (wantPosFromPts_) {
                    const int64_t pts = frm_->best_effort_timestamp != AV_NOPTS_VALUE ? frm_->best_effort_timestamp : frm_->pts;
                    if (pts != AV_NOPTS_VALUE) {
                        pos_ = std::max<int64_t>(0, pyRound((pts * av_q2d(tb_) - startOff_) * kSampleRate));
                        posKnown_ = true;
                    }
                    wantPosFromPts_ = false;
                }
                const size_t before = fl_.size();
                int skip = 0;
                if (trimMode_) {
                    if (!nativeKnown_) {
                        const int64_t pts = frm_->best_effort_timestamp != AV_NOPTS_VALUE ? frm_->best_effort_timestamp : frm_->pts;
                        nativeIdx_ = pts != AV_NOPTS_VALUE ? (int64_t)std::llround((pts * av_q2d(tb_) - startOff_) * frm_->sample_rate) : 0;
                        nativeKnown_ = true;
                    }
                    const int64_t target = (int64_t)std::llround(trimSec_ * frm_->sample_rate);
                    const int64_t sk = target - nativeIdx_;
                    nativeIdx_ += frm_->nb_samples;
                    if (sk >= frm_->nb_samples) { av_frame_unref(frm_); continue; }
                    if (sk > 0) skip = (int)sk;
                }
                convert(frm_, skip);
                av_frame_unref(frm_);
                if (fl_.size() > before) return true;
                continue;
            }
            if (rc == AVERROR_EOF) {
                if (swr_) {
                    const size_t before = fl_.size();
                    append(nullptr, 0, 4096 + swr_get_out_samples(swr_, 0));
                    flushed_ = true;
                    return fl_.size() > before;
                }
                flushed_ = true;
                return false;
            }
            if (rc != AVERROR(EAGAIN)) { flushed_ = true; return false; }
            if (eof_) { flushed_ = true; return false; }
            int rr = av_read_frame(fmt_, pkt_);
            if (rr < 0) {
                eof_ = true;
                avcodec_send_packet(dec_, nullptr);
                continue;
            }
            if (pkt_->stream_index == si_) avcodec_send_packet(dec_, pkt_);
            av_packet_unref(pkt_);
        }
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* dec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frm_ = nullptr;
    AVRational tb_{1, 1};
    int si_ = -1, channels_ = 2, bytesPerSample_ = 2;
    double startOff_ = 0;
    int64_t lengthSamples_ = -1;
    std::vector<float> fl_, fr_;
    size_t head_ = 0;
    int64_t pos_ = 0;           // fifo の先頭のサンプル位置
    bool posKnown_ = true, wantPosFromPts_ = false, eof_ = false, flushed_ = false;
    bool trimMode_ = false, nativeKnown_ = false;
    double trimSec_ = 0;
    int64_t nativeIdx_ = 0;
};

// ---------------------------------------------------------------- libavfilter のグラフ(入力は 48k stereo fltp)

class AGraph {
public:
    ~AGraph() { reset(); }
    void reset() {
        if (g_) avfilter_graph_free(&g_);
        src_.clear();
        sink_ = nullptr;
    }
    // desc は [名前] で入力を受け、最後を [out] にする
    bool build(const std::vector<std::string>& names, const std::string& desc, QString* err) {
        reset();
        g_ = avfilter_graph_alloc();
        if (g_) g_->nb_threads = 1;   // 既定はコア数ぶんスレッドを作る。編集のたびに作っては消すと固まるので 1 本
        AVFilterInOut *outs = nullptr, *ins = nullptr;
        for (const auto& nm : names) {
            AVFilterContext* c = nullptr;
            if (avfilter_graph_create_filter(&c, avfilter_get_by_name("abuffer"), nm.c_str(),
                                             "sample_rate=48000:sample_fmt=fltp:channel_layout=stereo:time_base=1/48000", nullptr, g_) < 0) {
                if (err) *err = "abuffer";
                return false;
            }
            src_.push_back(c);
            AVFilterInOut* o = avfilter_inout_alloc();
            o->name = av_strdup(nm.c_str());
            o->filter_ctx = c;
            o->pad_idx = 0;
            o->next = outs;
            outs = o;
        }
        if (avfilter_graph_create_filter(&sink_, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, g_) < 0) {
            if (err) *err = "abuffersink";
            return false;
        }
        AVFilterInOut* i = avfilter_inout_alloc();
        i->name = av_strdup("out");
        i->filter_ctx = sink_;
        i->pad_idx = 0;
        ins = i;
        int rc = avfilter_graph_parse_ptr(g_, desc.c_str(), &ins, &outs, nullptr);
        avfilter_inout_free(&ins);
        avfilter_inout_free(&outs);
        if (rc < 0) { if (err) *err = QString("filter parse 失敗: ") + desc.c_str(); return false; }
        rc = avfilter_graph_config(g_, nullptr);
        if (rc < 0) { if (err) *err = QString("filter config 失敗: ") + desc.c_str(); return false; }
        return true;
    }
    void push(int src, const float* l, const float* r, int n, int64_t pts) {
        AVFrame* f = av_frame_alloc();
        f->format = AV_SAMPLE_FMT_FLTP;
        f->nb_samples = n;
        f->sample_rate = kSampleRate;
        av_channel_layout_default(&f->ch_layout, 2);
        f->pts = pts;
        av_frame_get_buffer(f, 0);
        std::memcpy(f->extended_data[0], l, n * 4);
        std::memcpy(f->extended_data[1], r, n * 4);
        av_buffersrc_add_frame_flags(src_[src], f, 0);
        av_frame_free(&f);
    }
    void eof(int src) { av_buffersrc_add_frame_flags(src_[src], nullptr, 0); }
    // 出てきたぶんを l, r の後ろに足す
    void pull(std::vector<float>& l, std::vector<float>& r) {
        AVFrame* f = av_frame_alloc();
        while (av_buffersink_get_frame(sink_, f) >= 0) {
            l.insert(l.end(), (float*)f->extended_data[0], (float*)f->extended_data[0] + f->nb_samples);
            r.insert(r.end(), (float*)f->extended_data[1], (float*)f->extended_data[1] + f->nb_samples);
            av_frame_unref(f);
        }
        av_frame_free(&f);
    }

private:
    AVFilterGraph* g_ = nullptr;
    std::vector<AVFilterContext*> src_;
    AVFilterContext* sink_ = nullptr;
};

// ---------------------------------------------------------------- ミキサー本体

struct Ev {                 // 足し込む短い音(sfx・差し込みの音・aslip)
    std::shared_ptr<Pcm> pcm;
    int64_t src0, out0, len;
    float gain;
};

struct APiece {             // A1: _cut_wav の1区間
    int64_t o, src0, len;
    int fl;
    bool gap, fadeIn, fadeOut;
};

static std::string fmtG(double v) {
    char b[64];
    snprintf(b, sizeof b, "%.10g", v);
    return b;
}

struct AudioMixer::Impl {
    int64_t total = 0;
    int FS = 4096;                      // A1 を流すコマの大きさ(元の ffmpeg が wav から出すコマと同じ。enable がコマ単位で効くため)
    std::unique_ptr<AudioFile> a1;
    QString procPath, rawPath;          // 処理後(audio.wav)と生(元素材の音)。UI の「処理後の音で再生」で切り替える
    bool useProc = true;
    std::vector<APiece> pieces;
    std::shared_ptr<Pcm> bgm;
    std::vector<Ev> evs;
    std::vector<int64_t> evMaxEnd;      // evs を out0 順に並べたときの、そこまでの終わりの最大
    std::string desc;
    std::vector<std::string> names;
    bool hasBgm = false, hasX = false;
    bool passthrough = false;           // グラフ無し(A1 のみ・amute も無し)の場合も同じ経路で流すので常に false
    QString infoText;

    AGraph graph;
    bool graphOk = false;
    int64_t feedPos = 0, fifoStart = 0, pos = 0;
    bool eofSent = false;
    std::vector<float> fl, fr;
    std::vector<float> ta, tb, tx, ts;  // 一時
    std::vector<float> zl, zr;

    // ---- 要素の生成
    void genA1(int64_t s0, int n, float* L, float* R) {
        std::memset(L, 0, n * 4);
        std::memset(R, 0, n * 4);
        if (pieces.empty()) return;
        // o が s0 以下の最後の区間の1つ前から
        size_t lo = 0, hi = pieces.size();
        while (lo < hi) { size_t m = (lo + hi) / 2; if (pieces[m].o <= s0) lo = m + 1; else hi = m; }
        size_t i = lo > 1 ? lo - 2 : 0;
        const int64_t e = s0 + n;
        for (; i < pieces.size(); ++i) {
            const APiece& p = pieces[i];
            if (p.o >= e) break;
            if (p.gap || p.o + p.len <= s0) continue;
            const int64_t a = std::max(p.o, s0), b = std::min({p.o + p.len, e, total});
            if (b <= a) continue;
            a1->readAt(p.src0 + (a - p.o), b - a, L + (a - s0), R + (a - s0));
            if (p.fl > 0 && (p.fadeIn || p.fadeOut)) {
                const double step = 1.0 / p.fl;
                for (int64_t t = a; t < b; ++t) {
                    const int64_t k = t - p.o;
                    float g = 1.0f;
                    bool ap = false;
                    if (p.fadeIn && k < p.fl) { g = (float)(k * step); ap = true; }
                    else if (p.fadeOut && k >= p.len - p.fl) { g = (float)((p.fl - 1 - (k - (p.len - p.fl))) * step); ap = true; }
                    if (ap) { L[t - s0] *= g; R[t - s0] *= g; }
                }
            }
        }
    }
    void genBgm(int64_t s0, int n, float* L, float* R) {
        const int64_t N = bgm->n();
        for (int j = 0; j < n; ++j) {
            const int64_t k = (s0 + j) % N;
            L[j] = bgm->l[k];
            R[j] = bgm->r[k];
        }
    }
    void genX(int64_t s0, int n, float* L, float* R) {
        std::memset(L, 0, n * 4);
        std::memset(R, 0, n * 4);
        if (evs.empty()) return;
        const int64_t e = s0 + n;
        size_t lo = 0, hi = evs.size();   // evMaxEnd は単調 → 終わりが s0 を過ぎる最初
        while (lo < hi) { size_t m = (lo + hi) / 2; if (evMaxEnd[m] > s0) hi = m; else lo = m + 1; }
        for (size_t i = lo; i < evs.size(); ++i) {
            const Ev& v = evs[i];
            if (v.out0 >= e) break;
            const int64_t a = std::max(v.out0, s0), b = std::min(v.out0 + v.len, e);
            for (int64_t t = a; t < b; ++t) {
                const int64_t k = v.src0 + (t - v.out0);
                if (k < 0 || k >= v.pcm->n()) continue;
                L[t - s0] += v.pcm->l[k] * v.gain;
                R[t - s0] += v.pcm->r[k] * v.gain;
            }
        }
    }

    // ---- グラフに流す
    bool startAt(int64_t target) {
        const int64_t PRE = 3 * (int64_t)kSampleRate;
        int64_t from = std::max<int64_t>(0, target - PRE);
        from = from / FS * FS;
        QString err;
        graphOk = graph.build(names, desc, &err);
        if (!graphOk) { qWarning("audio graph: %s", qPrintable(err)); return false; }
        feedPos = from;
        fifoStart = from;
        fl.clear(); fr.clear();
        eofSent = false;
        pos = target;
        return true;
    }
    void feedOne() {
        const int n = (int)std::min<int64_t>(FS, total - feedPos);
        if (n <= 0) {
            if (!eofSent) { for (size_t i = 0; i < names.size(); ++i) graph.eof((int)i); eofSent = true; }
            return;
        }
        ta.resize(n); tb.resize(n);
        genA1(feedPos, n, ta.data(), tb.data());
        int idx = 0;
        graph.push(idx++, ta.data(), tb.data(), n, feedPos);
        if (hasBgm) {
            genBgm(feedPos, n, ta.data(), tb.data());
            graph.push(idx++, ta.data(), tb.data(), n, feedPos);
        }
        if (hasX) {
            genX(feedPos, n, ta.data(), tb.data());
            graph.push(idx++, ta.data(), tb.data(), n, feedPos);
        }
        feedPos += n;
        if (feedPos >= total) { for (size_t i = 0; i < names.size(); ++i) graph.eof((int)i); eofSent = true; }
    }
    void ensure(int64_t upTo) {
        if (!graphOk) return;
        while (fifoStart + (int64_t)fl.size() < upTo && !(eofSent && drained)) {
            const size_t before = fl.size();
            if (!eofSent) feedOne();
            graph.pull(fl, fr);
            if (eofSent && fl.size() == before) drained = true;
        }
        // 古いぶんを捨てる
        const int64_t drop = pos - fifoStart;
        if (drop > (1 << 18)) {
            fl.erase(fl.begin(), fl.begin() + drop);
            fr.erase(fr.begin(), fr.begin() + drop);
            fifoStart += drop;
        }
    }
    bool drained = false;
};

AudioMixer::AudioMixer() : d(new Impl) {}
AudioMixer::~AudioMixer() = default;

int64_t AudioMixer::totalSamples() const { return d->total; }
int64_t AudioMixer::position() const { return d->pos; }
QString AudioMixer::info() const { return d->infoText; }

void AudioMixer::seek(int64_t sample) {
    sample = std::min(std::max<int64_t>(0, sample), d->total);
    if (d->graphOk && sample >= d->fifoStart && sample == d->pos) return;
    // 今の fifo の中なら作り直さない(前へ少し戻る程度)
    if (d->graphOk && sample >= d->fifoStart && sample <= d->fifoStart + (int64_t)d->fl.size() + 4800) {
        d->pos = sample;
        return;
    }
    d->drained = false;
    d->startAt(sample);
}

void AudioMixer::read(float* l, float* r, int n) {
    if (!d->graphOk) { std::memset(l, 0, n * 4); std::memset(r, 0, n * 4); return; }
    d->ensure(d->pos + n);
    for (int i = 0; i < n; ++i) {
        const int64_t k = d->pos + i - d->fifoStart;
        if (k >= 0 && k < (int64_t)d->fl.size() && d->pos + i < d->total) { l[i] = d->fl[k]; r[i] = d->fr[k]; }
        else { l[i] = 0; r[i] = 0; }
    }
    d->pos += n;
}

static bool loadPcm(const QString& path, std::shared_ptr<Pcm>* out) {
    AudioFile f;
    if (!f.open(path)) return false;
    auto p = std::make_shared<Pcm>();
    f.readAll(p.get());
    *out = p;
    return true;
}

// A1 の音源を処理後(audio.wav)/ 生に切り替える。呼んだあとは seek で流し直すこと(グラフはそこで作り直される)
bool AudioMixer::setProcessed(bool on) {
    Impl& m = *d;
    if (m.useProc == on) return true;
    const QString path = on ? m.procPath : m.rawPath;
    if (path.isEmpty()) return false;
    if (path != (m.useProc ? m.procPath : m.rawPath)) {
        auto f = std::make_unique<AudioFile>();
        if (!f->open(path)) return false;
        m.a1 = std::move(f);
    }
    m.useProc = on;
    return true;
}

bool AudioMixer::init(const Project& p, const Timeline& tl, QString* err) {
    Impl& m = *d;
    QElapsedTimer tm;
    tm.start();
    const TimeMap& tmap = tl.timemap;
    const double totalSec = tl.total;
    m.total = pyRound(totalSec * kSampleRate);

    // ---- A1(audio.wav があればそれ、無ければ元素材の音)
    QString wavPath = QDir(p.cacheDir()).filePath("audio.wav");
    m.rawPath = p.resolve(p.source);
    m.procPath = QFileInfo::exists(wavPath) ? wavPath : m.rawPath;
    wavPath = m.useProc ? m.procPath : m.rawPath;
    m.a1 = std::make_unique<AudioFile>();
    if (!m.a1->open(wavPath)) { if (err) *err = "音声を開けません: " + wavPath; return false; }
    m.FS = 4096;   // ffmpeg が cut wav(PCM_24)から出すコマの大きさ。volume の enable はコマ単位で効くので合わせる(実測)
    {   // _cut_wav の区間表
        const auto& pcs = tl.pieces;
        const int64_t srcLen = m.a1->lengthSamples();
        double t = 0;
        const int n = (int)pcs.size();
        const double fadeMs = p.audioFadeMs;
        for (int i = 0; i < n; ++i) {
            const auto& pc = pcs[i];
            APiece ap{};
            ap.o = pyRound(t * kSampleRate);
            if (pc.gap) {
                ap.gap = true;
                t += pc.o1 - pc.o0;
                m.pieces.push_back(ap);
                continue;
            }
            ap.src0 = pyRound(pc.a * kSampleRate);
            int64_t L = pyRound(pc.b * kSampleRate) - ap.src0;
            if (srcLen >= 0) L = std::max<int64_t>(0, std::min<int64_t>(L, srcLen - ap.src0));
            ap.len = L;
            ap.fl = (int)std::min<int64_t>((int64_t)(fadeMs / 1000 * kSampleRate), L / 3);
            ap.fadeIn = n > 1 && i > 0;
            ap.fadeOut = n > 1 && i < n - 1;
            t += pc.b - pc.a;
            m.pieces.push_back(ap);
        }
    }

    // ---- 無音にする範囲・aslip
    TimeMap::Ranges silent;
    struct Slip { double a, b, o; };
    std::vector<Slip> slips;
    for (auto [a, b] : p.amute) {
        double oa, ob;
        if (tmap.outRange(a, b, &oa, &ob)) silent.push_back({oa, ob});
    }
    for (const auto& s : p.aslip) {
        const double o = tmap.out(s.a);
        silent.push_back({o, o + s.b - s.a});
        silent.push_back({o + s.d, o + s.d + s.b - s.a});
        slips.push_back({s.a, s.b, std::max(0.0, o + s.d)});
    }
    if (p.mute.contains("a1")) { silent = {{0.0, totalSec}}; slips.clear(); }

    // ---- aslip
    std::map<QString, std::shared_ptr<Pcm>> cache;   // 効果音ファイルの読み込み済み
    int nSlip = 0, nOv = 0, nSfx = 0;
    for (const auto& s : slips) {
        const double a4 = std::nearbyint(s.a * 1e4) / 1e4, b4 = std::nearbyint(s.b * 1e4) / 1e4;
        const int64_t s0 = pyRound(a4 * kSampleRate), s1 = pyRound(b4 * kSampleRate);
        if (s1 <= s0) continue;
        auto pcm = std::make_shared<Pcm>();
        pcm->l.resize(s1 - s0); pcm->r.resize(s1 - s0);
        AudioFile f;
        if (!f.open(wavPath)) continue;
        f.readAt(s0, s1 - s0, pcm->l.data(), pcm->r.data());
        const double fd = std::min(0.008, (s.b - s.a) / 3);
        char desc[256];
        snprintf(desc, sizeof desc, "[a]afade=t=in:d=%.4f,afade=t=out:st=%.4f:d=%.4f,aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo[out]", fd,
                 s.b - s.a - fd, fd);
        AGraph g;
        QString e2;
        if (!g.build({"a"}, desc, &e2)) { qWarning("slip: %s", qPrintable(e2)); continue; }
        auto outp = std::make_shared<Pcm>();
        for (int64_t i = 0; i < pcm->n(); i += 1024) {
            const int k = (int)std::min<int64_t>(1024, pcm->n() - i);
            g.push(0, pcm->l.data() + i, pcm->r.data() + i, k, i);
        }
        g.eof(0);
        g.pull(outp->l, outp->r);
        const int64_t ms = pyRound(s.o * 1000);
        m.evs.push_back({outp, 0, ms * kSampleRate / 1000, outp->n(), 1.0f});
        ++nSlip;
    }

    // ---- 差し込みの音
    for (const auto& L : tl.layers) {
        const OverlaySpec& o = p.overlays[L.ov];
        if (!o.audio || L.isImage) continue;
        AudioFile f;
        if (!f.open(L.path)) continue;
        const int64_t s0 = pyRound(o.in * kSampleRate);
        const int64_t len = pyRound((L.b - L.a) * kSampleRate);
        auto pcm = std::make_shared<Pcm>();
        pcm->l.resize(len); pcm->r.resize(len);
        (void)s0;
        f.readTrim(o.in, len, pcm->l.data(), pcm->r.data());
        const int64_t ms = (int64_t)(L.a * 1000);
        m.evs.push_back({pcm, 0, ms * kSampleRate / 1000, len, (float)std::pow(10.0, o.volumeDb / 20.0)});
        ++nOv;
    }

    // ---- sfx
    if (true) {
        struct It { double a, b; const SfxSpec* s; };
        std::vector<It> items;
        for (const auto& s : p.sfx) {
            const QString path = p.resolve(s.file);
            if (!cache.count(path)) {
                std::shared_ptr<Pcm> pcm;
                if (!loadPcm(path, &pcm)) { qWarning("sfx を開けません: %s", qPrintable(path)); cache[path] = nullptr; continue; }
                cache[path] = pcm;
            }
            if (!cache[path]) continue;
            const double dur = (double)cache[path]->n() / kSampleRate;
            const double b = s.hasEnd ? s.end : s.start + std::max(0.0, dur - s.in);
            double ra, rb;
            if (s.hasGapAt) {
                double g0, gl;
                if (!tmap.gapAt(s.gapAt, &g0, &gl)) continue;
                ra = g0 + std::max(0.0, s.start);
                rb = g0 + std::min(b, gl);
            } else if (p.timebase == "source") {
                if (!tmap.outRange(s.start, b, &ra, &rb)) continue;
            } else {
                ra = s.start; rb = std::min(b, totalSec);
            }
            if (!(rb > ra)) continue;
            items.push_back({ra, rb, &s});
        }
        // トランジションの効果音(fx.py extra_sfx。出力上の時刻)
        std::vector<SfxSpec> extra;
        extra.reserve(tl.trans.size());
        if (!p.mute.contains("se")) {
            for (const auto& t : tl.trans) {
                if (t.se.isEmpty() || t.se["file"].toString().isEmpty()) continue;
                const double dur = t.P["dur"].toDouble();
                const double a = std::max(0.0, t.o - dur + t.se["offset"].toDouble(0));
                const double b = std::min(totalSec, a + t.se["len"].toDouble(dur));
                if (!(b > a)) continue;
                SfxSpec s;
                s.file = t.se["file"].toString();
                s.start = a; s.end = b; s.hasEnd = true;
                s.in = parseTimeOr(t.se["in"], 0);
                s.gainDb = t.se["gain_db"].toDouble(0);
                const QString path = p.resolve(s.file);
                if (!cache.count(path)) {
                    std::shared_ptr<Pcm> pcm;
                    if (!loadPcm(path, &pcm)) { qWarning("sfx を開けません: %s", qPrintable(path)); cache[path] = nullptr; continue; }
                    cache[path] = pcm;
                }
                if (!cache[path]) continue;
                extra.push_back(s);
                items.push_back({a, b, &extra.back()});
            }
        }
        for (const auto& it : items) {
            const int64_t s0 = pyRound(it.a * kSampleRate);
            const int64_t s1 = std::max(s0 + 1, pyRound(it.b * kSampleRate));
            const double in4 = std::nearbyint(it.s->in * 1e4) / 1e4;
            const double g2 = std::nearbyint(it.s->gainDb * 100) / 100;
            m.evs.push_back({cache[p.resolve(it.s->file)], pyRound(in4 * kSampleRate), s0, s1 - s0, (float)std::pow(10.0, g2 / 20.0)});
            ++nSfx;
        }
    }
    std::stable_sort(m.evs.begin(), m.evs.end(), [](const Ev& x, const Ev& y) { return x.out0 < y.out0; });
    m.evMaxEnd.resize(m.evs.size());
    int64_t mx = 0;
    for (size_t i = 0; i < m.evs.size(); ++i) { mx = std::max(mx, m.evs[i].out0 + m.evs[i].len); m.evMaxEnd[i] = mx; }
    m.hasX = !m.evs.empty();

    // ---- bgm
    if (p.bgm.on) {
        if (loadPcm(p.resolve(p.bgm.file), &m.bgm) && m.bgm->n() > 0) m.hasBgm = true;
        else qWarning("bgm を開けません: %s", qPrintable(p.bgm.file));
    }

    // ---- グラフの文字列(build_audio と同じ)
    std::string g;
    m.names = {"a"};
    if (!silent.empty()) {
        std::string en;
        for (size_t i = 0; i < silent.size(); ++i) {
            char b[96];
            snprintf(b, sizeof b, "%sbetween(t,%.4f,%.4f)", i ? "+" : "", silent[i].first, silent[i].second);
            en += b;
        }
        g += "[a]volume=0:enable='" + en + "'[acat];";
    } else g += "[a]anull[acat];";
    std::vector<std::string> mix{"[acat]"};
    if (m.hasBgm) {
        m.names.push_back("b");
        const double fade = std::min(2.0, totalSec / 4);
        char b[256];
        snprintf(b, sizeof b, "[b]volume=%sdB,afade=t=out:st=%.3f:d=%.3f[bgm];", fmtG(p.bgm.volumeDb).c_str(), totalSec - fade, fade);
        g += b;
        if (p.bgm.duck) {
            g += "[acat]asplit=2[vo][sc];[bgm][sc]sidechaincompress=threshold=0.03:ratio=8:attack=20:release=400[bgmd];";
            mix = {"[vo]", "[bgmd]"};
        } else mix.push_back("[bgm]");
    }
    if (m.hasX) {
        m.names.push_back("x");
        g += "[x]anull[xx];";
        mix.push_back("[xx]");
    }
    const std::string fmt = "aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo";
    if (mix.size() == 1) g += "[acat]" + fmt + "[out]";
    else {
        for (auto& s : mix) g += s;
        g += "amix=inputs=" + std::to_string(mix.size()) + ":duration=first:normalize=0,alimiter=limit=0.891:attack=5:release=50:level=0," + fmt + "[out]";
    }
    m.desc = g;
    m.infoText = QString("audio: A1 区間 %1, aslip %2, 差し込み音 %3, sfx %4, bgm %5%6, 長さ %7 s, 初期化 %8 ms")
                     .arg(m.pieces.size()).arg(nSlip).arg(nOv).arg(nSfx).arg(m.hasBgm ? "あり" : "なし").arg(m.hasBgm && p.bgm.duck ? "(duck)" : "")
                     .arg(totalSec, 0, 'f', 2).arg(tm.elapsed());
    if (!m.startAt(0)) { if (err) *err = "音声グラフを作れません"; return false; }
    return true;
}

}  // namespace vn
