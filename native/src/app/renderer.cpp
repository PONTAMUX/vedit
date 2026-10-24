#include "renderer.h"
#include <QElapsedTimer>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "core/pyrand.h"
#include "fxgen.h"
#include "media/imageload.h"
#include "media/membudget.h"

namespace vn {

static double nowMs() { static QElapsedTimer t = [] { QElapsedTimer q; q.start(); return q; }(); return t.nsecsElapsed() * 1e-6; }
// 使い終わったデコーダの片付け(スレッドの join で描画を止めない)
static std::atomic<int> g_reapers{0};
static void reap(std::unique_ptr<VideoStream> s) {
    if (!s) return;
    ++g_reapers;
    vn::Thread([p = std::move(s)]() mutable { vn::lowPowerThread(); p.reset(); --g_reapers; }).detach();
}

void reapStream(std::unique_ptr<VideoStream> s) { reap(std::move(s)); }

static bool trace() { static const bool t = qEnvironmentVariableIsSet("VN_TRACE"); return t; }

static const char* kVert = R"(#version 330 core
uniform vec4 uRect;
uniform vec2 uCanvas;
uniform vec4 uUV;
uniform mat2 uXA;       // 変形(拡大・回転): p' = uXA * (p - uXP) + uXP + uXD
uniform vec2 uXP;
uniform vec2 uXD;
out vec2 vUV;
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 p = uRect.xy + c * uRect.zw;
    p = uXA * (p - uXP) + uXP + uXD;
    vec2 ndc = p / uCanvas * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vUV = mix(uUV.xy, uUV.zw, c);
}
)";

static const char* kFrag = R"(#version 330 core
in vec2 vUV;
out vec4 o;
uniform sampler2D t0;
uniform sampler2D t1;
uniform int uMode;      // 0=RGBA(掛け済み) 1=YUV(NV12/P010) 2=RGBA(掛け前) 3=黒
uniform float uAlpha;
uniform float uYoff, uYscale, uCoff, uCscale, uKr, uKb;
uniform int uNOps;      // 色を変えるエフェクト(順に掛ける)
uniform vec4 uOpA[8];   // x=種類 y=amount
uniform vec3 uOpB[8];   // tint の black
uniform vec3 uOpC[8];   // tint の white
uniform vec3 uSolid;    // mode 3 の色
uniform vec3 uSolid2;   // mode 4 の先端の色
uniform vec4 uTr;       // mode 4/5 の値
uniform vec2 uCv;       // 画面の大きさ(px)
float luma(vec3 v) { return dot(v, vec3(0.2126, 0.7152, 0.0722)); }
void main() {
    vec4 c;
    if (uMode == 4) {   // wipe-diag
        vec2 p = floor(vUV * uCv);
        float X = p.x + (uCv.y - p.y) * uTr.x;
        float dd = uTr.w > 0.5 ? uTr.z - X : X - uTr.z;
        float al = clamp(dd + 0.5, 0.0, 1.0);
        float red = clamp(uTr.y - dd + 0.5, 0.0, 1.0) * al;
        c = vec4((uSolid2 * red + uSolid * (1.0 - red)) * al, al);
    } else if (uMode == 5) {   // iris
        vec2 p = floor(vUV * uCv);
        float a = clamp((length(p - uTr.xy) - uTr.z) / uTr.w + 0.5, 0.0, 1.0);
        c = vec4(uSolid * a, a);
    } else if (uMode == 3) {
        c = vec4(uSolid, 1.0);
    } else if (uMode == 1) {
        float Y = texture(t0, vUV).r;
        vec2 cc = texture(t1, vUV).rg;
        float y = (Y - uYoff) * uYscale;
        float cb = (cc.r - uCoff) * uCscale;
        float cr = (cc.g - uCoff) * uCscale;
        float kg = 1.0 - uKr - uKb;
        float r = y + 2.0 * (1.0 - uKr) * cr;
        float b = y + 2.0 * (1.0 - uKb) * cb;
        float g = (y - uKr * r - uKb * b) / kg;
        c = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
    } else {
        c = texture(t0, vUV);
        if (uMode == 2) c.rgb *= c.a;
    }
    if (uNOps > 0 && c.a > 0.0) {
        vec3 x = c.rgb / c.a;
        for (int i = 0; i < uNOps; ++i) {
            int k = int(uOpA[i].x + 0.5);
            float a = uOpA[i].y;
            if (k == 1) x = x + a;
            else if (k == 2) x = (x - 0.5) * (1.0 + a) + 0.5;
            else if (k == 3) { float l = luma(x); x = l + (x - l) * (1.0 + a); }
            else if (k == 4) x = x + ((uOpB[i] + (uOpC[i] - uOpB[i]) * luma(x)) - x) * a;
            else if (k == 5) x = x + (luma(x) - x) * a;
            else if (k == 6) x = x + ((1.0 - x) - x) * a;
            x = clamp(x, 0.0, 1.0);
        }
        c.rgb = x * c.a;
    }
    o = c * uAlpha;
}
)";

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

bool Renderer::load(const QString& projectPath, QString* err) {
    if (!Project::load(projectPath, &proj_)) { if (err) *err = proj_.error; return false; }
    if (!tl_.init(proj_, [](const QString& p) { return probeMedia(p); })) { if (err) *err = tl_.error; return false; }
    srcPath_ = proj_.resolve(proj_.source);
    openedDecode_ = decodePath();
    return true;
}

static unsigned compile(QOpenGLFunctions_3_3_Core* f, unsigned type, const char* src) {
    unsigned s = f->glCreateShader(type);
    f->glShaderSource(s, 1, &src, nullptr);
    f->glCompileShader(s);
    int ok = 0;
    f->glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        f->glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
        qWarning("shader: %s", buf);
    }
    return s;
}

bool Renderer::initGL() {
    initializeOpenGLFunctions();
    unsigned vs = compile(this, GL_VERTEX_SHADER, kVert), fs = compile(this, GL_FRAGMENT_SHADER, kFrag);
    prog_ = glCreateProgram();
    glAttachShader(prog_, vs);
    glAttachShader(prog_, fs);
    glLinkProgram(prog_);
    int ok = 0;
    glGetProgramiv(prog_, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) return false;
    glUseProgram(prog_);
    auto U = [&](const char* n) { return glGetUniformLocation(prog_, n); };
    uRect_ = U("uRect"); uCanvas_ = U("uCanvas"); uUV_ = U("uUV"); uMode_ = U("uMode"); uAlpha_ = U("uAlpha");
    uYoff_ = U("uYoff"); uYscale_ = U("uYscale"); uCoff_ = U("uCoff"); uCscale_ = U("uCscale"); uKr_ = U("uKr"); uKb_ = U("uKb");
    uXA_ = U("uXA"); uXP_ = U("uXP"); uXD_ = U("uXD"); uNOps_ = U("uNOps");
    uOpA_ = U("uOpA[0]"); uOpB_ = U("uOpB[0]"); uOpC_ = U("uOpC[0]");
    uSolid_ = U("uSolid"); uSolid2_ = U("uSolid2"); uTr_ = U("uTr"); uCv_ = U("uCv");
    clearXform();
    glUniform1i(U("t0"), 0);
    glUniform1i(U("t1"), 1);
    glGenVertexArrays(1, &vao_);
    fbo_ = std::make_unique<QOpenGLFramebufferObject>(tl_.W, tl_.H);
    glOk_ = fbo_->isValid();
    if (glOk_ && !postInit()) qWarning("post pipeline unavailable");
    initV1Fx();
    startProber();
    subPlan_.init(proj_, tl_);
    if (!subPlan_.empty()) subs_ = std::make_unique<SubRender>(tl_.W, tl_.H, subPlan_.ass());   // ここから裏で libass を立ち上げる
    initTelops();
    return glOk_;
}

// 素材の情報(大きさ・長さ)を先に調べておく別スレッド。描画スレッドでの初回 probe(avformat_open_input 等で 10〜25ms)をなくす
void Renderer::startProber() {
    for (auto it = probers_.begin(); it != probers_.end();) {   // 前に止めた分のうち終わったものを片付ける
        if (it->done->load()) { it->th.join(); it = probers_.erase(it); } else ++it;
    }
    auto quit = std::make_shared<std::atomic<bool>>(false), done = std::make_shared<std::atomic<bool>>(false);
    std::vector<std::pair<double, QString>> items;
    for (const auto& L : tl_.layers) if (!L.path.isEmpty()) items.push_back({L.a, L.path});
    std::sort(items.begin(), items.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    // 動画の差し込み(重なりうる分、最大 4 本)の NVDEC を再生の前に作っておく。プロジェクトを読み直した時は作り済みなので 1 回だけ
    std::vector<std::string> warm;
    if (!warmStarted_.exchange(true)) {
        std::vector<const Timeline::LayerSpec*> vl;
        for (const auto& L : tl_.layers) if (!L.path.isEmpty() && !L.isImage && L.gen.isEmpty() && !L.sub) vl.push_back(&L);
        std::sort(vl.begin(), vl.end(), [](auto* x, auto* y) { return x->a < y->a; });
        for (auto* L : vl) {
            const std::string p = L->path.toUtf8().toStdString();
            if (std::find(warm.begin(), warm.end(), p) == warm.end() && warm.size() < 4) warm.push_back(p);
        }
    } else warmDone_ = true;
    vn::Thread th([this, items = std::move(items), warm = std::move(warm), quit, done] {
        vn::lowPowerThread();
        auto& proberQuit_ = *quit;
        if (!warm.empty() && !proberQuit_) { VideoStream::warmUp(warm); }
        warmDone_ = true;
        size_t i = 0;
        while (!proberQuit_ && i < items.size()) {
            const double ph = playhead_.load();
            // 再生位置の少し前〜12秒先まで(それより先は後で)。位置が戻った/飛んだ時は先頭から見直す
            size_t k = 0;
            while (k < items.size() && items[k].first < ph - 3.0) ++k;
            bool did = false;
            for (; k < items.size() && items[k].first < ph + 12.0 && !proberQuit_; ++k) {
                probeMedia(items[k].second);   // キャッシュ済みなら即戻る
                did = true;
            }
            (void)did; (void)i;
            for (int w = 0; w < 10 && !proberQuit_; ++w) QThread::msleep(20);
            if (ph > 1e8) break;
        }
        *done = true;
    });
    probers_.push_back({std::move(th), quit, done});
}

bool Renderer::waitDecodersWarm(int ms) {
    QElapsedTimer t;
    t.start();
    while (!warmDone_ && t.elapsed() < ms) QThread::msleep(5);
    return warmDone_;
}

void Renderer::stopProber(bool wait) {
    for (auto& p : probers_) *p.quit = true;
    if (!wait) return;
    for (auto& p : probers_) if (p.th.joinable()) p.th.join();
    probers_.clear();
}

void Renderer::releaseGL() {
    for (auto& [k, r] : subr_) { r->releaseGL(); }
    subr_.clear();
    stopProber();
    slots_.clear();
    for (auto& [k, o] : ovs_) freeVideo(o.gpu);
    ovs_.clear();
    while (g_reapers > 0) QThread::msleep(2);
    for (auto& o : tsrc_) { freeVideo(o.gpu); o.s.reset(); }
    freeVideo(base_);
    for (auto& [k, t] : imgs_) glDeleteTextures(1, &t.tex);
    imgs_.clear();
    for (auto& [k, t] : subTex_) if (t.tex) glDeleteTextures(1, &t.tex);
    subTex_.clear();
    subs_.reset();
    telopSubs_.reset();
    telop_.clear();
    postRelease();
    fbo_.reset();
    if (prog_) glDeleteProgram(prog_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    prog_ = vao_ = 0;
}

void Renderer::freeVideo(GpuVideo& g) {
    if (g.cu.resY || g.cu.resUV) cuda::unregisterTextures(g.cu);
    if (g.y) glDeleteTextures(1, &g.y);
    if (g.uv) glDeleteTextures(1, &g.uv);
    g = GpuVideo();
}

bool Renderer::uploadDev(GpuVideo& g, const VFrame& f) {
    if (!cuda::enabled() || !f.dev) return false;
    const bool p10 = f.kind == VFrame::P010;
    const bool re = !g.y || g.w != f.w || g.h != f.h || g.kind != f.kind;
    auto mk = [&](unsigned& t, int iw, int ih, unsigned ifmt, unsigned fmt, unsigned type) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, ifmt, iw, ih, 0, fmt, type, nullptr);
    };
    if (re) {
        freeVideo(g);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        mk(g.y, f.w, f.h, p10 ? GL_R16 : GL_R8, GL_RED, p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE);
        mk(g.uv, f.cw, f.ch, p10 ? GL_RG16 : GL_RG8, GL_RG, p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE);
        g.w = f.w; g.h = f.h; g.kind = f.kind;
    }
    if (!g.cu.resY && !cuda::registerTextures(g.cu, g.y, g.uv)) return false;
    if (!cuda::copyToTextures(g.cu, *f.dev, f.w, f.h, p10 ? 2 : 1)) return false;
    g.id = f.id; g.cs = f.cs; g.full = f.full; g.pts = f.pts;
    ++interop_;
    return true;
}

void Renderer::upload(GpuVideo& g, const VFrame& f) {
    if (f.dev) {
        if (uploadDev(g, f)) return;
        // GPU 経由が使えない: 以後は CPU 経路に戻し、このコマは VRAM から読んで出す
        cuda::disable();
        if (auto h = cuda::toHost(f)) {
            if (g.cu.resY || g.cu.resUV) cuda::unregisterTextures(g.cu);
            upload(g, *h);
        }
        return;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const bool re = !g.y || g.w != f.w || g.h != f.h || g.kind != f.kind;
    auto mk = [&](unsigned& t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    if (re) {
        freeVideo(g);
        mk(g.y);
        if (f.kind != VFrame::RGBA) mk(g.uv);
        g.w = f.w; g.h = f.h; g.kind = f.kind;
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.y);
    if (f.kind == VFrame::RGBA) {
        if (re) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f.w, f.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.y.get());
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_RGBA, GL_UNSIGNED_BYTE, f.y.get());
    } else {
        const bool p10 = f.kind == VFrame::P010;
        if (re) glTexImage2D(GL_TEXTURE_2D, 0, p10 ? GL_R16 : GL_R8, f.w, f.h, 0, GL_RED, p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, f.y.get());
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_RED, p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, f.y.get());
        glBindTexture(GL_TEXTURE_2D, g.uv);
        if (re) glTexImage2D(GL_TEXTURE_2D, 0, p10 ? GL_RG16 : GL_RG8, f.cw, f.ch, 0, GL_RG, p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, f.uv.get());
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.cw, f.ch, GL_RG, p10 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, f.uv.get());
    }
    g.id = f.id; g.cs = f.cs; g.full = f.full; g.pts = f.pts;
}

void Renderer::drawQuad(int mode, double x, double y, double w, double h, double u0, double v0, double u1, double v1, double alpha,
                        unsigned t0, unsigned t1, const GpuVideo* gv) {
    glUniform4f(uRect_, (float)x, (float)y, (float)w, (float)h);
    glUniform4f(uUV_, (float)u0, (float)v0, (float)u1, (float)v1);
    glUniform1i(uMode_, mode);
    glUniform1f(uAlpha_, (float)alpha);
    if (mode <= 2) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t0);
    }
    if (mode == 1 && gv) {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, t1);
        glActiveTexture(GL_TEXTURE0);
        const bool p10 = gv->kind == VFrame::P010;
        float yoff, yscale, coff, cscale;
        if (!p10) {
            coff = 128.f / 255.f;
            if (gv->full) { yoff = 0; yscale = 1; cscale = 1; }
            else { yoff = 16.f / 255.f; yscale = 255.f / 219.f; cscale = 255.f / 224.f; }
        } else {
            coff = 32768.f / 65535.f;
            if (gv->full) { yoff = 0; yscale = 65535.f / 65472.f; cscale = 65535.f / 65472.f; }
            else { yoff = 4096.f / 65535.f; yscale = 65535.f / 56064.f; cscale = 65535.f / 57344.f; }
        }
        static const float kr[3] = {0.2126f, 0.299f, 0.2627f}, kb[3] = {0.0722f, 0.114f, 0.0593f};
        glUniform1f(uYoff_, yoff); glUniform1f(uYscale_, yscale); glUniform1f(uCoff_, coff); glUniform1f(uCscale_, cscale);
        glUniform1f(uKr_, kr[gv->cs]); glUniform1f(uKb_, kb[gv->cs]);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void Renderer::drawVideo(const GpuVideo& g, double x, double y, double w, double h, double u0, double v0, double u1, double v1,
                         double alpha) {
    if (g.kind == VFrame::RGBA) drawQuad(2, x, y, w, h, u0, v0, u1, v1, alpha, g.y);
    else drawQuad(1, x, y, w, h, u0, v0, u1, v1, alpha, g.y, g.uv, &g);
}

double Renderer::pieceStartSrc(int piece) const {
    const auto& pc = tl_.pieces[piece];
    return std::fabs(tl_.src.fps - tl_.fps) < 0.01 ? tl_.snapUp(pc.a, tl_.src.start, tl_.src.fps) : pc.a;
}

VideoStream* Renderer::baseFor(int piece, double startT, double ahead) {
    for (auto& s : slots_)
        if (s.piece == piece) return s.s.get();
    // 直前の本編区間と元素材で続いている(空白を挟んだだけ)なら、同じデコーダをそのまま使う
    int prev = piece - 1;
    while (prev >= 0 && tl_.pieces[prev].gap) --prev;
    if (prev >= 0 && std::fabs(tl_.pieces[prev].b - tl_.pieces[piece].a) < 1e-4)
        for (auto& s : slots_)
            if (s.piece == prev) { s.piece = piece; return s.s.get(); }
    Slot* use = nullptr;
    for (auto& s : slots_)
        if (s.piece < curPiece_ || s.piece < 0) { use = &s; break; }
    if (!use && slots_.size() < 3) {
        slots_.push_back(Slot{std::make_unique<VideoStream>(decodePath(), true, startT), piece});
        return slots_.back().s.get();
    }
    if (!use) {
        int far = -1;
        for (auto& s : slots_)
            if (std::abs(s.piece - curPiece_) > far) { far = std::abs(s.piece - curPiece_); use = &s; }
    }
    use->piece = piece;
    use->s->retarget(startT, ahead);
    return use->s.get();
}

void Renderer::drawBase(const Timeline::V1State& v, bool wait, double T) {
    if (v.black || v.piece < 0) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    VideoStream* s = baseFor(v.piece, v.srcT, 1.0);
    FramePtr f = s ? s->frameAt(v.srcT, wait, 1.0) : nullptr;
    if (f) {
        if (!wait && v.srcT - f->pts > 1.5 / tl_.src.fps) { ++stale_; lag_ = v.srcT - f->pts; if (trace()) fprintf(stderr, "STALE base piece=%d srcT=%.3f lag=%.3f\n", v.piece, v.srcT, lag_); }
        if (f->id != base_.id || !base_.y) upload(base_, *f);
    } else {
        ++stale_;
        if (trace()) fprintf(stderr, "STALE base-null piece=%d srcT=%.3f\n", v.piece, v.srcT);
        if (!base_.y) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            return;
        }
    }
    glDisable(GL_BLEND);
    if (tl_.shaking(T)) {
        double dx, dy;
        tl_.shakeAt(T, &dx, &dy);
        const double s = tl_.shakeZoom;
        const int W = tl_.W, H = tl_.H;
        const int cw = (int)(W / s / 2) * 2, ch = (int)(H / s / 2) * 2;
        int x = (int)((W - cw) / 2.0 - dx / s), y = (int)((H - ch) / 2.0 - dy / s);
        x = std::max(0, std::min(W - cw, x)) & ~1;
        y = std::max(0, std::min(H - ch, y)) & ~1;
        drawVideo(base_, 0, 0, W, H, (double)x / W, (double)y / H, (double)(x + cw) / W, (double)(y + ch) / H, 1.0);
        return;
    }
    drawVideo(base_, 0, 0, tl_.W, tl_.H, 0, 0, 1, 1, 1.0);
}

Renderer::OvStream* Renderer::overlayStream(int li, double T) {
    auto it = ovs_.find(li);
    if (it != ovs_.end()) return &it->second;
    const Timeline::LayerState st = tl_.state(li, std::max(T, tl_.layers[li].a));
    OvStream o;
    o.s = std::make_unique<VideoStream>(tl_.layers[li].path.toUtf8().toStdString(), true, std::max(0.0, st.srcT));
    return &(ovs_[li] = std::move(o));
}

unsigned Renderer::uploadImage(const QString& key, const QImage& im) {
    ImgTex t;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &t.tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.width(), im.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, im.constBits());
    t.w = im.width(); t.h = im.height(); t.used = ++useCounter_;
    {
        // 不透明(alpha>0)部分の外枠。拡大・回転の中心(anchor)の基準
        int x0 = t.w, y0 = t.h, x1 = -1, y1 = -1;
        for (int y = 0; y < t.h; ++y) {
            const uchar* p = im.constScanLine(y);
            int lo = -1, hi = -1;
            for (int x = 0; x < t.w; ++x) if (p[x * 4 + 3]) { lo = x; break; }
            if (lo < 0) continue;
            for (int x = t.w - 1; x >= lo; --x) if (p[x * 4 + 3]) { hi = x; break; }
            x0 = std::min(x0, lo); x1 = std::max(x1, hi);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        if (x1 >= 0) { t.bb[0] = (float)x0 / t.w; t.bb[1] = (float)y0 / t.h; t.bb[2] = (float)(x1 + 1) / t.w; t.bb[3] = (float)(y1 + 1) / t.h; }
    }
    imgs_[key] = t;
    return t.tex;
}

void Renderer::requestImage(const QString& key, const QString& path, int tw, int th) {
    {
        QMutexLocker l(&imgMu_);
        if (pending_.contains(key) || ready_.contains(key) || failed_.contains(key)) return;
        pending_.insert(key);
    }
    ++inflight_;
    QThreadPool::globalInstance()->start([this, key, path, tw, th] {
        QImage im = loadImageScaled(path, tw, th);
        {
            QMutexLocker l(&imgMu_);
            pending_.remove(key);
            if (im.isNull()) failed_.insert(key);
            else ready_.insert(key, im);
        }
        --inflight_;
    });
}

unsigned Renderer::imageTex(int li, bool wait, bool allowLoad) {
    const auto& L = tl_.layers[li];
    const QString key = QString("%1|%2|%3").arg(L.path).arg(L.g.texW).arg(L.g.texH);
    auto it = imgs_.find(key);
    if (it != imgs_.end()) { it->second.used = ++useCounter_; return it->second.tex; }
    QImage im;
    {
        QMutexLocker l(&imgMu_);
        auto r = ready_.find(key);
        if (r != ready_.end()) { im = r.value(); ready_.erase(r); }
        else if (failed_.contains(key)) return 0;
    }
    if (im.isNull() && wait) {
        // 静止画用: 読み込み中なら終わるまで待つ。無ければここで読む
        for (;;) {
            {
                QMutexLocker l(&imgMu_);
                if (!pending_.contains(key)) break;
            }
            QThread::msleep(2);
        }
        {
            QMutexLocker l(&imgMu_);
            auto r = ready_.find(key);
            if (r != ready_.end()) { im = r.value(); ready_.erase(r); }
        }
        if (im.isNull()) {
            im = loadImageScaled(L.path, L.g.texW, L.g.texH);
            if (im.isNull()) { QMutexLocker l(&imgMu_); failed_.insert(key); return 0; }
        }
    }
    if (im.isNull()) {
        if (allowLoad) requestImage(key, L.path, L.g.texW, L.g.texH);
        return 0;
    }
    return uploadImage(key, im);
}

bool Renderer::texBBox(unsigned tex, float bb[4]) const {
    for (const auto& kv : imgs_)
        if (kv.second.tex == tex) { for (int i = 0; i < 4; ++i) bb[i] = kv.second.bb[i]; return true; }
    return false;
}

void Renderer::restoreXform() {
    glUniformMatrix2fv(uXA_, 1, GL_FALSE, xfM_);
    glUniform2f(uXP_, xfP_[0], xfP_[1]);
    glUniform2f(uXD_, xfD_[0], xfD_[1]);
}

void Renderer::clearXform() {
    static const float I[4] = {1, 0, 0, 1};
    glUniformMatrix2fv(uXA_, 1, GL_FALSE, I);
    glUniform2f(uXP_, 0, 0);
    glUniform2f(uXD_, 0, 0);
    glUniform1i(uNOps_, 0);
    glUniform3f(uSolid_, 0, 0, 0);
}

// 差し込み1つ分の動き・色エフェクトを計算してシェーダに渡す(fxPath の要素だけ)。戻り値: 不透明度
double Renderer::setupFx(int li, double T, double bx, double by, double bw, double bh) {
    const auto& L = tl_.layers[li];
    auto it = efx_.find(li);
    if (it == efx_.end()) {
        fx::ElemFx e;
        e.init(proj_.overlays[L.ov].raw, L.a, L.b);
        it = efx_.emplace(li, std::move(e)).first;
    }
    fx::StateCtx c;
    c.W = tl_.W; c.H = tl_.H; c.fps = tl_.fps; c.k = tl_.H / 1080.0;
    c.rx = bx; c.ry = by; c.rw = bw; c.rh = bh;
    c.bbw = bw; c.bbh = bh;
    if (L.gen == "telop") { c.rx = 0; c.ry = 0; c.rw = tl_.W; c.rh = tl_.H; }   // Python の E.rect(ファイルなし)は画面全体
    fx::St st;
    std::vector<fx::PixFx> pix;
    fx::computeState(it->second, T - L.a, c, &st, &pix);
    // 拡大・回転の中心(anchor)は動かさない: p' = pa + R S (p - pa) + d
    const double a = st.rot * M_PI / 180.0, ca = std::cos(a), sa = std::sin(a);
    // 行列は行優先で [[ca*sx, -sa*sy],[sa*sx, ca*sy]]。glUniformMatrix2fv は列優先
    const float M[4] = {(float)(ca * st.sx), (float)(sa * st.sx), (float)(-sa * st.sy), (float)(ca * st.sy)};
    clipOn_ = st.clipped();
    if (clipOn_) {
        for (int i = 0; i < 4; ++i) clip_[i] = st.clip[i];
        clipFe_ = st.feather;
        clipBox_[0] = bx; clipBox_[1] = by; clipBox_[2] = bw; clipBox_[3] = bh;
    }
    for (int i = 0; i < 4; ++i) xfM_[i] = M[i];
    xfP_[0] = (float)(bx + st.ax * bw); xfP_[1] = (float)(by + st.ay * bh);
    xfD_[0] = (float)st.dx; xfD_[1] = (float)st.dy;
    restoreXform();
    float opA[8][4] = {}, opB[8][3] = {}, opC[8][3] = {};
    int n = 0;
    static const QHash<QString, int> kinds{{"brightness", 1}, {"contrast", 2}, {"saturation", 3}, {"tint", 4}, {"grayscale", 5}, {"invert", 6}};
    // 色の処理だけ(範囲指定なし)ならシェーダで直接。それ以外が混ざる/ぼかし/調整層は、キャンバスに描いて順にパスを掛ける
    bool needPost = st.blur > 0.3 || L.gen == "adjust" || clipOn_;
    for (const auto& p : pix)
        if (!kinds.contains(p.name) || (p.P.contains("rect") && fx::truthy(p.P["rect"])) || (int)pix.size() > 8) needPost = true;
    postOn_ = needPost;
    postFx_.clear();
    postBlur_ = needPost ? st.blur : 0;
    if (needPost) {
        postFx_ = pix;
        pix.clear();
    }
    for (const auto& p : pix) {
        if (n >= 8) continue;
        opA[n][0] = (float)kinds[p.name];
        opA[n][1] = (float)fx::num(p.P["amount"]);
        if (p.name == "tint") {
            const int d0[3] = {0, 0, 0}, d1[3] = {255, 255, 255};
            int cb[3], cw[3];
            fx::parseColor(p.P["black"], cb, d0);
            fx::parseColor(p.P["white"], cw, d1);
            for (int i = 0; i < 3; ++i) { opB[n][i] = cb[i] / 255.f; opC[n][i] = cw[i] / 255.f; }
        }
        ++n;
    }
    glUniform1i(uNOps_, n);
    if (n) {
        glUniform4fv(uOpA_, 8, &opA[0][0]);
        glUniform3fv(uOpB_, 8, &opB[0][0]);
        glUniform3fv(uOpC_, 8, &opC[0][0]);
    }
    return std::max(0.0, std::min(1.0, st.op));
}

// 拡大・回転の中心(anchor)の基準になる外枠: 画像は不透明な部分、それ以外は置いた枠
void Renderer::fxBox(int li, bool wait, const Timeline::LayerState& st, double* bxp, double* byp, double* bwp, double* bhp) {
    const auto& L = tl_.layers[li];
    const auto& g = L.g;
    if (L.gen == "comp" && compBBLi_ == li) {
        *bxp = compBB_[0]; *byp = compBB_[1]; *bwp = compBB_[2] - compBB_[0]; *bhp = compBB_[3] - compBB_[1];
        return;
    }
    if (L.gen == "comp") {   // anchor は comp の頭(tau=0)で出ている中の層の不透明な範囲(Python: content(0.0) の bbox)
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (int k : proj_.overlays[L.ov].subs) {
            const int sl = layerOfOv(k);
            if (sl < 0 || !tl_.prepare(sl)) continue;
            if (!(tl_.layers[sl].a <= L.a + 1e-9 && L.a < tl_.layers[sl].b)) continue;
            if (tl_.layers[sl].gen == "adjust") continue;
            double bx, by, bw, bh;
            fxBox(sl, wait, tl_.state(sl, L.a), &bx, &by, &bw, &bh);
            x0 = std::min(x0, bx); y0 = std::min(y0, by); x1 = std::max(x1, bx + bw); y1 = std::max(y1, by + bh);
        }
        if (x1 > x0 && y1 > y0) { *bxp = x0; *byp = y0; *bwp = x1 - x0; *bhp = y1 - y0; return; }
    }
    if (L.gen == "telop" && telopBox(li, wait, bxp, byp, bwp, bhp)) return;
    // fx の経路では全画面の黒い下地は敷かない(Python は透明の上に置く)
    double bw = g.cw, bh = g.ch;
    double bx = st.x + g.cx, by = st.y + g.cy;
    if (L.isImage && L.gen.isEmpty()) {
        float bb[4];
        const unsigned tex = imageTex(li, wait);
        if (tex && texBBox(tex, bb) && g.u1 > g.u0 && g.v1 > g.v0) {
            const double x0 = std::max<double>(bb[0], g.u0), x1 = std::min<double>(bb[2], g.u1);
            const double y0 = std::max<double>(bb[1], g.v0), y1 = std::min<double>(bb[3], g.v1);
            if (x1 > x0 && y1 > y0) {
                const double sx = g.cw / (g.u1 - g.u0), sy = g.ch / (g.v1 - g.v0);
                bx += (x0 - g.u0) * sx; by += (y0 - g.v0) * sy;
                bw = (x1 - x0) * sx; bh = (y1 - y0) * sy;
                // Python は先に拡縮した絵の「アルファが 0 でない画素」の外枠を取る。補間のすそ(約 1.25 px)ぶん元の外枠より広い(拡縮しない時は同じ)
                const double pad = std::fabs(sx / std::max(1, g.texW) - 1.0) > 1e-3 || std::fabs(sy / std::max(1, g.texH) - 1.0) > 1e-3 ? 1.25 : 0.0;
                bx -= pad; by -= pad; bw += 2 * pad; bh += 2 * pad;
            }
        }
    }
    *bxp = bx; *byp = by; *bwp = bw; *bhp = bh;
}

// キャンバス 0 に中身を描いて(マスク → 動き → fx の順)、結果が入ったキャンバス番号を返す。setupFx は済んでいること
int Renderer::canvasPass(int li, double T, bool wait, const Timeline::LayerState& st, const QJsonArray& masks, double maskOp,
                         const std::vector<fx::PixFx>& list, double blurSigma) {
    bindCanvas(0, true);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!masks.isEmpty() || clipOn_) {
        // Python と同じ順: 動かす前の絵に wipe の切り抜き・マスクを掛け、そのあとで動かす
        clearXform();
        drawContent(li, T, 1.0, wait, st);
        int m = 0;
        if (clipOn_) m = clipPass(0);
        if (!masks.isEmpty()) m = chainFx(m, {}, 0, masks, maskOp);
        bindCanvas(0, true);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        restoreXform();
        glActiveTexture(GL_TEXTURE0);
        drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, 1.0, cv_[m].tex);
        clearXform();
        return chainFx(0, list, blurSigma, QJsonArray(), -1);
    }
    drawContent(li, T, 1.0, wait, st);
    clearXform();
    return chainFx(0, list, blurSigma, QJsonArray(), -1);
}

int Renderer::clipPass(int cur) {
    PostArgs A;
    const double L = clipBox_[0], T = clipBox_[1], w = clipBox_[2], h = clipBox_[3];
    A.a[0] = clip_[0] > -9 ? (float)(L + clip_[0] * w) : -1e6f;
    A.a[1] = clip_[1] > -9 ? (float)(T + clip_[1] * h) : -1e6f;
    A.a[2] = clip_[2] < 10 ? (float)(L + clip_[2] * w) : 1e6f;
    A.a[3] = clip_[3] < 10 ? (float)(T + clip_[3] * h) : 1e6f;
    A.b[0] = (float)clipFe_;
    const int out = freeIdx({cur});
    postRun(19, out, cur, -1, A);
    return out;
}

// canvasPass + 要素自身の動きから作るモーションブラー(motion-blur の length=0)。戻り値はキャンバス番号(reserve したものは返さない)
int Renderer::layerCanvas(int li, double T, bool wait, const Timeline::LayerState& st, const QJsonArray& masks, double maskOp) {
    const auto& L = tl_.layers[li];
    const double k = tl_.H / 1080.0;
    int mb = -1;
    for (int i = 0; i < (int)postFx_.size(); ++i)
        if (postFx_[i].name == "motion-blur" && fx::num(postFx_[i].P["length"]) * k <= 0.5) { mb = i; break; }
    if (mb < 0) return canvasPass(li, T, wait, st, masks, maskOp, postFx_, postBlur_);
    const QJsonObject mp = postFx_[mb].P;
    const std::vector<fx::PixFx> post(postFx_.begin() + mb + 1, postFx_.end());
    const int n = std::max(2, (int)fx::num(mp["samples"], 8));
    const double sh = fx::num(mp["shutter"], 0.5) / tl_.fps;
    const int acc = reserveCanvas(true);
    if (acc < 0) return canvasPass(li, T, wait, st, masks, maskOp, postFx_, postBlur_);
    bindCanvas(acc, true);
    for (int i = 0; i < n; ++i) {
        const double Ti = i ? std::max(L.a, T - sh * i / (n - 1)) : T;
        Timeline::LayerState si = i ? tl_.state(li, Ti) : st;
        if (i) si.srcT = st.srcT;   // 動画の中身は今のコマのまま(位置・拡大だけ過去の値)
        double bx, by, bw, bh;
        fxBox(li, wait, si, &bx, &by, &bw, &bh);
        setupFx(li, Ti, bx, by, bw, bh);
        std::vector<fx::PixFx> pre;
        for (const auto& f : postFx_) { if (f.name == "motion-blur") break; pre.push_back(f); }
        const int r = canvasPass(li, T, wait, si, masks, maskOp, pre, postBlur_);
        bindCanvas(acc, false);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        clearXform();
        glActiveTexture(GL_TEXTURE0);
        drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, 1.0 / n, cv_[r].tex);
    }
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    const int res = chainFx(acc, post, 0, QJsonArray(), -1);
    // 足し算用のキャンバスは返す前に通常のキャンバスへ写す(呼び出し側は 0..7 だけを知っていればよい)
    int out = res;
    if (res >= kGenCv) {
        out = freeIdx({});
        bindCanvas(out, true);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        clearXform();
        glActiveTexture(GL_TEXTURE0);
        drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, 1.0, cv_[res].tex);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    releaseCanvas(acc);
    return out;
}

// ---- matte / parent / blend
void Renderer::buildCompose() {
    comp_.clear();
    compBuilt_ = true;
    const auto& ovs = proj_.overlays;
    QHash<QString, int> ids;
    for (int i = 0; i < (int)ovs.size(); ++i) {
        const QString id = ovs[i].raw["id"].toString();
        if (!id.isEmpty()) ids[id] = i;
    }
    static const QStringList modes{"normal", "add", "screen", "multiply", "overlay", "darken", "lighten", "difference"};
    for (int i = 0; i < (int)ovs.size(); ++i) {
        const QJsonObject& r = ovs[i].raw;
        Compose c;
        const QJsonValue m = r["matte"];
        if (m.isString() && !m.toString().isEmpty()) {
            c.hasMatte = true;
            c.matteMode = m.toString();
            c.matteOv = i + 1 < (int)ovs.size() ? i + 1 : -1;   // モードだけの時は次の要素をマットに使う
        } else if (m.isObject()) {
            c.hasMatte = true;
            c.matteMode = m.toObject()["mode"].toString("alpha");
            c.matteOv = ids.value(m.toObject()["src"].toString(), -1);
        }
        QSet<int> seen;
        int p = ids.value(r["parent"].toString(), -1);
        while (p >= 0 && !seen.contains(p)) {
            seen.insert(p);
            c.parents.push_back(p);
            p = ids.value(ovs[p].raw["parent"].toString(), -1);
        }
        const int bi = modes.indexOf(r["blend"].toString("normal"));
        c.blend = bi > 0 ? bi : 0;
        if (c.hasMatte || !c.parents.empty() || c.blend) comp_[i] = c;
    }
    std::vector<int> hide;
    for (const auto& kv : comp_)
        if (kv.second.hasMatte && kv.second.matteOv >= 0) hide.push_back(kv.second.matteOv);   // マットに使う要素は自分では出ない
    for (int h : hide) comp_[h].hidden = true;
}

int Renderer::layerOfOv(int ov) const {
    for (int i = 0; i < (int)tl_.layers.size(); ++i)
        if (tl_.layers[i].ov == ov) return i;
    return -1;
}

// プリコンポーズ: 中の層(comp の頭からの時刻)を下から重ねて 1 枚の絵にする。comp 自体の動き・fx・マスクはこの絵に掛かる
int Renderer::renderComp(int li, double T, bool wait) {
    const auto& L = tl_.layers[li];
    const int acc = reserveCanvas(false);
    if (acc < 0) return -1;
    bindCanvas(acc, true);
    for (int k : proj_.overlays[L.ov].subs) {
        const int sl = layerOfOv(k);
        if (sl < 0 || !(tl_.layers[sl].a <= T && T < tl_.layers[sl].b)) continue;
        const auto h = comp_.find(k);
        if (h != comp_.end() && h->second.hidden) continue;
        double al = 1;
        const int r = layerToCanvas(sl, T, wait, &al, true);
        if (r < 0) continue;
        bindCanvas(acc, false);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        clearXform();
        glActiveTexture(GL_TEXTURE0);
        drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, al, cv_[r].tex);
    }
    // 拡大・回転の中心(anchor)は Python では「そのコマの中身の外枠」(comp は動く中身なので毎コマ)。動かす指定がある時だけ読み戻して求める
    compBBLi_ = -1;
    const QJsonObject& raw = proj_.overlays[L.ov].raw;
    bool need = false;
    for (const char* k : {"keys", "in_anim", "out_anim", "fx", "_preset"}) {
        const QJsonValue v = raw[k];
        if (v.isObject() ? !v.toObject().isEmpty() : v.isArray() ? !v.toArray().isEmpty() : v.isString() ? (!v.toString().isEmpty() && v.toString() != "none") : false) need = true;
    }
    if (need) {
        bindCanvas(acc, false);
        std::vector<unsigned char> buf((size_t)tl_.W * tl_.H * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, tl_.W, tl_.H, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        int x0 = tl_.W, x1 = -1, y0 = tl_.H, y1 = -1;
        for (int y = 0; y < tl_.H; ++y) {
            const unsigned char* row = buf.data() + (size_t)y * tl_.W * 4;
            for (int x = 0; x < tl_.W; ++x)
                if (row[x * 4 + 3]) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        }
        if (x1 >= x0) {
            // 画像の行 0 は画面の下(キャンバスは上下逆に持っている)
            compBB_[0] = x0; compBB_[2] = x1 + 1;
            compBB_[1] = tl_.H - (y1 + 1); compBB_[3] = tl_.H - y0;
            compBBLi_ = li;
        }
    }
    return acc;
}

int Renderer::stashCanvas(int r) {
    const int s = reserveCanvas(false);
    if (s < 0) return -1;
    bindCanvas(s, true);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    clearXform();
    glActiveTexture(GL_TEXTURE0);
    drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, 1.0, cv_[r].tex);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return s;
}

// 親の動き(拡大・回転・移動。透明度は受け継がない)。p' = A p + D(画面 px)。親が何も動かさないなら false
bool Renderer::parentAffine(int li, double T, float A[4], float D[2]) {
    const auto it = comp_.find(tl_.layers[li].ov);
    if (it == comp_.end() || it->second.parents.empty()) return false;
    double M[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int pov : it->second.parents) {
        const int pl = layerOfOv(pov);
        if (pl < 0 || !tl_.prepare(pl)) continue;
        const auto& PL = tl_.layers[pl];
        const double pt = std::min(std::max(0.0, T - PL.a), std::max(0.0, PL.b - PL.a - 1e-6));
        auto e = efx_.find(pl);
        if (e == efx_.end()) {
            fx::ElemFx ef;
            ef.init(proj_.overlays[PL.ov].raw, PL.a, PL.b);
            e = efx_.emplace(pl, std::move(ef)).first;
        }
        const Timeline::LayerState ps = tl_.state(pl, PL.a + pt);
        double bx, by, bw, bh;
        fxBox(pl, true, ps, &bx, &by, &bw, &bh);
        fx::StateCtx c;
        c.W = tl_.W; c.H = tl_.H; c.fps = tl_.fps; c.k = tl_.H / 1080.0;
        c.rx = bx; c.ry = by; c.rw = bw; c.rh = bh; c.bbw = bw; c.bbh = bh;
        fx::St st;
        std::vector<fx::PixFx> pix;
        fx::computeState(e->second, pt, c, &st, &pix);
        const double r = st.rot * M_PI / 180.0, ca = std::cos(r), sa = std::sin(r);
        const double a00 = ca * st.sx, a01 = -sa * st.sy, a10 = sa * st.sx, a11 = ca * st.sy;
        const double px = bx + st.ax * bw, py = by + st.ay * bh;
        const double t0 = px + st.dx - (a00 * px + a01 * py), t1 = py + st.dy - (a10 * px + a11 * py);
        // M = [A t] @ M
        double N[3][3] = {};
        const double P[3][3] = {{a00, a01, t0}, {a10, a11, t1}, {0, 0, 1}};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) N[i][j] += P[i][k] * M[k][j];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) M[i][j] = N[i][j];
    }
    const bool ident = std::fabs(M[0][0] - 1) < 1e-9 && std::fabs(M[1][1] - 1) < 1e-9 && std::fabs(M[0][1]) < 1e-9 && std::fabs(M[1][0]) < 1e-9 &&
                       std::fabs(M[0][2]) < 1e-9 && std::fabs(M[1][2]) < 1e-9;
    if (ident) return false;
    A[0] = (float)M[0][0]; A[1] = (float)M[1][0]; A[2] = (float)M[0][1]; A[3] = (float)M[1][1];   // 列優先
    D[0] = (float)M[0][2]; D[1] = (float)M[1][2];
    return true;
}

// r に matte → parent → blend を順に掛ける。戻り値は結果のキャンバス(0..7)。-1 なら何も出さない
int Renderer::applyCompose(int li, int r, double T, bool wait) {
    const auto it = comp_.find(tl_.layers[li].ov);
    if (it == comp_.end()) return r;
    const Compose cm = it->second;
    int cur = stashCanvas(r);
    if (cur < 0) return r;
    bool transparent = false;
    if (cm.hasMatte) {
        const int ml = cm.matteOv >= 0 ? layerOfOv(cm.matteOv) : -1;
        int src = -1;
        if (ml >= 0 && tl_.layers[ml].a <= T && T < tl_.layers[ml].b) {
            double ma = 1;
            const int mr = layerToCanvas(ml, T, wait, &ma, true);
            if (mr >= 0) src = stashCanvas(mr);
        }
        const bool inv = cm.matteMode.endsWith("-inv");
        if (src < 0) {   // マットが無い/出ていない時間 → 絵は消える(Python は mv=0。-inv なら全部残る)
            if (!inv) transparent = true;
        } else {
            const int dst = reserveCanvas(false);
            if (dst < 0) { releaseCanvas(src); } else {
                PostArgs A;
                A.a[0] = cm.matteMode.startsWith("luma") ? 1.f : 0.f;
                A.a[1] = inv ? 1.f : 0.f;
                postRun(20, dst, cur, src, A);
                releaseCanvas(cur);
                cur = dst;
            }
            releaseCanvas(src);
        }
    }
    float PA[4], PD[2];
    if (!transparent && parentAffine(li, T, PA, PD)) {
        const int dst = reserveCanvas(false);
        if (dst >= 0) {
            bindCanvas(dst, true);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            clearXform();
            glUniformMatrix2fv(uXA_, 1, GL_FALSE, PA);
            glUniform2f(uXP_, 0, 0);
            glUniform2f(uXD_, PD[0], PD[1]);
            glActiveTexture(GL_TEXTURE0);
            drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, 1.0, cv_[cur].tex);
            clearXform();
            releaseCanvas(cur);
            cur = dst;
        }
    }
    if (!transparent && cm.blend) {
        const int dst = reserveCanvas(false);
        const int below = reserveCanvas(false);
        if (dst >= 0 && below >= 0) {
            copyToCanvas(below);   // いまの画面(下の絵)
            PostArgs A;
            A.a[0] = (float)cm.blend;
            postRun(21, dst, cur, below, A);
            releaseCanvas(cur);
            cur = dst;
        } else if (dst >= 0) releaseCanvas(dst);
        if (below >= 0) releaseCanvas(below);
    }
    int out = -1;
    if (!transparent) {   // 結果は呼び出し側が知っている 0..7 のキャンバスへ
        out = freeIdx({});
        bindCanvas(out, true);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        clearXform();
        glActiveTexture(GL_TEXTURE0);
        drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, 1.0, cv_[cur].tex);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    releaseCanvas(cur);
    return out;
}

// 層 1 つぶんの絵をキャンバスにして返す(画面にはまだ載せない)。alphaOut=載せる時の不透明度
int Renderer::layerToCanvas(int li, double T, bool wait, double* alphaOut, bool force) {
    if (!tl_.prepare(li)) return -1;
    const auto& L = tl_.layers[li];
    const Timeline::LayerState st = tl_.state(li, T);
    if (st.alpha <= 0) return -1;
    const bool fxp = proj_.overlays[L.ov].fxPath;
    double alpha = st.alpha;
    postOn_ = false;
    QJsonArray masks;
    double maskOp = -1;
    if (!compBuilt_) buildCompose();
    const bool needCompose = fxp && comp_.count(L.ov);
    if (L.sub && L.gen == "adjust") return -1;   // comp の中の調整層は下の絵が無いので何もしない(Python は下=無し)
    struct CompGuard { Renderer* r; int c; ~CompGuard() { if (c >= 0) { r->releaseCanvas(c); r->compCv_ = -1; } } } cg{this, -1};
    if (L.gen == "comp") { cg.c = renderComp(li, T, wait); compCv_ = cg.c; if (cg.c < 0) return -1; }
    if (fxp) {
        const OverlaySpec& os = proj_.overlays[L.ov];
        const QJsonValue mv = os.raw["mask"];
        fx::Dyn dy;
        dy.dur = L.b - L.a; dy.fps = tl_.fps;
        auto addMask = [&](const QJsonValue& v, int i) {
            if (!v.isObject()) return;
            masks.append(fx::hasDyn(v) ? fx::pval(v, T - L.a, dy, fx::seedOf("mask", i)) : v);
        };
        if (mv.isArray()) { int i = 0; for (const auto& v : mv.toArray()) addMask(v, i++); }
        else addMask(mv, 0);
        if (!os.raw["mask_opacity"].isNull() && !os.raw["mask_opacity"].isUndefined()) maskOp = fx::num(os.raw["mask_opacity"]);
        double bx, by, bw, bh;
        fxBox(li, wait, st, &bx, &by, &bw, &bh);
        alpha *= setupFx(li, T, bx, by, bw, bh);
        if (alpha <= 0) { clearXform(); return -1; }
        if (!masks.isEmpty() || needCompose || L.gen == "comp") postOn_ = true;
    }
    struct Guard { Renderer* r; bool on; ~Guard() { if (on) r->clearXform(); } } guard{this, fxp};
    *alphaOut = alpha;
    if (postOn_ && cv_[0].fbo) {
        int r;
        if (L.gen == "adjust") {
            copyToCanvas(0);
            clearXform();
            r = chainFx(0, postFx_, postBlur_, masks, maskOp);
        } else {
            r = layerCanvas(li, T, wait, st, masks, maskOp);
        }
        if (needCompose) {
            clearXform();
            r = applyCompose(li, r, T, wait);
        }
        return r;
    }
    if (L.gen == "adjust") return -1;
    if (force) {   // 素の層をキャンバスに(マットの元などで必要な時)
        bindCanvas(0, true);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        drawContent(li, T, alpha, wait, st);
        return 0;
    }
    drawContent(li, T, alpha, wait, st);
    return -2;
}

void Renderer::drawLayer(int li, double T, bool wait) {
    if (!compBuilt_) buildCompose();
    if (li >= 0 && li < (int)tl_.layers.size()) {
        const auto h = comp_.find(tl_.layers[li].ov);
        if (h != comp_.end() && h->second.hidden) return;
    }
    double alpha = 1;
    const int r = layerToCanvas(li, T, wait, &alpha, false);
    if (r >= 0) compositeCanvas(r, alpha);
}

void Renderer::drawContent(int li, double T, double alpha, bool wait, const Timeline::LayerState& st) {
    const auto& L = tl_.layers[li];
    const auto& g = L.g;
    if (L.gen == "comp") {
        if (compCv_ >= 0) { glActiveTexture(GL_TEXTURE0); drawQuad(0, st.x + g.cx, st.y + g.cy, g.cw, g.ch, 0, 1, 1, 0, alpha, cv_[compCv_].tex); }
        return;
    }
    if (L.gen == "subproj") {
        auto it = subr_.find(li);
        if (it != subr_.end() && it->second && it->second->frameTex()) { glActiveTexture(GL_TEXTURE0); drawQuad(0, st.x, st.y, tl_.W, tl_.H, 0, 1, 1, 0, alpha, it->second->frameTex()); }
        return;
    }
    if (!L.gen.isEmpty()) { drawGen(li, T, alpha, wait); return; }
    if (L.isImage) {
        unsigned tex = imageTex(li, wait);
        if (!tex) { if (!wait) { ++stale_; if (trace()) fprintf(stderr, "STALE image li=%d T=%.3f\n", li, T); } return; }
        if (g.pad && !proj_.overlays[L.ov].fxPath) drawQuad(3, st.x, st.y, g.lw, g.lh, 0, 0, 1, 1, alpha);
        drawQuad(0, st.x + g.cx, st.y + g.cy, g.cw, g.ch, g.u0, g.v0, g.u1, g.v1, alpha, tex);
        return;
    }
    if (st.srcT > L.m.duration + 0.5) return;   // 動画の終わり + 0.5秒(最後のコマ止め)を過ぎたら出さない
    OvStream* o = overlayStream(li, T);
    FramePtr f = o->s->frameAt(st.srcT, wait, 1.0);
    if (f) {
        const double ovFrame = 1.0 / std::max(1.0, L.m.fps);
        // 動画の終わり(最後のコマ止めの 0.5 秒)では古い絵が正しい
        if (!wait && st.srcT - f->pts > 1.5 * ovFrame && st.srcT < L.m.duration - 1.5 * ovFrame) { ++stale_; if (trace()) fprintf(stderr, "STALE ov li=%d T=%.3f lag=%.3f fps=%.2f hw=%d a=%.3f path=%s\n", li, T, st.srcT - f->pts, L.m.fps, (int)o->s->usingHw(), L.a, qPrintable(L.path.right(40))); }
        if (f->id != o->gpu.id || !o->gpu.y) upload(o->gpu, *f);
    } else {
        if (!wait) { ++stale_; if (trace()) fprintf(stderr, "STALE ov-null li=%d T=%.3f\n", li, T); }
        if (!o->gpu.y) return;
    }
    if (g.pad && !proj_.overlays[L.ov].fxPath) drawQuad(3, st.x, st.y, g.lw, g.lh, 0, 0, 1, 1, alpha);
    drawVideo(o->gpu, st.x + g.cx, st.y + g.cy, g.cw, g.ch, g.u0, g.v0, g.u1, g.v1, alpha);
}

// ---- 生成要素(speedlines / gloom は CPU で描いて貼る。flash は単色)
QJsonObject Renderer::genParams(int li, double tau, bool* animated) const {
    const auto& L = tl_.layers[li];
    const OverlaySpec& s = proj_.overlays[L.ov];
    if (L.gen == "telop") {
        if (animated) { const auto t = telop_.find(li); *animated = t != telop_.end() && t->second->animator; }
        return QJsonObject();
    }
    QJsonObject P = fx::params("gen", L.gen, s.raw["params"].toObject());
    const bool dyn = fx::hasDyn(P);
    if (dyn) {
        fx::Dyn d;
        d.dur = L.b - L.a;
        d.fps = tl_.fps;
        P = fx::pval(P, tau, d, fx::seedOf(L.gen)).toObject();
    }
    if (animated) {
        *animated = dyn || (L.gen == "speedlines" && fx::truthy(P["animate"])) || (L.gen == "gloom" && fx::num(P["grow"]) > 0);
    }
    return P;
}

QImage Renderer::makeGenImage(int li, double tau) const {
    const auto& L = tl_.layers[li];
    if (L.gen == "telop") return const_cast<Renderer*>(this)->telopImage(li, tau);
    const QJsonObject P = genParams(li, tau, nullptr);
    if (L.gen == "speedlines") return fxgen::speedlines(tl_.W, tl_.H, tl_.fps, P, tau);
    return fxgen::gloom(tl_.W, tl_.H, P, tau);
}

void Renderer::requestGen(const QString& key, int li, double tau) {
    int ep;
    {
        QMutexLocker l(&imgMu_);
        if (pending_.contains(key) || ready_.contains(key) || failed_.contains(key)) return;
        pending_.insert(key);
        genPending_.insert(key);
        ep = genEpoch_;
    }
    ++inflight_;
    QThreadPool::globalInstance()->start([this, key, li, tau, ep] {
        {
            QMutexLocker l(&imgMu_);
            if (ep != genEpoch_) { --inflight_; return; }   // 待っている間に reload された(pending_ は reload が消した)
            ++genRunning_;
        }
        QImage im = makeGenImage(li, tau);
        {
            QMutexLocker l(&imgMu_);
            --genRunning_;
            if (ep == genEpoch_) {
                pending_.remove(key);
                genPending_.remove(key);
                if (im.isNull()) failed_.insert(key);
                else ready_.insert(key, im);
            }
        }
        --inflight_;
    });
}

unsigned Renderer::genTex(const QString& key, int li, double tau, bool wait) {
    auto it = imgs_.find(key);
    if (it != imgs_.end()) { it->second.used = ++useCounter_; return it->second.tex; }
    QImage im;
    {
        QMutexLocker l(&imgMu_);
        auto r = ready_.find(key);
        if (r != ready_.end()) { im = r.value(); ready_.erase(r); }
    }
    if (im.isNull() && wait) {
        for (;;) {
            {
                QMutexLocker l(&imgMu_);
                if (!pending_.contains(key)) break;
            }
            QThread::msleep(2);
        }
        {
            QMutexLocker l(&imgMu_);
            auto r = ready_.find(key);
            if (r != ready_.end()) { im = r.value(); ready_.erase(r); }
        }
        if (im.isNull()) im = makeGenImage(li, tau);
    }
    if (im.isNull()) { requestGen(key, li, tau); return 0; }
    return uploadImage(key, im);
}

void Renderer::drawSolid(const int rgb[3], double x, double y, double w, double h, double alpha) {
    glUniform3f(uSolid_, rgb[0] / 255.f, rgb[1] / 255.f, rgb[2] / 255.f);
    drawQuad(3, x, y, w, h, 0, 0, 1, 1, alpha);
    glUniform3f(uSolid_, 0, 0, 0);
}

void Renderer::drawGen(int li, double T, double alpha, bool wait) {
    const auto& L = tl_.layers[li];
    const double tau = T - L.a;
    bool anim = false;
    const QJsonObject P = genParams(li, tau, &anim);
    if (L.gen == "flash") {
        const double d = fx::num(P["dur"]), f = fx::num(P["fade"]);
        const double a = fx::num(P["opacity"]) * (tau < d - f ? 1.0 : (f > 0 ? std::max(0.0, (d - tau) / f) : 0.0));
        const double q = std::nearbyint(a * 255) / 255.0;
        if (q <= 0) return;
        const int white[3] = {255, 255, 255};
        int col[3];
        fx::parseColor(P["color"], col, white);
        drawSolid(col, 0, 0, tl_.W, tl_.H, alpha * q);
        return;
    }
    const long long fr = (long long)std::nearbyint(tau * tl_.fps);
    long long fk = -1;
    if (L.gen == "telop") {
        fk = telopFk(li, tau);
    } else if (anim) {
        fk = fr;
        if (L.gen == "speedlines" && fx::truthy(P["animate"])) fk = (long long)std::floor((double)fr / std::max(1LL, (long long)fx::num(P["every"])));
    }
    const QString key = QString("gen|%1|%2").arg(li).arg(fk);
    unsigned tex = genTex(key, li, tau, wait);
    if (!tex) {
        auto lg = lastGen_.find(li);
        if (anim && lg != lastGen_.end()) { auto it = imgs_.find(lg->second); if (it != imgs_.end()) tex = it->second.tex; }
        if (!tex) { if (!wait) ++stale_; return; }
    } else {
        lastGen_[li] = key;
    }
    drawQuad(2, 0, 0, tl_.W, tl_.H, 0, 0, 1, 1, alpha, tex);
}

void Renderer::prefetchGen(int li, double T) {
    const auto& L = tl_.layers[li];
    if (L.gen == "flash" || L.gen == "adjust") return;
    if (L.gen == "telop") {
        for (int i = 0; i < 4; ++i) {
            const double t = std::max(T, L.a) + i / tl_.fps;
            if (t > L.b) break;
            const double tau = t - L.a;
            const QString key = QString("gen|%1|%2").arg(li).arg(telopFk(li, tau));
            if (!imgs_.count(key)) requestGen(key, li, tau);
        }
        return;
    }
    bool anim = false;
    genParams(li, std::max(0.0, T - L.a), &anim);
    if (!anim) {
        const QString key = QString("gen|%1|-1").arg(li);
        if (!imgs_.count(key)) requestGen(key, li, 0.0);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        const double t = std::max(T, L.a) + i / tl_.fps;
        if (t > L.b) break;
        const double tau = t - L.a;
        bool a2 = false;
        const QJsonObject P = genParams(li, tau, &a2);
        long long fk = (long long)std::nearbyint(tau * tl_.fps);
        if (L.gen == "speedlines" && fx::truthy(P["animate"])) fk = (long long)std::floor((double)fk / std::max(1LL, (long long)fx::num(P["every"])));
        const QString key = QString("gen|%1|%2").arg(li).arg(fk);
        if (!imgs_.count(key)) requestGen(key, li, tau);
    }
}

void Renderer::prefetch(double T, double horizon) {
    // 本編: 次の区間(カットの先)を別のデコーダで先に開いて頭を読んでおく
    int n = 0;
    for (int p = curPiece_ + 1; p < (int)tl_.pieces.size() && tl_.pieces[p].o0 < T + 1.5 && n < 2; ++p) {
        if (tl_.pieces[p].gap) continue;
        { const double a = nowMs(); baseFor(p, pieceStartSrc(p), 0.4); if (trace() && nowMs() - a > 3) fprintf(stderr, "  PF baseFor %.1f\n", nowMs() - a); }
        ++n;
    }
    // 差し込み: 出る少し前に画像を読み・動画のデコーダを開く
    for (int li : tl_.layersIn(T, T + horizon)) {
        const double a0 = nowMs();
        if (!tl_.prepare(li)) continue;
        const double a1 = nowMs();
        const auto& L = tl_.layers[li];
        if (L.gen == "comp" || L.gen == "subproj") {
        } else if (!L.gen.isEmpty()) {
            prefetchGen(li, T);
        } else if (L.isImage) {
            imageTex(li, false);
        } else {
            OvStream* o = overlayStream(li, T);
            const Timeline::LayerState st = tl_.state(li, std::max(T, L.a));
            o->s->retarget(st.srcT, T < L.a ? 0.5 : 1.0);
        }
        const double a2 = nowMs();
        if (trace() && a2 - a0 > 3) fprintf(stderr, "  PF layer li=%d prepare=%.1f rest=%.1f img=%d\n", li, a1 - a0, a2 - a1, (int)L.isImage);
    }
    const double b0 = nowMs();
    for (auto it = ovs_.begin(); it != ovs_.end();) {
        const auto& L = tl_.layers[it->first];
        if (L.b < T - 1.0 || L.a > T + 6.0) { freeVideo(it->second.gpu); reap(std::move(it->second.s)); it = ovs_.erase(it); }
        else ++it;
    }
    prefetchTrans(T);
    trimImages();
    if (trace() && nowMs() - b0 > 3) fprintf(stderr, "  PF free+trim %.1f\n", nowMs() - b0);
}

void Renderer::drawSubs(double T, bool wait) {
    if (!subs_) return;
    const long long now = SubPlan::nowMs(T);
    const QString key = subPlan_.keyAt(now);
    if (key.isEmpty()) return;
    auto it = subTex_.find(key);
    if (it == subTex_.end()) {
        SubRender::Img im;
        if (!subs_->peek(key, &im)) {
            if (!wait) {   // 間に合わなかった: 古い絵は出さない(別の状態の字幕になるので)
                ++stale_;
                if (trace()) fprintf(stderr, "STALE subs T=%.3f key=%s\n", T, qPrintable(key));
                subs_->request(key, now, true);
                return;
            }
            subs_->get(key, now, &im);
        }
        SubTex t;
        if (!im.img.isNull()) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glGenTextures(1, &t.tex);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, im.img.width(), im.img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, im.img.constBits());
            t.w = im.img.width(); t.h = im.img.height(); t.x = im.x; t.y = im.y;
        }
        if (subTex_.size() > 24) {
            auto old = subTex_.begin();
            for (auto i = subTex_.begin(); i != subTex_.end(); ++i) if (i->second.used < old->second.used) old = i;
            if (old->second.tex) glDeleteTextures(1, &old->second.tex);
            subTex_.erase(old);
        }
        it = subTex_.emplace(key, t).first;
    }
    it->second.used = ++useCounter_;
    if (it->second.tex) drawQuad(0, it->second.x, it->second.y, it->second.w, it->second.h, 0, 0, 1, 1, 1.0, it->second.tex);
}

void Renderer::prefetchSubs(double T) {
    if (!subs_) return;
    const int n = (int)std::ceil(0.5 * tl_.fps);
    QString last;
    for (int i = 0; i <= n; ++i) {
        const long long now = SubPlan::nowMs(T + i / tl_.fps);
        const QString key = subPlan_.keyAt(now);
        if (key.isEmpty() || key == last || subTex_.count(key)) continue;
        last = key;
        subs_->request(key, now);
    }
}

// 差し込み画像のテクスチャは「使えるメモリ」の 6 割まで持つ。超えたら古く使ったものから 5 割まで捨てる
void Renderer::trimImages() {
    auto texBytes = [](const ImgTex& t) { return (long long)t.w * t.h * 4; };
    long long bytes = 0;
    for (auto& [k, t] : imgs_) bytes += texBytes(t);
    const long long cap = memBudget() * 6 / 10, keep = memBudget() / 2;
    if (bytes <= cap) return;
    std::vector<std::pair<uint64_t, QString>> v;
    for (auto& [k, t] : imgs_) v.push_back({t.used, k});
    std::sort(v.begin(), v.end());
    for (size_t i = 0; bytes > keep && imgs_.size() > 8 && i < v.size(); ++i) {
        auto it = imgs_.find(v[i].second);
        bytes -= texBytes(it->second);
        glDeleteTextures(1, &it->second.tex);
        imgs_.erase(it);
    }
}


// ---- トランジション(fx.py の _t_*)。継ぎ目からの秒 tau = T - o。覆う絵のものは画面の一番上、前後の映像を使うものは別扱い
static double coverU(const QJsonObject& P, double tau, double fps) {
    const double dur = fx::num(P["dur"]);
    const QString e = P["ease"].toString();
    if (tau < 0) return fx::ease(e, (tau + dur + 1 / fps) / dur);
    return 1 - fx::ease(e, tau / dur);
}

void Renderer::drawWipe(const QJsonObject& P, double tau) {
    const int W = tl_.W, H = tl_.H;
    const double k0 = std::tan(fx::num(P["angle"]) * M_PI / 180.0);
    const double R = fx::num(P["edge_w"]) * (H / 1080.0);
    const double L = W + H * k0 + R;
    const double dur = fx::num(P["dur"]);
    const QString e = P["ease"].toString();
    const bool cover = tau < 0;
    double edge;
    if (cover) edge = fx::ease(e, (tau + dur + 1 / tl_.fps) / dur) * (L + 2) - 1;
    else edge = fx::ease(e, tau / dur) * (L + 2) - R - 1;
    const int d0[3] = {200, 20, 27}, d1[3] = {14, 14, 18};
    int ec[3], bc[3];
    fx::parseColor(P["edge_color"], ec, d0);
    fx::parseColor(P["color"], bc, d1);
    glUniform3f(uSolid_, bc[0] / 255.f, bc[1] / 255.f, bc[2] / 255.f);
    glUniform3f(uSolid2_, ec[0] / 255.f, ec[1] / 255.f, ec[2] / 255.f);
    glUniform4f(uTr_, (float)k0, (float)R, (float)edge, cover ? 1.f : 0.f);
    glUniform2f(uCv_, (float)W, (float)H);
    drawQuad(4, 0, 0, W, H, 0, 0, 1, 1, 1.0);
    glUniform3f(uSolid_, 0, 0, 0);
}

void Renderer::drawIris(const QJsonObject& P, double tau) {
    const int W = tl_.W, H = tl_.H;
    const QJsonArray c = P["center"].toArray();
    const double cx = c.at(0).toDouble(0.5) * W, cy = c.at(1).toDouble(0.5) * H;
    const double mx = std::max(cx, W - 1 - cx), my = std::max(cy, H - 1 - cy);
    const double Rm = std::hypot(mx, my) + 2;
    const double r = (1 - coverU(P, tau, tl_.fps)) * Rm;
    const double s = std::max(0.5, fx::num(P["soft"]) * (H / 1080.0));
    const int d0[3] = {0, 0, 0};
    int col[3];
    fx::parseColor(P["color"], col, d0);
    glUniform3f(uSolid_, col[0] / 255.f, col[1] / 255.f, col[2] / 255.f);
    glUniform4f(uTr_, (float)cx, (float)cy, (float)r, (float)s);
    glUniform2f(uCv_, (float)W, (float)H);
    drawQuad(5, 0, 0, W, H, 0, 0, 1, 1, 1.0);
    glUniform3f(uSolid_, 0, 0, 0);
}

void Renderer::drawTrans(double T) {
    for (const auto& t : tl_.trans) {
        if (t.src || T < t.a - 1e-9 || T >= t.b - 1e-9) continue;
        // (Python 版は焼いた動画の重ね位置が ffmpeg の丸めで 0〜1 コマずれることがある。ここでは理想の時刻に合わせる)
        const double tau = T - t.o;
        if (t.name == "wipe-diag") drawWipe(t.P, tau);
        else if (t.name == "iris") drawIris(t.P, tau);
        else if (t.name == "dip-black" || t.name == "dip-white" || t.name == "flash") {
            double a = coverU(t.P, tau, tl_.fps);
            if (t.name == "flash") a *= fx::num(t.P["opacity"]);
            a = std::nearbyint(255 * std::min(1.0, std::max(0.0, a))) / 255.0;
            if (a <= 0) continue;
            const int d0[3] = {255, 255, 255};
            int col[3];
            fx::parseColor(t.P["color"], col, d0);
            drawSolid(col, 0, 0, tl_.W, tl_.H, a);
        }
    }
}

// ---- 前後の映像を使うトランジション(crossfade / push-* / zoom / whip)
Renderer::GpuVideo* Renderer::transFrame(int side, int idx, const Timeline::Trans& t, double tau, bool wait, bool* black) {
    *black = true;
    const bool has = side == 0 ? t.hasA : t.hasB;
    if (!has || proj_.hide.contains("v1")) return nullptr;
    const double j = (side == 0 ? t.jA : t.jB) + tau;
    const double st = std::max(0.0, std::fabs(tl_.src.fps - tl_.fps) < 0.01 ? tl_.snapUp(j, tl_.src.start, tl_.src.fps) : j);
    OvStream& ts = tsrc_[side];
    if (!ts.s || tsIdx_[side] != idx) {
        if (ts.s) reap(std::move(ts.s));
        freeVideo(ts.gpu);
        ts.gpu = GpuVideo();
        ts.s = std::make_unique<VideoStream>(decodePath(), true, st);
        tsIdx_[side] = idx;
    }
    FramePtr f = ts.s->frameAt(st, wait, 1.0);
    if (f) { if (f->id != ts.gpu.id || !ts.gpu.y) upload(ts.gpu, *f); }
    else if (!wait) ++stale_;
    if (!ts.gpu.y) return nullptr;
    *black = false;
    return &ts.gpu;
}

void Renderer::prefetchTrans(double T) {
    for (int i = 0; i < (int)tl_.trans.size(); ++i) {
        const auto& t = tl_.trans[i];
        if (!t.src) continue;
        if (T > t.b || T < t.a - 1.0) continue;
        for (int side = 0; side < 2; ++side) {
            const bool has = side == 0 ? t.hasA : t.hasB;
            if (!has) continue;
            const double j = (side == 0 ? t.jA : t.jB) + std::max(T, t.a) - t.o;
            const double st = std::max(0.0, std::fabs(tl_.src.fps - tl_.fps) < 0.01 ? tl_.snapUp(j, tl_.src.start, tl_.src.fps) : j);
            OvStream& ts = tsrc_[side];
            if (!ts.s || tsIdx_[side] != i) {
                if (ts.s) reap(std::move(ts.s));
                freeVideo(ts.gpu);
                ts.gpu = GpuVideo();
                ts.s = std::make_unique<VideoStream>(decodePath(), true, st);
                tsIdx_[side] = i;
            } else {
                ts.s->retarget(st, T < t.a ? 0.5 : 1.0);
            }
        }
        break;
    }
}

void Renderer::drawSrcTrans(double T, bool wait) {
    for (int idx = 0; idx < (int)tl_.trans.size(); ++idx) {
        const auto& t = tl_.trans[idx];
        if (!t.src || T < t.a - 1e-9 || T >= t.b - 1e-9) continue;
        const double tau = T - t.o, dur = fx::num(t.P["dur"]), fps = tl_.fps;
        const int W = tl_.W, H = tl_.H;
        const QString e = t.P["ease"].toString();
        const QString& n = t.name;
        // 1枚描く(映像が無ければ黒)。uv は枠の中の範囲
        auto put = [&](int side, double tt, double x, double y, double u0, double v0, double u1, double v1, double alpha) {
            bool blk;
            GpuVideo* g = transFrame(side, idx, t, tt, wait, &blk);
            if (!g) drawQuad(3, x, y, W, H, 0, 0, 1, 1, alpha);
            else drawVideo(*g, x, y, W, H, u0, v0, u1, v1, alpha);
        };
        if (n == "crossfade") {
            const double u = fx::ease(e, (tau + dur) / (2 * dur));
            put(0, tau, 0, 0, 0, 0, 1, 1, 1.0);
            put(1, tau, 0, 0, 0, 0, 1, 1, u);
        } else if (n.startsWith("push-")) {
            const double u = fx::ease(e, (tau + dur) / (2 * dur));
            const bool horiz = n == "push-left" || n == "push-right";
            const bool neg = n == "push-left" || n == "push-up";
            const double o = std::nearbyint(u * (horiz ? W : H));
            const double sgn = neg ? -1 : 1, span = horiz ? W : H;
            const double a = sgn * o, b = sgn * o - sgn * span;   // A の位置 / B の位置
            put(0, tau, horiz ? a : 0, horiz ? 0 : a, 0, 0, 1, 1, 1.0);
            put(1, tau, horiz ? b : 0, horiz ? 0 : b, 0, 0, 1, 1, 1.0);
        } else if (n == "zoom") {
            const double S = fx::num(t.P["scale"]), bl = fx::num(t.P["blur"]);
            double u, s;
            int side;
            if (tau < 0) { u = fx::ease("ease-in", std::min(1.0, (tau + dur + 1 / fps) / dur)); side = 0; }
            else { u = 1 - fx::ease("ease-out", std::min(1.0, tau / dur)); side = 1; }
            s = 1 + (S - 1) * u;
            auto win = [&](double ss, double alpha) {
                if (ss <= 1.0005) { put(side, tau, 0, 0, 0, 0, 1, 1, alpha); return; }
                const double h0 = (1 - 1 / ss) / 2, h1 = (1 + 1 / ss) / 2;
                put(side, tau, 0, 0, h0, h0, h1, h1, alpha);
            };
            if (bl > 0 && u > 0.05) {
                for (int j = 0; j < 4; ++j) win(s * (1 + bl * u * j / 3.0), 1.0 / (j + 1));
            } else {
                win(s, 1.0);
            }
        } else if (n == "glitch") {
            // 継ぎ目の前後の映像を横の帯ごとにずらして色ずれ(Python と同じ乱数)
            const int side = tau < 0 ? 0 : 1;
            const double g = fx::num(t.P["strength"]) * std::max(0.0, 1 - std::fabs(tau) / dur);
            PyRandom rnd((long long)fx::num(t.P["seed"]) * 1000 + (long long)std::nearbyint(tau * fps) + 500);
            PostArgs A;
            int y = 0, ng = 0;
            while (y < H && ng < 128) {
                const int hh = (int)rnd.randint((int)(H * 0.01) + 1, (int)(H * 0.12) + 2);
                if (rnd.random() < 0.55 * g + 0.05) {
                    A.g[ng][0] = (float)y; A.g[ng][1] = (float)(y + hh); A.g[ng][2] = (float)(int)(rnd.uniform(-1, 1) * g * W * 0.08);
                    ++ng;
                }
                y += hh;
            }
            A.ng = ng;
            A.a[0] = (float)std::nearbyint(g * 18 * (H / 1080.0));
            bindCanvas(0, true);
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            put(side, tau, 0, 0, 0, 0, 1, 1, 1.0);
            const int t1 = freeIdx({0});
            postRun(17, t1, 0, -1, A);
            compositeCanvas(t1, 1.0);
        } else if (n == "whip") {
            const double dist = fx::num(t.P["dist"]), bl = fx::num(t.P["blur"]);
            double u, off;
            int side;
            if (tau < 0) { u = fx::ease("ease-in", std::min(1.0, (tau + dur + 1 / fps) / dur)); side = 0; off = -u * dist * W; }
            else { u = 1 - fx::ease("ease-out", std::min(1.0, tau / dur)); side = 1; off = u * dist * W; }
            const double o = std::nearbyint(off);
            const double r = std::floor(u * bl * W);   // 横の箱ぼかしの半径(近似: 間引いた数枚の平均)
            const int taps = r < 1 ? 1 : (int)std::min(12.0, 2 * r + 1);
            for (int j = 0; j < taps; ++j) {
                const double d = taps == 1 ? 0.0 : -r + 2 * r * j / (taps - 1);
                double x = std::fmod(o + d, (double)W);
                if (x < 0) x += W;
                const double al = 1.0 / (j + 1);
                put(side, tau, x, 0, 0, 0, 1, 1, al);
                put(side, tau, x - W, 0, 0, 0, 1, 1, al);
            }
        }
        return;
    }
}

// 外部 project の comp: 中の project を別の描画器で comp の時刻(in + T - 頭)に描いておく(外側の FBO を使う前に)
void Renderer::prepSubProj(double T, bool wait) {
    for (int li = 0; li < (int)tl_.layers.size(); ++li) {
        const auto& L = tl_.layers[li];
        if (L.gen != "subproj" || T < L.a || T > L.b) continue;
        auto& r = subr_[li];
        if (!r) {
            auto nr = std::make_unique<Renderer>();
            QString err;
            if (!nr->load(proj_.resolve(proj_.overlays[L.ov].subProject), &err) || !nr->initGL()) { nr->releaseGL(); continue; }
            r = std::move(nr);
        }
        double st = proj_.overlays[L.ov].in + (T - L.a);
        const double tot = r->timeline().total;
        if (tot > 0) st = std::min(st, std::max(0.0, tot - 1.0 / std::max(1.0, r->timeline().fps)));   // 最後のコマで止める
        r->render(std::max(0.0, st), wait);
    }
}

void Renderer::render(double T, bool wait) {
    if (!glOk_) return;
    applyLive();   // 編集があれば、この描画から新しい内容で
    const double t0 = nowMs();
    playhead_ = T;
    if (!proj_.overlays.empty()) prepSubProj(T, wait);
    fbo_->bind();
    glViewport(0, 0, tl_.W, tl_.H);
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glUniform2f(uCanvas_, (float)tl_.W, (float)tl_.H);
    const Timeline::V1State v = tl_.v1At(T);
    curPiece_ = std::max(0, v.piece);
    drawBase(v, wait, T);
    const double t1 = nowMs();
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (!tl_.dipColor.isEmpty() && !v.black && v.piece >= 0) {   // cut_fx の暗転/白飛び(本編だけにかかる)
        const auto& pc = tl_.pieces[v.piece];
        const double L = pc.o1 - pc.o0, dur = std::min(tl_.dipDur, L / 3), t = T - pc.o0;
        double u = 1;
        if (v.piece > 0 && t < dur) u *= std::max(0.0, t / dur);
        if (v.piece < (int)tl_.pieces.size() - 1 && t > L - dur) u *= std::max(0.0, 1 - (t - (L - dur)) / dur);
        if (u < 1) {
            const int c[3] = {tl_.dipColor == "#FFFFFF" ? 255 : 0, tl_.dipColor == "#FFFFFF" ? 255 : 0, tl_.dipColor == "#FFFFFF" ? 255 : 0};
            drawSolid(c, 0, 0, tl_.W, tl_.H, 1 - u);
        }
    }
    // 統一 z 順(fx.py expand の flat.sort): V1 → v1_fx(z=1、先) → 前後の映像を使うトランジション(z=1、後) →
    // 差し込み(track 2..、同じ track はリスト順。テロップ fx は 900) → 上に出るトランジション(999) → 999 より上の差し込み → ASS の字幕
    drawV1Fx(T);
    drawSrcTrans(T, wait);
    bool transDone = false;
    for (int li : tl_.activeLayers(T)) {
        if (!transDone && proj_.overlays[tl_.layers[li].ov].track > kTopTrack) { drawTrans(T); transDone = true; }
        const double a = nowMs();
        drawLayer(li, T, wait);
        if (trace() && nowMs() - a > 6) fprintf(stderr, "SLOW layer li=%d %.1fms %s\n", li, nowMs() - a, qPrintable(tl_.layers[li].path.right(30)));
    }
    const double t2 = nowMs();
    if (!transDone) drawTrans(T);
    drawSubs(T, wait);   // テロップ・トピックは最上段(書き出しは重ねた後に ass フィルタ)
    glDisable(GL_BLEND);
    const double t3 = nowMs();
    if (!wait || prewarm_) { prefetch(T, wait ? 5.0 : 2.5); prefetchSubs(T); }   // 止まっている間(再生前の仕込み)は先まで開いておく: 再生中の NVDEC 作成を減らす
    fbo_->release();
    const double t4 = nowMs();
    if (trace() && t4 - t0 > 6) fprintf(stderr, "SLOW T=%.3f total=%.1f base=%.1f layers=%.1f subs=%.1f prefetch=%.1f\n", T, t4 - t0, t1 - t0, t2 - t1, t3 - t2, t4 - t3);
}

QImage Renderer::grab() { return fbo_->toImage(); }

void Renderer::blit(unsigned dst, int x, int y, int w, int h) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_->handle());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    glBlitFramebuffer(0, 0, tl_.W, tl_.H, x, y, x + w, y + h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, dst);
}

}  // namespace vn
