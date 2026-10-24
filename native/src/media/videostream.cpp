#include "videostream.h"
#include <QFileInfo>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <map>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace vn {

static bool vtraceOn() { static const bool t = getenv("VN_TRACE") != nullptr; return t; }
static void vtrace(const char* fmt, ...) {
    if (!vtraceOn()) return;
    static const auto t0 = std::chrono::steady_clock::now();
    fprintf(stderr, "[%7.3f] ", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc(10, stderr);
}

// ---------------------------------------------------------------- 情報の取得

static bool isImageExt(const QString& p) {
    const QString e = p.section('.', -1).toLower();
    return e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "bmp";
}

MediaProps probeMedia(const QString& path) {
    static QMutex mu;
    static std::map<QString, MediaProps> cache;
    {
        QMutexLocker l(&mu);
        auto it = cache.find(path);
        if (it != cache.end()) return it->second;
    }
    MediaProps r;
    if (!QFileInfo::exists(path)) {
        // 無いファイル
    } else if (isImageExt(path)) {
        QImageReader rd(path);
        const QSize s = rd.size();
        if (s.isValid()) { r.ok = true; r.isImage = true; r.w = s.width(); r.h = s.height(); }
    } else {
        AVFormatContext* f = nullptr;
        const QByteArray u8 = path.toUtf8();
        if (avformat_open_input(&f, u8.constData(), nullptr, nullptr) == 0) {
            if (avformat_find_stream_info(f, nullptr) >= 0) {
                int vi = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
                if (vi >= 0) {
                    AVStream* st = f->streams[vi];
                    r.ok = true;
                    r.w = st->codecpar->width;
                    r.h = st->codecpar->height;
                    AVRational fr = st->r_frame_rate.num > 0 ? st->r_frame_rate : st->avg_frame_rate;
                    r.fps = fr.num > 0 ? av_q2d(fr) : 30.0;
                    r.start = st->start_time != AV_NOPTS_VALUE ? st->start_time * av_q2d(st->time_base) : 0.0;
                    r.duration = f->duration != AV_NOPTS_VALUE ? f->duration / (double)AV_TIME_BASE
                                 : (st->duration != AV_NOPTS_VALUE ? st->duration * av_q2d(st->time_base) : 0.0);
                }
            }
            avformat_close_input(&f);
        }
    }
    QMutexLocker l(&mu);
    cache[path] = r;
    return r;
}

// ---------------------------------------------------------------- NVDEC の器(共有)

static std::mutex g_hwMu;
static AVBufferRef* g_hwDev = nullptr;
static bool g_hwTried = false;
static int g_hwCount = 0;
static constexpr int kHwMax = 6;   // 同時に NVDEC で開く本数(VRAM を使い切らないように)

static AVBufferRef* acquireHw() {
    std::lock_guard<std::mutex> l(g_hwMu);
    if (!g_hwTried) {
        g_hwTried = true;
        if (av_hwdevice_ctx_create(&g_hwDev, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) g_hwDev = nullptr;
        // AVCUDADeviceContext の先頭メンバが CUcontext
        else if (void* hc = ((AVHWDeviceContext*)g_hwDev->data)->hwctx) cuda::setContext(*(void**)hc);
    }
    if (!g_hwDev || g_hwCount >= kHwMax) return nullptr;
    ++g_hwCount;
    return g_hwDev;
}
static bool takeHwSlot() {
    std::lock_guard<std::mutex> l(g_hwMu);
    if (!g_hwDev || g_hwCount >= kHwMax) return false;
    ++g_hwCount;
    return true;
}
static void releaseHw() {
    std::lock_guard<std::mutex> l(g_hwMu);
    --g_hwCount;
}


// 使い終えた NVDEC のデコーダ(AVCodecContext)を捨てずに取っておき、同じ形式の次のファイルで使い回す。
// NVDEC の作成(cuvidCreateDecoder)は GPU 全体を数十〜百数十 ms 止め、描画の glFinish が引っかかる原因になるため。
struct PooledCtx { AVCodecContext* ctx; int codec, w, h, fmt; std::vector<uint8_t> extra; };
static std::mutex g_poolMu;
static std::vector<PooledCtx> g_pool;
static constexpr size_t kPoolMax = 4;
static bool takeHwSlot();

static AVCodecContext* poolTake(const AVCodecParameters* par) {
    std::lock_guard<std::mutex> l(g_poolMu);
    if (g_pool.empty()) return nullptr;
    for (size_t i = 0; i < g_pool.size(); ++i) {
        auto& e = g_pool[i];
        if (e.codec == (int)par->codec_id && e.w == par->width && e.h == par->height && e.fmt == par->format &&
            e.extra.size() == (size_t)par->extradata_size &&
            (e.extra.empty() || std::memcmp(e.extra.data(), par->extradata, e.extra.size()) == 0)) {
            if (!takeHwSlot()) return nullptr;   // NVDEC の同時本数の上限
            AVCodecContext* c = e.ctx;
            g_pool.erase(g_pool.begin() + i);
            return c;
        }
    }
    return nullptr;
}
static void poolPut(AVCodecContext* c, const AVCodecParameters* par) {
    avcodec_flush_buffers(c);
    PooledCtx e{c, (int)par->codec_id, par->width, par->height, par->format,
                std::vector<uint8_t>(par->extradata, par->extradata + std::max(0, par->extradata_size))};
    AVCodecContext* drop = nullptr;
    {
        std::lock_guard<std::mutex> l(g_poolMu);
        g_pool.push_back(std::move(e));
        if (g_pool.size() > kPoolMax) { drop = g_pool.front().ctx; g_pool.erase(g_pool.begin()); }
    }
    if (drop) avcodec_free_context(&drop);
}

static AVPixelFormat getCuda(AVCodecContext*, const AVPixelFormat* f) {
    for (; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == AV_PIX_FMT_CUDA) return *f;
    return AV_PIX_FMT_NONE;
}

// ---------------------------------------------------------------- VideoStream

void VideoStream::warmUp(const std::vector<std::string>& paths) {
    std::vector<std::unique_ptr<VideoStream>> v;
    for (const auto& p : paths) {
        auto s = std::make_unique<VideoStream>(p, true, 0.0);
        s->frameAt(0.0, true, 0.1);   // 最初のコマが出る = デコーダができた
        v.push_back(std::move(s));
    }
    v.clear();   // 破棄 → 作ったデコーダは置き場へ(poolPut)
}

VideoStream::VideoStream(const std::string& path, bool tryHw, double initialTime)
    : path_(path), tryHw_(tryHw), initial_(initialTime) {
    seekT_ = target_ = initialTime;
    seekReq_ = initialTime > 0.001;
    th_ = vn::Thread([this] { run(); });
}

VideoStream::~VideoStream() {
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
    }
    cv_work_.notify_all();
    cv_done_.notify_all();
    if (th_.joinable()) th_.join();
    closeAll();
}

bool VideoStream::openAll(bool hw) {
    if (avformat_open_input(&fmt_, path_.c_str(), nullptr, nullptr) < 0) { fmt_ = nullptr; return false; }
    if (avformat_find_stream_info(fmt_, nullptr) < 0) return false;
    vidx_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx_ < 0) return false;
    for (unsigned i = 0; i < fmt_->nb_streams; ++i)
        if ((int)i != vidx_) fmt_->streams[i]->discard = AVDISCARD_ALL;
    AVStream* st = fmt_->streams[vidx_];
    AVCodecParameters* par = st->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) return false;
    if (hw && (ctx_ = poolTake(par))) {
        // 取っておいたデコーダをそのまま使う(NVDEC の作り直しなし。HW の枠も引き継ぐ)
        ctx_->pkt_timebase = st->time_base;
        tb_ = av_q2d(st->time_base);
        AVRational fr0 = st->r_frame_rate.num > 0 ? st->r_frame_rate : st->avg_frame_rate;
        fps_ = fr0.num > 0 ? av_q2d(fr0) : 30.0;
        startTime_ = st->start_time != AV_NOPTS_VALUE ? st->start_time * tb_ : 0.0;
        eps_ = 0.25 / fps_;
        hw_ = true;
        pooled_ = true;
        pkt_ = av_packet_alloc();
        fr_ = av_frame_alloc();
        draining_ = false;
        usingHw_ = true;
        return true;
    }
    ctx_ = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx_, par);
    ctx_->pkt_timebase = st->time_base;
    tb_ = av_q2d(st->time_base);
    AVRational fr = st->r_frame_rate.num > 0 ? st->r_frame_rate : st->avg_frame_rate;
    fps_ = fr.num > 0 ? av_q2d(fr) : 30.0;
    startTime_ = st->start_time != AV_NOPTS_VALUE ? st->start_time * tb_ : 0.0;
    eps_ = 0.25 / fps_;
    hw_ = false;
    const bool hwOk = hw && (par->codec_id == AV_CODEC_ID_H264 || par->codec_id == AV_CODEC_ID_HEVC ||
                             par->codec_id == AV_CODEC_ID_VP9 || par->codec_id == AV_CODEC_ID_AV1) &&
                      (par->format == AV_PIX_FMT_YUV420P || par->format == AV_PIX_FMT_YUVJ420P ||
                       par->format == AV_PIX_FMT_YUV420P10LE) && par->width <= 4096;
    if (hwOk) {
        if (AVBufferRef* dev = acquireHw()) {
            ctx_->hw_device_ctx = av_buffer_ref(dev);
            ctx_->get_format = getCuda;
            ctx_->extra_hw_frames = 2;
            hw_ = true;
        }
    }
    if (!hw_) ctx_->thread_count = std::min(4u, std::max(1u, std::thread::hardware_concurrency() / 2));
    if (avcodec_open2(ctx_, codec, nullptr) < 0) return false;
    pkt_ = av_packet_alloc();
    fr_ = av_frame_alloc();
    draining_ = false;
    usingHw_ = hw_;
    return true;
}

void VideoStream::closeAll() {
    if (fr_) av_frame_free(&fr_);
    if (ctx_ && hw_ && usingHw_ && !noPool_ && pkt_ && fmt_ && vidx_ >= 0 && gotFrame_) {
        poolPut(ctx_, fmt_->streams[vidx_]->codecpar);
        ctx_ = nullptr;
    }
    if (pkt_) av_packet_free(&pkt_);
    if (ctx_) avcodec_free_context(&ctx_);
    if (fmt_) avformat_close_input(&fmt_);
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (hw_) { releaseHw(); hw_ = false; }
    pooled_ = false;
    usingHw_ = false;
}

void VideoStream::doSeek(double t) {
    if (!fmt_) return;
    double tt = std::max(t, startTime_);
    int64_t ts = (int64_t)std::floor(tt / tb_);
    if (av_seek_frame(fmt_, vidx_, ts, AVSEEK_FLAG_BACKWARD) < 0) av_seek_frame(fmt_, vidx_, 0, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(ctx_);
    draining_ = false;
}

static void copyRows(uint8_t* dst, int dstStride, const uint8_t* src, int srcStride, int rowBytes, int rows) {
    for (int j = 0; j < rows; ++j) std::memcpy(dst + (size_t)j * dstStride, src + (size_t)j * srcStride, rowBytes);
}

FramePtr VideoStream::convert(AVFrame* f) {
    if (f->format == AV_PIX_FMT_CUDA && f->hw_frames_ctx && cuda::enabled()) {
        const AVPixelFormat sf = ((AVHWFramesContext*)f->hw_frames_ctx->data)->sw_format;
        if (sf == AV_PIX_FMT_NV12 || sf == AV_PIX_FMT_P010LE) {
            const int bps = sf == AV_PIX_FMT_NV12 ? 1 : 2;
            auto dev = cuda::copyFromDecoder((unsigned long long)(uintptr_t)f->data[0], f->linesize[0],
                                             (unsigned long long)(uintptr_t)f->data[1], f->linesize[1], f->width, f->height, bps);
            if (dev) {
                auto v = std::make_shared<VFrame>();
                v->kind = bps == 1 ? VFrame::NV12 : VFrame::P010;
                v->w = f->width; v->h = f->height;
                v->cw = (v->w + 1) / 2; v->ch = (v->h + 1) / 2;
                int64_t ts = f->best_effort_timestamp;
                if (ts == AV_NOPTS_VALUE) ts = f->pts;
                v->pts = ts == AV_NOPTS_VALUE ? lastPts_ + 1.0 / fps_ : ts * tb_;
                lastPts_ = v->pts;
                switch (f->colorspace) {
                    case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_SMPTE240M: v->cs = 1; break;
                    case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: v->cs = 2; break;
                    default: v->cs = 0; break;
                }
                v->full = f->color_range == AVCOL_RANGE_JPEG;
                v->dev = dev;
                static std::atomic<uint64_t> gid2{1ull << 40};
                v->id = gid2++;
                return v;
            }
            cuda::disable();   // 以後は CPU 経路
        }
    }
    AVFrame* sw = f;
    AVFrame* tmp = nullptr;
    if (f->format == AV_PIX_FMT_CUDA) {
        tmp = av_frame_alloc();
        if (av_hwframe_transfer_data(tmp, f, 0) < 0) { av_frame_free(&tmp); return nullptr; }
        av_frame_copy_props(tmp, f);
        sw = tmp;
    }
    auto v = std::make_shared<VFrame>();
    v->w = sw->width; v->h = sw->height;
    v->cw = (v->w + 1) / 2; v->ch = (v->h + 1) / 2;
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f->pts;
    v->pts = ts == AV_NOPTS_VALUE ? lastPts_ + 1.0 / fps_ : ts * tb_;
    lastPts_ = v->pts;
    switch (sw->colorspace) {
        case AVCOL_SPC_BT470BG: case AVCOL_SPC_SMPTE170M: case AVCOL_SPC_SMPTE240M: v->cs = 1; break;
        case AVCOL_SPC_BT2020_NCL: case AVCOL_SPC_BT2020_CL: v->cs = 2; break;
        default: v->cs = 0; break;   // 既定は BT.709
    }
    const AVPixelFormat pf = (AVPixelFormat)sw->format;
    v->full = sw->color_range == AVCOL_RANGE_JPEG || pf == AV_PIX_FMT_YUVJ420P;
    const int w = v->w, h = v->h, cw = v->cw, ch = v->ch;
    if (pf == AV_PIX_FMT_NV12 || pf == AV_PIX_FMT_P010LE) {
        const int bps = pf == AV_PIX_FMT_NV12 ? 1 : 2;
        v->kind = bps == 1 ? VFrame::NV12 : VFrame::P010;
        v->y.reset(new uint8_t[(size_t)w * h * bps]);
        v->uv.reset(new uint8_t[(size_t)cw * 2 * bps * ch]);
        copyRows(v->y.get(), w * bps, sw->data[0], sw->linesize[0], w * bps, h);
        copyRows(v->uv.get(), cw * 2 * bps, sw->data[1], sw->linesize[1], cw * 2 * bps, ch);
    } else if (pf == AV_PIX_FMT_YUV420P || pf == AV_PIX_FMT_YUVJ420P) {
        v->kind = VFrame::NV12;
        v->y.reset(new uint8_t[(size_t)w * h]);
        v->uv.reset(new uint8_t[(size_t)cw * 2 * ch]);
        copyRows(v->y.get(), w, sw->data[0], sw->linesize[0], w, h);
        for (int j = 0; j < ch; ++j) {
            const uint8_t* u = sw->data[1] + (size_t)j * sw->linesize[1];
            const uint8_t* vv = sw->data[2] + (size_t)j * sw->linesize[2];
            uint8_t* d = v->uv.get() + (size_t)j * cw * 2;
            for (int i = 0; i < cw; ++i) { d[2 * i] = u[i]; d[2 * i + 1] = vv[i]; }
        }
    } else if (pf == AV_PIX_FMT_YUV420P10LE) {
        v->kind = VFrame::P010;
        v->y.reset(new uint8_t[(size_t)w * h * 2]);
        v->uv.reset(new uint8_t[(size_t)cw * 4 * ch]);
        for (int j = 0; j < h; ++j) {
            const uint16_t* s = (const uint16_t*)(sw->data[0] + (size_t)j * sw->linesize[0]);
            uint16_t* d = (uint16_t*)(v->y.get() + (size_t)j * w * 2);
            for (int i = 0; i < w; ++i) d[i] = (uint16_t)(s[i] << 6);
        }
        for (int j = 0; j < ch; ++j) {
            const uint16_t* u = (const uint16_t*)(sw->data[1] + (size_t)j * sw->linesize[1]);
            const uint16_t* vv = (const uint16_t*)(sw->data[2] + (size_t)j * sw->linesize[2]);
            uint16_t* d = (uint16_t*)(v->uv.get() + (size_t)j * cw * 4);
            for (int i = 0; i < cw; ++i) { d[2 * i] = (uint16_t)(u[i] << 6); d[2 * i + 1] = (uint16_t)(vv[i] << 6); }
        }
    } else {
        // 透明つき・4:2:2/4:4:4・RGB など: RGBA にして持つ(シェーダ側で透明度を掛ける)
        v->kind = VFrame::RGBA;
        v->y.reset(new uint8_t[(size_t)w * h * 4]);
        sws_ = sws_getCachedContext(sws_, w, h, pf, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) { if (tmp) av_frame_free(&tmp); return nullptr; }
        const int* coef = sws_getCoefficients(v->cs == 1 ? SWS_CS_ITU601 : v->cs == 2 ? SWS_CS_BT2020 : SWS_CS_ITU709);
        sws_setColorspaceDetails(sws_, coef, v->full ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
        uint8_t* dst[4] = {v->y.get(), nullptr, nullptr, nullptr};
        int dls[4] = {w * 4, 0, 0, 0};
        sws_scale(sws_, sw->data, sw->linesize, 0, h, dst, dls);
    }
    if (tmp) av_frame_free(&tmp);
    static std::atomic<uint64_t> gid{1};
    v->id = gid++;
    return v;
}

int VideoStream::decodeOne(FramePtr* out) {
    for (;;) {
        int r = avcodec_receive_frame(ctx_, fr_);
        if (r == 0) {
            *out = convert(fr_);
            av_frame_unref(fr_);
            return *out ? 1 : -1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return -1;
        if (draining_) return 0;
        int rr = av_read_frame(fmt_, pkt_);
        if (rr < 0) { avcodec_send_packet(ctx_, nullptr); draining_ = true; continue; }
        if (pkt_->stream_index != vidx_) { av_packet_unref(pkt_); continue; }
        int sr = avcodec_send_packet(ctx_, pkt_);
        av_packet_unref(pkt_);
        if (sr == AVERROR_INVALIDDATA) continue;
        if (sr < 0 && sr != AVERROR(EAGAIN)) return -1;
    }
}

bool VideoStream::needMoreLocked() const {
    if (q_.size() >= 240) return false;
    return q_.empty() || q_.back()->pts < target_ + ahead_;
}

void VideoStream::updateTargetLocked(double time, double ahead) {
    target_ = time;
    ahead_ = ahead;
    bool back, far;
    if (q_.empty()) {
        back = time < seekT_ - 0.5;
        far = time > seekT_ + 2.0;
    } else {
        back = time < q_.front()->pts - eps_;
        far = time > q_.back()->pts + 2.0 && time > seekT_ + 2.0;   // シーク直後は目的の時刻までキーフレームから進めるだけ
    }
    if ((back || far) && !(seekReq_ && std::fabs(seekT_ - time) < 0.5)) {
        seekT_ = time;
        seekReq_ = true;
        eof_ = false;
        q_.clear();
        ++gen_;
    }
    cv_work_.notify_one();
}

void VideoStream::retarget(double time, double ahead) {
    std::lock_guard<std::mutex> l(m_);
    updateTargetLocked(time, ahead);
}

FramePtr VideoStream::frameAt(double time, bool wait, double ahead) {
    std::unique_lock<std::mutex> lk(m_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (;;) {
        updateTargetLocked(time, ahead);
        FramePtr cand;
        bool haveNext = false;
        for (const auto& f : q_) {
            if (f->pts <= time + eps_) cand = f;
            else { haveNext = true; break; }
        }
        if (!wait) return cand;
        if (cand && (haveNext || eof_)) return cand;
        if (failed_ || (eof_ && !seekReq_)) return cand;
        if (cv_done_.wait_until(lk, deadline) == std::cv_status::timeout) return cand;
    }
}

void VideoStream::run() {
    bool ok = openAll(tryHw_);
    if (!ok) {
        closeAll();
        ok = tryHw_ ? openAll(false) : false;
    }
    if (!ok) {
        failed_ = true;
        cv_done_.notify_all();
        return;
    }
    vtrace("open %s hw=%d pooled=%d initial=%.3f", path_.c_str() + (path_.size() > 30 ? path_.size() - 30 : 0), (int)hw_, (int)pooled_, initial_);
    if (hw_) cuda::bindThread();
    std::unique_lock<std::mutex> lk(m_);
    opened_ = true;
    bool firstAfterSeek = true;
    for (;;) {
        cv_work_.wait(lk, [&] { return quit_ || seekReq_ || (!eof_ && needMoreLocked()); });
        if (quit_) break;
        if (seekReq_) {
            const double t = seekT_;
            seekReq_ = false;
            lk.unlock();
            vtrace("seek %.3f (%s)", t, path_.c_str() + (path_.size() > 30 ? path_.size() - 30 : 0));
            doSeek(t);
            firstAfterSeek = true;
            lk.lock();
            continue;
        }
        const uint64_t g = gen_;
        lk.unlock();
        FramePtr f;
        int r = decodeOne(&f);
        lk.lock();
        if (g != gen_) continue;   // 読んでいる間にシークが入った
        if (r == 1) {
            if (firstAfterSeek) { vtrace("first frame pts=%.3f target=%.3f (%s)", f->pts, target_, path_.c_str() + (path_.size() > 30 ? path_.size() - 30 : 0)); firstAfterSeek = false; }
            q_.push_back(f);
            gotFrame_ = true;
            while (q_.size() >= 2 && q_[1]->pts <= target_ + eps_) q_.pop_front();
        } else if (r == 0) {
            eof_ = true;
        } else if (hw_ && !gotFrame_) {
            // NVDEC で最初のコマが取れない → CPU で開き直す
            const double t = seekT_;
            lk.unlock();
            noPool_ = true;
            closeAll();
            bool ok2 = openAll(false);
            if (ok2) doSeek(t);
            lk.lock();
            if (!ok2) { failed_ = true; eof_ = true; }
        } else {
            eof_ = true;
        }
        cv_done_.notify_all();
    }
}

}  // namespace vn
