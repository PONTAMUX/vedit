#include "media.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <regex>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

namespace vaudio {

namespace {

std::wstring widen(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], n);
    return w;
}

std::string avErr(int e) {
    char b[256];
    av_strerror(e, b, sizeof b);
    return b;
}

[[noreturn]] void fail(const std::string& m, int e = 0) {
    throw std::runtime_error(e ? m + ": " + avErr(e) : m);
}

}  // namespace

Decoded decodeAudio(const std::string& path, int outSr) {
    AVFormatContext* fmt = nullptr;
    int r = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);
    if (r < 0) fail("音声を開けません: " + path, r);
    struct FmtGuard { AVFormatContext** f; ~FmtGuard() { avformat_close_input(f); } } fg{&fmt};
    r = avformat_find_stream_info(fmt, nullptr);
    if (r < 0) fail("ストリーム情報を読めません", r);
    int si = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (si < 0) fail("音声ストリームがありません: " + path);
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) fail("デコーダがありません");
    AVCodecContext* cc = avcodec_alloc_context3(codec);
    struct CcGuard { AVCodecContext** c; ~CcGuard() { avcodec_free_context(c); } } cg{&cc};
    avcodec_parameters_to_context(cc, st->codecpar);
    cc->pkt_timebase = st->time_base;
    // 先頭の skip(AAC のプライミング)は適用し、末尾の trim はしない。
    // Python 版が使う ffmpeg CLI(WinGet の yt-dlp 同梱版)は末尾を切らないので、サンプル数をそろえる。
    cc->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    r = avcodec_open2(cc, codec, nullptr);
    if (r < 0) fail("デコーダを開けません", r);

    int inSr = cc->sample_rate;
    int sr = outSr > 0 ? outSr : inSr;
    AVChannelLayout layout;
    av_channel_layout_copy(&layout, &cc->ch_layout);
    if (layout.nb_channels == 0) av_channel_layout_default(&layout, 2);
    int nch = layout.nb_channels;

    SwrContext* swr = nullptr;
    r = swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLTP, sr, &layout, cc->sample_fmt, inSr, 0, nullptr);
    if (r < 0) fail("swr 初期化に失敗", r);
    struct SwrGuard { SwrContext** s; ~SwrGuard() { swr_free(s); } } sg{&swr};
    // デコード済みフレームの ch_layout が空(未指定)のとき用に、入力側にも同じレイアウトを明示
    r = swr_init(swr);
    if (r < 0) fail("swr 初期化に失敗", r);

    Decoded out;
    out.sampleRate = sr;
    out.ch.assign((size_t)nch, std::vector<float>());

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    struct FGuard { AVPacket** p; AVFrame** f; ~FGuard() { av_packet_free(p); av_frame_free(f); } } fgd{&pkt, &frame};

    std::vector<std::vector<float>> tmp((size_t)nch);
    auto convert = [&](AVFrame* f) {
        if (f) {
            AVFrameSideData* sd = av_frame_get_side_data(f, AV_FRAME_DATA_SKIP_SAMPLES);
            if (sd && sd->size >= 10) {
                uint32_t skipStart; memcpy(&skipStart, sd->data, 4);
                if (skipStart >= (uint32_t)f->nb_samples) return;
                if (skipStart) {
                    for (int c = 0; c < nch; c++) f->extended_data[c] += sizeof(float) * skipStart;  // fltp 想定でなければ下で変換
                    f->nb_samples -= (int)skipStart;
                }
            }
        }
        int maxOut = swr_get_out_samples(swr, f ? f->nb_samples : 0) + 64;
        for (auto& t : tmp) t.resize((size_t)maxOut);
        std::vector<uint8_t*> op((size_t)nch);
        for (int c = 0; c < nch; c++) op[(size_t)c] = reinterpret_cast<uint8_t*>(tmp[(size_t)c].data());
        int got = swr_convert(swr, op.data(), maxOut, f ? (const uint8_t**)f->extended_data : nullptr, f ? f->nb_samples : 0);
        if (got < 0) fail("リサンプルに失敗", got);
        for (int c = 0; c < nch; c++) out.ch[(size_t)c].insert(out.ch[(size_t)c].end(), tmp[(size_t)c].begin(), tmp[(size_t)c].begin() + got);
    };
    auto drain = [&]() {
        while (true) {
            int e = avcodec_receive_frame(cc, frame);
            if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return;
            if (e < 0) fail("デコードに失敗", e);
            convert(frame);
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == si) {
            r = avcodec_send_packet(cc, pkt);
            if (r < 0 && r != AVERROR(EAGAIN)) { av_packet_unref(pkt); fail("デコードに失敗", r); }
            drain();
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(cc, nullptr);
    drain();
    // リサンプラに残った分を出す
    while (true) {
        int maxOut = swr_get_out_samples(swr, 0) + 64;
        for (auto& t : tmp) t.resize((size_t)maxOut);
        std::vector<uint8_t*> op((size_t)nch);
        for (int c = 0; c < nch; c++) op[(size_t)c] = reinterpret_cast<uint8_t*>(tmp[(size_t)c].data());
        int got = swr_convert(swr, op.data(), maxOut, nullptr, 0);
        if (got <= 0) break;
        for (int c = 0; c < nch; c++) out.ch[(size_t)c].insert(out.ch[(size_t)c].end(), tmp[(size_t)c].begin(), tmp[(size_t)c].begin() + got);
    }
    av_channel_layout_uninit(&layout);
    return out;
}

Decoded readAnyAudio(const std::string& p) { return decodeAudio(p, 0); }

// ------------------------------------------------------------------ フィルタグラフ
namespace {

struct GraphIO {
    AVFilterGraph* g = nullptr;
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    ~GraphIO() { if (g) avfilter_graph_free(&g); }
};

void buildGraph(GraphIO& gi, int nch, int sr, const std::string& filterSpec, bool forceFltp) {
    gi.g = avfilter_graph_alloc();
    if (!gi.g) fail("filter graph");
    AVChannelLayout lay;
    av_channel_layout_default(&lay, nch);
    char layname[128];
    av_channel_layout_describe(&lay, layname, sizeof layname);
    av_channel_layout_uninit(&lay);
    char args[512];
    snprintf(args, sizeof args, "time_base=1/%d:sample_rate=%d:sample_fmt=fltp:channel_layout=%s", sr, sr, layname);
    int r = avfilter_graph_create_filter(&gi.src, avfilter_get_by_name("abuffer"), "in", args, nullptr, gi.g);
    if (r < 0) fail("abuffer", r);
    gi.sink = avfilter_graph_alloc_filter(gi.g, avfilter_get_by_name("abuffersink"), "out");
    if (!gi.sink) fail("abuffersink");
    if (forceFltp) {
        r = av_opt_set(gi.sink, "sample_formats", "fltp", AV_OPT_SEARCH_CHILDREN);
        if (r < 0) fail("sample_formats", r);
    }
    r = avfilter_init_str(gi.sink, nullptr);
    if (r < 0) fail("abuffersink init", r);
    AVFilterInOut* outs = avfilter_inout_alloc();
    AVFilterInOut* ins = avfilter_inout_alloc();
    outs->name = av_strdup("in");
    outs->filter_ctx = gi.src;
    outs->pad_idx = 0;
    outs->next = nullptr;
    ins->name = av_strdup("out");
    ins->filter_ctx = gi.sink;
    ins->pad_idx = 0;
    ins->next = nullptr;
    r = avfilter_graph_parse_ptr(gi.g, filterSpec.c_str(), &ins, &outs, nullptr);
    avfilter_inout_free(&ins);
    avfilter_inout_free(&outs);
    if (r < 0) fail("フィルタ指定が不正です: " + filterSpec, r);
    r = avfilter_graph_config(gi.g, nullptr);
    if (r < 0) fail("フィルタを構成できません: " + filterSpec, r);
}

// 入力を流し込みながら、出てきたフレームを fn に渡す
template <class F>
void pump(GraphIO& gi, const Audio& a, int sr, F&& onFrame) {
    int nch = (int)a.size();
    size_t total = a.empty() ? 0 : a[0].size();
    const size_t CH = 4096;
    AVFrame* fr = av_frame_alloc();
    AVFrame* of = av_frame_alloc();
    struct G { AVFrame** a; AVFrame** b; ~G() { av_frame_free(a); av_frame_free(b); } } guard{&fr, &of};
    auto drainOut = [&]() {
        while (true) {
            int r = av_buffersink_get_frame(gi.sink, of);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return;
            if (r < 0) fail("フィルタ出力に失敗", r);
            onFrame(of);
            av_frame_unref(of);
        }
    };
    int64_t pts = 0;
    for (size_t pos = 0; pos < total; pos += CH) {
        int n = (int)std::min(CH, total - pos);
        av_frame_unref(fr);
        fr->nb_samples = n;
        fr->format = AV_SAMPLE_FMT_FLTP;
        fr->sample_rate = sr;
        av_channel_layout_default(&fr->ch_layout, nch);
        fr->pts = pts;
        int r = av_frame_get_buffer(fr, 0);
        if (r < 0) fail("frame alloc", r);
        for (int c = 0; c < nch; c++) std::memcpy(fr->extended_data[c], a[(size_t)c].data() + pos, sizeof(float) * (size_t)n);
        pts += n;
        r = av_buffersrc_add_frame_flags(gi.src, fr, 0);
        if (r < 0) fail("フィルタ入力に失敗", r);
        drainOut();
    }
    int r = av_buffersrc_add_frame_flags(gi.src, nullptr, 0);
    if (r < 0) fail("フィルタ終端に失敗", r);
    drainOut();
}

// ログ捕捉(ebur128 の Summary は INFO で出る)
std::string g_log;
std::mutex g_logMu;
void logCb(void*, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_INFO) return;
    char buf[1024];
    vsnprintf(buf, sizeof buf, fmt, vl);
    g_log += buf;
}

}  // namespace

double loudness(const Audio& a, int sr) {
    std::lock_guard<std::mutex> lk(g_logMu);
    g_log.clear();
    int oldLevel = av_log_get_level();
    av_log_set_level(AV_LOG_INFO);
    av_log_set_callback(logCb);
    try {
        GraphIO gi;
        buildGraph(gi, (int)a.size(), sr, "ebur128", false);
        pump(gi, a, sr, [](AVFrame*) {});
        avfilter_graph_free(&gi.g);  // uninit で Summary が出る
    } catch (...) {
        av_log_set_callback(av_log_default_callback);
        av_log_set_level(oldLevel);
        throw;
    }
    av_log_set_callback(av_log_default_callback);
    av_log_set_level(oldLevel);
    std::regex re("I:\\s+(-?[\\d.]+) LUFS");
    double val = 0;
    bool found = false;
    for (auto it = std::sregex_iterator(g_log.begin(), g_log.end(), re); it != std::sregex_iterator(); ++it) {
        val = atof((*it)[1].str().c_str());
        found = true;
    }
    if (!found) throw std::runtime_error("ラウドネスを測れませんでした");
    return val;
}

Audio dynaudnorm(const Audio& a, int sr, const std::string& args) {
    GraphIO gi;
    buildGraph(gi, (int)a.size(), sr, "dynaudnorm=" + args, true);
    Audio out(a.size());
    pump(gi, a, sr, [&](AVFrame* f) {
        for (size_t c = 0; c < out.size(); c++) {
            const float* p = reinterpret_cast<const float*>(f->extended_data[c]);
            out[c].insert(out[c].end(), p, p + f->nb_samples);
        }
    });
    return out;
}

void writeWav24(const std::string& path, const Audio& a, int sr, double gainDb) {
    int nch = (int)a.size();
    size_t n = nch ? a[0].size() : 0;
    char gbuf[64];
    snprintf(gbuf, sizeof gbuf, "%.4f", gainDb);  // Python 版: volume={gain:.4f}dB
    double dbv = atof(gbuf);
    float vol = static_cast<float>(std::pow(10.0, dbv / 20.0));

    FILE* f = _wfopen(widen(path).c_str(), L"wb");
    if (!f) throw std::runtime_error("書き込めません: " + path);
    uint32_t dataBytes = (uint32_t)(n * (size_t)nch * 3);
    auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    fwrite("RIFF", 1, 4, f); w32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16((uint16_t)nch); w32((uint32_t)sr); w32((uint32_t)(sr * nch * 3)); w16((uint16_t)(nch * 3)); w16(24);
    fwrite("data", 1, 4, f); w32(dataBytes);
    std::vector<uint8_t> buf;
    const size_t BLK = 65536;
    for (size_t pos = 0; pos < n; pos += BLK) {
        size_t m = std::min(BLK, n - pos);
        buf.resize(m * (size_t)nch * 3);
        size_t o = 0;
        for (size_t i = 0; i < m; i++)
            for (int c = 0; c < nch; c++) {
                float x = a[(size_t)c][pos + i] * vol;
                // swresample の flt→s32: av_clipl_int32(llrintf(x * 2^31))、pcm_s24le は >> 8
                long long v = llrintf(x * 2147483648.0f);
                if (v > 2147483647LL) v = 2147483647LL;
                if (v < -2147483648LL) v = -2147483648LL;
                int32_t s24 = (int32_t)v >> 8;
                buf[o++] = (uint8_t)(s24 & 0xFF);
                buf[o++] = (uint8_t)((s24 >> 8) & 0xFF);
                buf[o++] = (uint8_t)((s24 >> 16) & 0xFF);
            }
        fwrite(buf.data(), 1, buf.size(), f);
    }
    fclose(f);
}

}  // namespace vaudio
