#include "subrender.h"
#include <QDir>
#include <QFile>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <windows.h>
#include "media/membudget.h"
#endif

namespace vn {
static double t_ass_ms = 0;

struct SubRender::Graph {
    AVFilterGraph* g = nullptr;
    AVFilterContext *src = nullptr, *ass = nullptr, *sink = nullptr;
    AVFrame* base[2] = {nullptr, nullptr};   // 黒地(Y=16)・白地(Y=235)の yuv420p。毎回 ass に通す(中身は変えない)
    SwsContext* sws = nullptr;
    std::vector<uint8_t> row16, row235;
};

SubRender::SubRender(int W, int H, const QString& assText) : W_(W), H_(H), assText_(assText) {
    av_log_set_level(AV_LOG_WARNING);   // libass の font 選択ログ(info)を出さない
    // ass フィルタがファイルで読むので、プロジェクトのフォルダ・キャッシュではない一時の場所に書く
    QString dir = QDir("C:/Temp").exists() ? QString("C:/Temp/vedit-native-subs") : QDir::temp().filePath("vedit-native-subs");
    QDir().mkpath(dir);
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    assPath_ = QString("%1/%2_%3.ass").arg(dir).arg(pid).arg((quintptr)this, 0, 16);
    QFile f(assPath_);
    if (f.open(QIODevice::WriteOnly)) { f.write(assText_.toUtf8()); f.close(); }
    else failed_ = true;
    th_ = vn::Thread([this] { run(); });
}

SubRender::~SubRender() {
    {
        std::lock_guard<std::mutex> l(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    done_.notify_all();
    if (th_.joinable()) th_.join();
    if (!qEnvironmentVariableIsSet("VN_KEEPASS")) QFile::remove(assPath_);
}

bool SubRender::peek(const QString& key, Img* out) {
    std::lock_guard<std::mutex> l(mu_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    if (out) *out = it.value();
    return true;
}

void SubRender::request(const QString& key, long long nowMs, bool urgent) {
    std::lock_guard<std::mutex> l(mu_);
    if (cache_.contains(key) || pending_.contains(key)) return;
    pending_.insert(key, 1);
    lastReq_ = nowMs;
    if (urgent) q_.push_front({key, nowMs}); else q_.push_back({key, nowMs});
    cv_.notify_one();
}

bool SubRender::get(const QString& key, long long nowMs, Img* out) {
    std::unique_lock<std::mutex> l(mu_);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        must_.insert(key, 1);
        if (!pending_.contains(key)) {
            pending_.insert(key, 1);
            q_.push_front({key, nowMs});
            cv_.notify_one();
        } else {   // 先回りで積んだものを先頭に出す
            for (auto qi = q_.begin(); qi != q_.end(); ++qi)
                if (qi->first == key) { auto v = *qi; q_.erase(qi); q_.push_front(v); break; }
        }
        done_.wait(l, [&] { return quit_ || cache_.contains(key); });
        it = cache_.find(key);
        if (it == cache_.end()) return false;
    }
    if (out) *out = it.value();
    return true;
}

bool SubRender::initGraph() {
    g_ = new Graph;
    Graph& G = *g_;
    G.g = avfilter_graph_alloc();
    if (!G.g) return false;
    G.g->nb_threads = 1;
    // 書き出しと同じ条件(yuv420p・bt709・tv)の地に ass を通す。RGB のまま通すと色が違う(白が 235 になる)
    char args[256];
    snprintf(args, sizeof args, "video_size=%dx%d:pix_fmt=yuv420p:time_base=1/1000:pixel_aspect=1/1:colorspace=bt709:range=tv", W_, H_);
    if (avfilter_graph_create_filter(&G.src, avfilter_get_by_name("buffer"), "in", args, nullptr, G.g) < 0) return false;
    const AVFilter* af = avfilter_get_by_name("ass");
    if (!af) { fprintf(stderr, "subrender: ass フィルタがありません(libass 無しの ffmpeg)\n"); return false; }
    G.ass = avfilter_graph_alloc_filter(G.g, af, "ass");
    if (!G.ass) return false;
    av_opt_set(G.ass, "filename", assPath_.toUtf8().constData(), AV_OPT_SEARCH_CHILDREN);
    if (avfilter_init_str(G.ass, nullptr) < 0) { fprintf(stderr, "subrender: ass の初期化に失敗\n"); return false; }
    if (avfilter_graph_create_filter(&G.sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, G.g) < 0) return false;
    if (avfilter_link(G.src, 0, G.ass, 0) < 0 || avfilter_link(G.ass, 0, G.sink, 0) < 0) return false;
    if (avfilter_graph_config(G.g, nullptr) < 0) { fprintf(stderr, "subrender: graph config 失敗\n"); return false; }
    for (int pass = 0; pass < 2; ++pass) {
        AVFrame* f = av_frame_alloc();
        f->format = AV_PIX_FMT_YUV420P;
        f->width = W_;
        f->height = H_;
        f->sample_aspect_ratio = AVRational{1, 1};
        f->colorspace = AVCOL_SPC_BT709;
        f->color_range = AVCOL_RANGE_MPEG;
        if (av_frame_get_buffer(f, 32) < 0) return false;
        for (int y = 0; y < H_; ++y) memset(f->data[0] + (size_t)y * f->linesize[0], pass ? 235 : 16, (size_t)W_);
        for (int y = 0; y < (H_ + 1) / 2; ++y) {
            memset(f->data[1] + (size_t)y * f->linesize[1], 128, (size_t)(W_ + 1) / 2);
            memset(f->data[2] + (size_t)y * f->linesize[2], 128, (size_t)(W_ + 1) / 2);
        }
        G.base[pass] = f;
    }
    G.row16.assign((size_t)W_, 16);
    G.row235.assign((size_t)W_, 235);
    G.sws = sws_alloc_context();
    return G.sws != nullptr;
}

// 黒地・白地に ass を通し、絵のある範囲だけを RGB にして、
// 「地が黒のときの絵 B」「地が白のときの絵 Wt」から 重ねる絵(掛け済み RGB = B、不透明度 α = 1 - (Wt-B)/255)を作る。
// ass フィルタの alpha 処理(alpha=1)は重なった層で不透明度がずれるので使わない
SubRender::Img SubRender::renderOne(long long nowMs) {
    Img r;
    if (!g_) return r;
    Graph& G = *g_;
    AVFrame* out[2] = {nullptr, nullptr};
    AVFrame* rgb[2] = {nullptr, nullptr};
    bool ok = true;
    const auto tA = std::chrono::steady_clock::now();
    for (int pass = 0; pass < 2 && ok; ++pass) {
        G.base[pass]->pts = nowMs;
        out[pass] = av_frame_alloc();
        ok = av_buffersrc_add_frame_flags(G.src, G.base[pass], AV_BUFFERSRC_FLAG_KEEP_REF) >= 0 &&
             av_buffersink_get_frame(G.sink, out[pass]) >= 0 && out[pass]->format == AV_PIX_FMT_YUV420P;
    }
    t_ass_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tA).count();
    if (ok) {
        // 絵のある範囲(輝度が地と違う所)。地のままの行は memcmp で飛ばす
        int x0 = W_, y0 = H_, x1 = -1, y1 = -1;
        for (int y = 0; y < H_; ++y) {
            const uint8_t* b = out[0]->data[0] + (size_t)y * out[0]->linesize[0];
            const uint8_t* w = out[1]->data[0] + (size_t)y * out[1]->linesize[0];
            if (!memcmp(b, G.row16.data(), (size_t)W_) && !memcmp(w, G.row235.data(), (size_t)W_)) continue;
            int a = 0, e = W_ - 1;
            while (a < W_ && b[a] == 16 && w[a] == 235) ++a;
            while (e > a && b[e] == 16 && w[e] == 235) --e;
            x0 = std::min(x0, a); x1 = std::max(x1, e);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        if (x1 >= x0) {
            // 色差の補間のぶん広げる。左上は偶数に揃える(4:2:0 の升目)
            x0 = std::max(0, x0 - 4) & ~1;
            y0 = std::max(0, y0 - 4) & ~1;
            x1 = std::min(W_ - 1, x1 + 4);
            y1 = std::min(H_ - 1, y1 + 4);
            const int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
            for (int pass = 0; pass < 2 && ok; ++pass) {
                AVFrame s = {};   // 切り取った範囲を指す枠(コピーしない)
                s.format = AV_PIX_FMT_YUV420P;
                s.width = cw;
                s.height = ch;
                s.colorspace = AVCOL_SPC_BT709;
                s.color_range = AVCOL_RANGE_MPEG;
                s.color_primaries = AVCOL_PRI_BT709;
                s.color_trc = AVCOL_TRC_BT709;
                s.data[0] = out[pass]->data[0] + (size_t)y0 * out[pass]->linesize[0] + x0;
                s.data[1] = out[pass]->data[1] + (size_t)(y0 / 2) * out[pass]->linesize[1] + x0 / 2;
                s.data[2] = out[pass]->data[2] + (size_t)(y0 / 2) * out[pass]->linesize[2] + x0 / 2;
                for (int i = 0; i < 3; ++i) s.linesize[i] = out[pass]->linesize[i];
                rgb[pass] = av_frame_alloc();
                rgb[pass]->format = AV_PIX_FMT_RGBA;
                rgb[pass]->width = cw;
                rgb[pass]->height = ch;
                rgb[pass]->color_range = AVCOL_RANGE_JPEG;
                rgb[pass]->colorspace = AVCOL_SPC_RGB;
                rgb[pass]->color_primaries = AVCOL_PRI_BT709;
                rgb[pass]->color_trc = AVCOL_TRC_BT709;
                ok = av_frame_get_buffer(rgb[pass], 32) >= 0 && sws_scale_frame(G.sws, rgb[pass], &s) >= 0;
            }
            if (ok) {
                QImage im(cw, ch, QImage::Format_RGBA8888_Premultiplied);
                for (int y = 0; y < ch; ++y) {
                    const uint8_t* b = rgb[0]->data[0] + (size_t)y * rgb[0]->linesize[0];
                    const uint8_t* w = rgb[1]->data[0] + (size_t)y * rgb[1]->linesize[0];
                    uint8_t* d = im.scanLine(y);
                    for (int x = 0; x < cw; ++x, b += 4, w += 4, d += 4) {
                        const int diff = ((int)w[0] - b[0] + (int)w[1] - b[1] + (int)w[2] - b[2] + 1) / 3;
                        const int a = 255 - std::min(255, std::max(0, diff));
                        d[0] = (uint8_t)std::min<int>(b[0], a);   // 掛け済みなので α を超えない
                        d[1] = (uint8_t)std::min<int>(b[1], a);
                        d[2] = (uint8_t)std::min<int>(b[2], a);
                        d[3] = (uint8_t)a;
                    }
                }
                r.img = im;
                r.x = x0;
                r.y = y0;
            }
        }
    }
    for (auto*& o : out) if (o) av_frame_free(&o);
    for (auto*& o : rgb) if (o) av_frame_free(&o);
    return r;
}

void SubRender::run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // 再生側を邪魔しない
#endif
    if (!failed_ && !initGraph()) failed_ = true;
    long long bytes = 0; double tot_ms = 0; int njobs = 0;
    for (;;) {
        std::pair<QString, long long> job;
        {
            std::unique_lock<std::mutex> l(mu_);
            cv_.wait(l, [&] { return quit_ || !q_.empty(); });
            if (quit_) break;
            job = q_.front();
            q_.pop_front();
            // 先回りの依頼のうち、再生が通り過ぎて要らなくなったものは描かない(遅れを引きずらない)
            if (job.second < lastReq_ - 1500 && !must_.contains(job.first)) { pending_.remove(job.first); continue; }
        }
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
        Img im = failed_ ? Img() : renderOne(job.second);
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq); tot_ms += (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart; ++njobs;
        if (qEnvironmentVariableIsSet("VN_DUMPSUB") && !im.img.isNull()) {   // 検証用: 描いた絵を PNG で残す(名前に時刻)
            QImage full(W_, H_, QImage::Format_RGBA8888_Premultiplied);
            full.fill(Qt::transparent);
            for (int y = 0; y < im.img.height(); ++y)
                memcpy(full.scanLine(y + im.y) + im.x * 4, im.img.constBits() + (size_t)y * im.img.bytesPerLine(), (size_t)im.img.width() * 4);
            full.save(QString("C:/Temp/vnative/s2/sub_%1.png").arg(job.second));
        }
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_.remove(job.first);
            must_.remove(job.first);
            cache_.insert(job.first, im);
            order_.push_back(job.first);
            bytes += im.img.sizeInBytes();
            while (order_.size() > 24 && (order_.size() > 96 || bytes > memBudget() * 15 / 100)) {
                bytes -= cache_.value(order_.front()).img.sizeInBytes();
                cache_.remove(order_.front());
                order_.pop_front();
            }
        }
        done_.notify_all();
    }
    if (qEnvironmentVariableIsSet("VN_SUBSTAT")) fprintf(stderr, "subrender jobs=%d total=%.0fms avg=%.2fms ass=%.0fms\n", njobs, tot_ms, njobs ? tot_ms / njobs : 0.0, t_ass_ms);
    if (g_) {
        for (auto*& f : g_->base) if (f) av_frame_free(&f);
        if (g_->sws) sws_free_context(&g_->sws);
        if (g_->g) avfilter_graph_free(&g_->g);
        delete g_;
        g_ = nullptr;
    }
}

}  // namespace vn
