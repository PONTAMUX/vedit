// 画素エフェクト(fx.py の blur / mosaic / glow / shadow ... と mask・adjust・v1_fx の範囲指定)。
// 層を W×H の作業用画像(キャンバス)に描いてから、全画面のパスを順に掛けて本描画へ重ねる。
// キャンバスは RGBA8(掛け済み)。FBO は上下が反転して持たれているので、シェーダでは「上が 0 の画素番号」に直して読む
#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <random>
#include "core/pyrand.h"
#include "fxgen.h"
#include "renderer.h"

namespace vn {

static const char* kPV = R"(#version 330 core
out vec2 vUV;
void main() {
    vec2 c = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
    vUV = c;
}
)";

static const char* kPF = R"(#version 330 core
in vec2 vUV;
out vec4 o;
uniform sampler2D t0;
uniform sampler2D t1;
uniform int uMode;
uniform ivec2 uSz;
uniform vec4 uA;
uniform vec4 uB;
uniform vec3 uC;
uniform vec3 uD;
uniform int uNM;
uniform vec4 uMk[4];
uniform vec4 uMk2[4];
uniform int uNG;
uniform vec3 uG[128];
uniform vec2 uPoly[64];

float luma(vec3 v) { return dot(v, vec3(0.2126, 0.7152, 0.0722)); }
vec4 F0(ivec2 q) { q = clamp(q, ivec2(0), uSz - ivec2(1)); return texelFetch(t0, ivec2(q.x, uSz.y - 1 - q.y), 0); }
vec4 F1(ivec2 q) { q = clamp(q, ivec2(0), uSz - ivec2(1)); return texelFetch(t1, ivec2(q.x, uSz.y - 1 - q.y), 0); }
vec4 Sm(vec2 q) { return texture(t0, vec2((q.x + 0.5) / float(uSz.x), (float(uSz.y) - q.y - 0.5) / float(uSz.y))); }
int refl(int i, int n) {
    if (n <= 1) return 0;
    i = abs(i);
    int p = 2 * (n - 1);
    i = i % p;
    return i >= n ? p - i : i;
}
float erf_(float x) {
    float s = sign(x);
    x = abs(x);
    float t = 1.0 + x * (0.278393 + x * (0.230389 + x * (0.000972 + x * 0.078108)));
    t *= t; t *= t;
    return s * (1.0 - 1.0 / t);
}
float edge(float c, float a, float b, float sg) {   // 区間 [a,b) をガウスでぼかしたものの c での値
    if (sg < 0.01) return (c >= a && c < b) ? 1.0 : 0.0;
    float k = 1.0 / (sg * 1.41421356);
    return 0.5 * (erf_((c - a) * k) - erf_((c - b) * k));
}
float polyCov(int s, int n, vec2 c) {   // 多角形の内側の割合(4x4 のサンプル)
    float acc = 0.0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            vec2 p = c + (vec2(float(i), float(j)) + 0.5) / 4.0 - 0.5;
            bool in_ = false;
            for (int e = 0; e < n; ++e) {
                vec2 u = uPoly[s + e], v = uPoly[s + (e + 1) % n];
                if ((u.y > p.y) != (v.y > p.y) && p.x < (v.x - u.x) * (p.y - u.y) / (v.y - u.y) + u.x) in_ = !in_;
            }
            if (in_) acc += 1.0;
        }
    return acc / 16.0;
}
float shapeM(vec4 a, vec4 b, vec2 c) {
    float m = 1.0;
    float sg = b.y;
    if (a.x > 1.5) {
        m = polyCov(int(a.y), int(a.z), c);
    } else if (a.x < 0.5) {
        m = edge(c.x, a.y, a.w, sg) * edge(c.y, a.z, b.x, sg);
    } else if (a.x < 1.5) {
        vec2 r = vec2(a.w, b.x);
        float d = (length((c - a.yz) / r) - 1.0) * min(r.x, r.y);
        float s2 = sqrt(sg * sg + 0.25);
        m = 0.5 * (1.0 - erf_(d / (s2 * 1.41421356)));
    }
    return b.z > 0.5 ? 1.0 - m : m;
}
float maskAll(vec2 c) {
    float m = 1.0;
    for (int i = 0; i < uNM; ++i) {
        float f = shapeM(uMk[i], uMk2[i], c);
        int mode = int(uMk2[i].w + 0.5);
        if (i == 0) m = mode == 1 ? 1.0 - f : f;
        else if (mode == 1) m *= 1.0 - f;
        else if (mode == 2) m *= f;
        else m = max(m, f);
    }
    return m;
}
vec3 unp(vec4 c) { return c.a > 0.0 ? c.rgb / c.a : vec3(0.0); }
vec3 cop(vec3 x, int k, float a, vec3 B, vec3 C) {
    if (k == 1) x = x + a;
    else if (k == 2) x = (x - 0.5) * (1.0 + a) + 0.5;
    else if (k == 3) { float l = luma(x); x = l + (x - l) * (1.0 + a); }
    else if (k == 4) x = x + ((B + (C - B) * luma(x)) - x) * a;
    else if (k == 5) x = x + (luma(x) - x) * a;
    else if (k == 6) x = x + ((1.0 - x) - x) * a;
    return clamp(x, 0.0, 1.0);
}
int imod(int a, int n) { return a - n * int(floor(float(a) / float(n))); }
vec4 gl_band(int x, int r) {   // glitch: 帯のずらし(端は回り込む)
    float s = 0.0;
    for (int i = 0; i < uNG; ++i) if (float(r) >= uG[i].x && float(r) < uG[i].y) { s = uG[i].z; break; }
    int xx = imod(x - int(s), uSz.x);
    return F0(ivec2(xx, r));
}
void main() {
    ivec2 P = ivec2(int(gl_FragCoord.x), uSz.y - 1 - int(gl_FragCoord.y));
    vec2 Pf = vec2(P);
    int mode = uMode;
    vec4 c = F0(P);
    if (mode == 0) { o = c; }
    else if (mode == 1 || mode == 2) {   // ガウスぼかし(横/縦)
        float s = uA.x;
        int R = int(ceil(4.0 * s));
        R = min(R, 160);
        vec4 acc = vec4(0.0);
        float ws = 0.0;
        for (int i = -R; i <= R; ++i) {
            float w = exp(-0.5 * float(i * i) / (s * s));
            ivec2 q = mode == 1 ? ivec2(refl(P.x + i, uSz.x), P.y) : ivec2(P.x, refl(P.y + i, uSz.y));
            acc += F0(q) * w;
            ws += w;
        }
        o = acc / ws;
    } else if (mode == 3 || mode == 4) {   // モザイクのブロック平均(横/縦)
        float bw = uA.x;
        int n = int(uA.y + 0.5);
        int p = mode == 3 ? P.x : P.y;
        int len = mode == 3 ? uSz.x : uSz.y;
        int i = min(int(floor(float(p) / bw)), n - 1);
        int a0 = int(floor(float(i) * bw + 0.5));
        int a1 = i == n - 1 ? len : int(floor(float(i + 1) * bw + 0.5));
        a1 = max(a1, a0 + 1);
        vec4 acc = vec4(0.0);
        for (int q = a0; q < a1; ++q) acc += F0(mode == 3 ? ivec2(q, P.y) : ivec2(P.x, q));
        o = acc / float(a1 - a0);
    } else if (mode == 5) {   // rgb-split
        vec2 d = uA.xy;
        vec4 sr = Sm(Pf - d), sb = Sm(Pf + d);
        o = vec4(sr.r, c.g, sb.b, max(max(sr.a, sb.a), c.a));
    } else if (mode == 6) {   // vignette
        vec2 sz = vec2(uSz);
        float d = length(vec2((Pf.x - sz.x / 2.0) / (sz.x / 2.0), (Pf.y - sz.y / 2.0) / (sz.y / 2.0))) / 1.41421356;
        float v = pow(clamp((d - uA.y) / max(1e-3, uA.z), 0.0, 1.0), 1.5) * uA.x;
        vec3 x = unp(c);
        x = clamp(x + (uC - x) * v, 0.0, 1.0);
        o = vec4(x * c.a, c.a);
    } else if (mode == 7) {   // mirror
        bool ax = uA.x < 0.5;
        ivec2 q = ax ? ivec2(uSz.x - 1 - P.x, P.y) : ivec2(P.x, uSz.y - 1 - P.y);
        bool flip = true;
        if (uA.y > 0.5) flip = ax ? (P.x >= uSz.x / 2) : (P.y >= uSz.y / 2);
        o = flip ? F0(q) : c;
    } else if (mode == 8) {   // shadow の元(形をずらして色を付ける)
        float a = Sm(Pf - uA.xy).a * uA.z;
        o = vec4(uC * a, a);
    } else if (mode == 9) {   // t0 を t1 の上に重ねる
        vec4 b = F1(P);
        o = c + b * (1.0 - c.a);
    } else if (mode == 10) {   // outline の元(形を太らせる)
        int w = int(uA.x);
        float a = 0.0;
        for (int dy = -w; dy <= w; ++dy)
            for (int dx = -w; dx <= w; ++dx)
                if (dx * dx + dy * dy <= w * w) a = max(a, F0(P + ivec2(dx, dy)).a);
        a *= uA.y;
        o = vec4(uC * a, a);
    } else if (mode == 11) {   // glow の元
        vec3 rgb = unp(c);
        float w = clamp((luma(rgb) - uA.x) / max(1e-3, 1.0 - uA.x), 0.0, 1.0) * c.a;
        vec3 src = (uA.y > 0.5 ? uC : rgb) * w;
        o = vec4(src, w);
    } else if (mode == 12) {   // glow をのせる(c=元, t1=ぼかした光)
        vec4 g = F1(P) * uA.x;
        vec3 gl = g.rgb;
        float ga = clamp(g.a, 0.0, 1.0);
        float oa = clamp(c.a + ga * (1.0 - c.a), 0.0, 1.0);
        vec3 pm = 1.0 - (1.0 - c.rgb) * (1.0 - clamp(gl, 0.0, 1.0));
        o = vec4(oa > 1e-4 ? pm : vec3(0.0), oa);
    } else if (mode == 13) {   // chroma-key
        vec3 rgb = unp(c);
        float y = luma(rgb), yk = luma(uC);
        vec2 cb = vec2((rgb.b - y) / 1.8556, (rgb.r - y) / 1.5748);
        vec2 ck = vec2((uC.b - yk) / 1.8556, (uC.r - yk) / 1.5748);
        float d = length(cb - ck) / 0.75;
        float a2 = clamp((d - uA.x) / max(1e-3, uA.y), 0.0, 1.0);
        int ch = 0;
        if (uC.g > uC[ch]) ch = 1;
        if (uC.b > uC[ch]) ch = 2;
        int o1 = ch == 0 ? 1 : 0, o2 = ch == 2 ? 1 : 2;
        float lim = max(rgb[o1], rgb[o2]);
        rgb[ch] = rgb[ch] - max(0.0, rgb[ch] - lim) * uA.z;
        float na = c.a * a2;
        o = vec4(clamp(rgb, 0.0, 1.0) * na, na);
    } else if (mode == 14) {   // 色の処理
        vec3 x = cop(unp(c), int(uA.x + 0.5), uA.y, uC, uD);
        o = vec4(x * c.a, c.a);
    } else if (mode == 15) {   // 範囲の中だけ t1(処理後)に置き換える
        o = mix(c, F1(P), maskAll(Pf + 0.5));
    } else if (mode == 16) {   // マスクを掛ける
        o = c * (maskAll(Pf + 0.5) * uB.x);
    } else if (mode == 17) {   // glitch
        float d = uA.x;
        vec4 g = gl_band(P.x, P.y);
        int xr = imod(P.x - int(d), uSz.x);
        int xb = imod(P.x + int(d), uSz.x);
        o = vec4(gl_band(xr, P.y).r, g.g, gl_band(xb, P.y).b, g.a);
    } else if (mode == 18) {   // 決まった向きのモーションブラー
        int n = int(uA.z + 0.5);
        vec4 acc = vec4(0.0);
        for (int i = 0; i < n; ++i) {
            float d = float(i) / float(n - 1) - 0.5;
            acc += Sm(Pf - d * uA.xy);
        }
        o = acc / float(n);
    } else if (mode == 19) {   // wipe の切り抜き(外枠に対する四辺。境目は feather の幅で直線的に)
        vec2 c2 = Pf + 0.5;
        float fe = max(1e-3, uB.x);
        float mx = clamp((c2.x - uA.x) / fe, 0.0, 1.0) * clamp((uA.z - c2.x) / fe, 0.0, 1.0);
        float my = clamp((c2.y - uA.y) / fe, 0.0, 1.0) * clamp((uA.w - c2.y) / fe, 0.0, 1.0);
        o = c * (mx * my);
    } else if (mode == 20) {   // マット: t1(マットの絵)の透明度(luma なら明るさも)を掛ける。uA.x=luma、uA.y=反転
        vec4 m = F1(P);
        float mv = m.a;
        if (uA.x > 0.5) mv *= luma(unp(m));
        if (uA.y > 0.5) mv = 1.0 - mv;
        o = c * mv;
    } else if (mode == 21) {   // blend: 下の絵(t1。不透明)と混ぜた色にする(透明度は c のまま)。uA.x=方式
        vec3 b = F1(P).rgb;
        vec3 s = unp(c);
        int k = int(uA.x + 0.5);
        vec3 r = s;
        if (k == 1) r = clamp(b + s, 0.0, 1.0);
        else if (k == 2) r = 1.0 - (1.0 - b) * (1.0 - s);
        else if (k == 3) r = b * s;
        else if (k == 4) r = mix(1.0 - 2.0 * (1.0 - b) * (1.0 - s), 2.0 * b * s, vec3(lessThan(b, vec3(0.5))));
        else if (k == 5) r = min(b, s);
        else if (k == 6) r = max(b, s);
        else if (k == 7) r = abs(b - s);
        o = vec4(r * c.a, c.a);
    } else o = c;
}
)";

bool Renderer::postInit() {
    unsigned vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    const char* v = kPV;
    const char* f = kPF;
    glShaderSource(vs, 1, &v, nullptr);
    glShaderSource(fs, 1, &f, nullptr);
    glCompileShader(vs);
    glCompileShader(fs);
    int ok = 0;
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048];
        glGetShaderInfoLog(fs, sizeof buf, nullptr, buf);
        qWarning("post shader: %s", buf);
    }
    postProg_ = glCreateProgram();
    glAttachShader(postProg_, vs);
    glAttachShader(postProg_, fs);
    glLinkProgram(postProg_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glGetProgramiv(postProg_, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(postProg_); postProg_ = 0; return false; }
    glUseProgram(postProg_);
    for (const char* n : {"uMode", "uSz", "uA", "uB", "uC", "uD", "uNM", "uMk[0]", "uMk2[0]", "uNG", "uG[0]", "uPoly[0]"}) pu_[n] = glGetUniformLocation(postProg_, n);
    glUniform1i(glGetUniformLocation(postProg_, "t0"), 0);
    glUniform1i(glGetUniformLocation(postProg_, "t1"), 1);
    for (int i = 0; i < 4; ++i) {
        ensureCanvas(i);
        glBindFramebuffer(GL_FRAMEBUFFER, cv_[i].fbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) ok = 0;
    }
    glUseProgram(prog_);
    return ok != 0;
}

void Renderer::postRelease() {
    for (auto& c : cv_) {
        if (c.fbo) glDeleteFramebuffers(1, &c.fbo);
        if (c.tex) glDeleteTextures(1, &c.tex);
        c = Canvas();
    }
    if (postProg_) glDeleteProgram(postProg_);
    postProg_ = 0;
}

void Renderer::bindCanvas(int i, bool clear) {
    glBindFramebuffer(GL_FRAMEBUFFER, cv_[i].fbo);
    glViewport(0, 0, tl_.W, tl_.H);
    glUseProgram(prog_);
    if (clear) {
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
    }
}

void Renderer::copyToCanvas(int i) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_->handle());
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, cv_[i].fbo);
    glBlitFramebuffer(0, 0, tl_.W, tl_.H, 0, 0, tl_.W, tl_.H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, cv_[i].fbo);
}

void Renderer::compositeCanvas(int i, double alpha) {
    fbo_->bind();
    glViewport(0, 0, tl_.W, tl_.H);
    glUseProgram(prog_);
    glBindVertexArray(vao_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    clearXform();
    drawQuad(0, 0, 0, tl_.W, tl_.H, 0, 1, 1, 0, alpha, cv_[i].tex);
}

void Renderer::postRun(int mode, int dst, int a, int b, const PostArgs& A) {
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, cv_[dst].fbo);
    glViewport(0, 0, tl_.W, tl_.H);
    glUseProgram(postProg_);
    glUniform1i(pu_["uMode"], mode);
    glUniform2i(pu_["uSz"], tl_.W, tl_.H);
    glUniform4fv(pu_["uA"], 1, A.a);
    glUniform4fv(pu_["uB"], 1, A.b);
    glUniform3fv(pu_["uC"], 1, A.c);
    glUniform3fv(pu_["uD"], 1, A.d);
    glUniform1i(pu_["uNM"], A.nm);
    if (A.nm) {
        glUniform4fv(pu_["uMk[0]"], 4, &A.mk[0][0]);
        glUniform4fv(pu_["uMk2[0]"], 4, &A.mk2[0][0]);
    }
    if (A.np) glUniform2fv(pu_["uPoly[0]"], A.np, &A.poly[0][0]);
    glUniform1i(pu_["uNG"], A.ng);
    if (A.ng) glUniform3fv(pu_["uG[0]"], A.ng, &A.g[0][0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, b >= 0 ? cv_[b].tex : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, cv_[a].tex);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

int Renderer::freeIdx(std::initializer_list<int> used) {
    for (int i = 0; i < kGenCv; ++i) {
        bool u = false;
        for (int x : used) if (x == i) u = true;
        if (!u) { ensureCanvas(i); return i; }
    }
    return 0;
}

void Renderer::ensureCanvas(int i, bool f16) {
    Canvas& c = cv_[i];
    if (c.fbo) return;
    glGenTextures(1, &c.tex);
    glBindTexture(GL_TEXTURE_2D, c.tex);
    if (f16) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tl_.W, tl_.H, 0, GL_RGBA, GL_FLOAT, nullptr);
    else glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tl_.W, tl_.H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &c.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.tex, 0);
}

// 取っておくキャンバス(8..11)。releaseCanvas で返す。f16=足し算用の浮動小数点
int Renderer::reserveCanvas(bool f16) {
    for (int i = 11; i >= kGenCv; --i) {
        if (cvBusy_[i]) continue;
        if (cv_[i].fbo && f16 != (cvF16_ >> i & 1)) { glDeleteFramebuffers(1, &cv_[i].fbo); glDeleteTextures(1, &cv_[i].tex); cv_[i] = Canvas(); }
        ensureCanvas(i, f16);
        if (f16) cvF16_ |= 1u << i; else cvF16_ &= ~(1u << i);
        cvBusy_[i] = true;
        return i;
    }
    return -1;
}

void Renderer::releaseCanvas(int i) { if (i >= kGenCv && i < 12) cvBusy_[i] = false; }

int Renderer::blurIdx(int src, double sigma, int keep) {
    if (sigma < 0.3) return src;
    PostArgs A;
    A.a[0] = (float)sigma;
    const int t = freeIdx({src, keep});
    postRun(1, t, src, -1, A);
    const int u = freeIdx({src, keep, t});
    postRun(2, u, t, -1, A);
    return u;
}

// 範囲(shape_mask)の 1 つ分を uniform に入れる。slot=番号、mode=0 add / 1 subtract / 2 intersect
static void fillMask(PostArgs& A, int slot, const QJsonObject& s, double k, int mode) {
    QString shape = s["shape"].toString();
    if (shape.isEmpty()) shape = fx::truthy(s["rect"]) ? "rect" : "polygon";
    double x = 0, y = 0, w = 1920, h = 1080;
    if (s["rect"].isArray() && s["rect"].toArray().size() >= 4) {
        const QJsonArray r = s["rect"].toArray();
        x = r[0].toDouble(); y = r[1].toDouble(); w = r[2].toDouble(); h = r[3].toDouble();
    }
    x *= k; y *= k; w *= k; h *= k;
    const double fe = fx::num(s["feather"]) * k;
    const float sg = fe > 0.3 ? (float)(fe / 2) : 0.f;
    float* m = A.mk[slot];
    float* m2 = A.mk2[slot];
    if (shape == "rect") {
        m[0] = 0; m[1] = (float)std::nearbyint(x); m[2] = (float)std::nearbyint(y); m[3] = (float)std::nearbyint(x + w);
        m2[0] = (float)std::nearbyint(y + h);
    } else if (shape == "ellipse") {
        m[0] = 1; m[1] = (float)std::nearbyint(x + w / 2); m[2] = (float)std::nearbyint(y + h / 2);
        m[3] = (float)std::max(1.0, std::nearbyint(w / 2));
        m2[0] = (float)std::max(1.0, std::nearbyint(h / 2));
    } else {   // polygon: 頂点を A.poly に詰める(3 点未満は何も無い)
        const QJsonArray pa = s["points"].toArray();
        const int n = pa.size();
        if (n >= 3 && A.np + n <= 64) {
            m[0] = 2; m[1] = (float)A.np; m[2] = (float)n;
            for (int i = 0; i < n; ++i) {
                const QJsonArray q = pa[i].toArray();
                A.poly[A.np + i][0] = (float)(q[0].toDouble() * k);
                A.poly[A.np + i][1] = (float)(q[1].toDouble() * k);
            }
            A.np += n;
        } else {
            m[0] = 2; m[1] = 0; m[2] = 0;
        }
    }
    m2[1] = sg;
    m2[2] = fx::truthy(s["invert"]) ? 1.f : 0.f;
    m2[3] = (float)mode;
}

// エフェクトの region(または rect / points)→ 範囲の指定。無ければ false
static bool regionOf(const QJsonObject& P, QJsonObject* out) {
    if (P["region"].isObject()) { *out = P["region"].toObject(); return true; }
    if (fx::truthy(P["rect"]) || fx::truthy(P["points"])) {
        QJsonObject r;
        r["shape"] = P["shape"].isString() && !P["shape"].toString().isEmpty() ? P["shape"] : QJsonValue(fx::truthy(P["rect"]) ? "rect" : "polygon");
        r["rect"] = P["rect"];
        r["points"] = P["points"];
        r["feather"] = P["feather"];
        r["invert"] = P["invert"];
        *out = r;
        return true;
    }
    return false;
}

int Renderer::fxPass(const fx::PixFx& f, int cur) {
    const QString& n = f.name;
    const QJsonObject& P = f.P;
    const double k = tl_.H / 1080.0;
    PostArgs A;
    auto colorOf = [&](const QJsonValue& v, const int def[3], float* dst) {
        int c[3];
        fx::parseColor(v, c, def);
        for (int i = 0; i < 3; ++i) dst[i] = c[i] / 255.f;
    };
    static const int black[3] = {0, 0, 0}, white[3] = {255, 255, 255}, green[3] = {0, 255, 0};
    if (n == "blur" || n == "blur-region") return blurIdx(cur, fx::num(P["radius"]) * k);
    if (n == "mosaic") {
        const int b = std::max(2, (int)std::nearbyint(fx::num(P["size"]) * k));
        const int nx = std::max(1, tl_.W / b), ny = std::max(1, tl_.H / b);
        const int t = freeIdx({cur}), u = freeIdx({cur, t});
        A.a[0] = (float)tl_.W / nx; A.a[1] = (float)nx;
        postRun(3, t, cur, -1, A);
        A.a[0] = (float)tl_.H / ny; A.a[1] = (float)ny;
        postRun(4, u, t, -1, A);
        return u;
    }
    if (n == "rgb-split") {
        const double d = fx::num(P["amount"]) * k, a = fx::num(P["angle"]) * M_PI / 180;
        A.a[0] = (float)(d * std::cos(a)); A.a[1] = (float)(d * std::sin(a));
        const int t = freeIdx({cur});
        postRun(5, t, cur, -1, A);
        return t;
    }
    if (n == "vignette") {
        A.a[0] = (float)fx::num(P["amount"]); A.a[1] = (float)fx::num(P["radius"]); A.a[2] = (float)fx::num(P["soft"]);
        colorOf(P["color"], black, A.c);
        const int t = freeIdx({cur});
        postRun(6, t, cur, -1, A);
        return t;
    }
    if (n == "mirror") {
        A.a[0] = P["axis"].toString() == "x" ? 0.f : 1.f;
        A.a[1] = fx::truthy(P["half"]) ? 1.f : 0.f;
        const int t = freeIdx({cur});
        postRun(7, t, cur, -1, A);
        return t;
    }
    if (n == "shadow") {
        A.a[0] = (float)(fx::num(P["x"]) * k); A.a[1] = (float)(fx::num(P["y"]) * k); A.a[2] = (float)fx::num(P["opacity"]);
        colorOf(P["color"], black, A.c);
        const int b0 = freeIdx({cur});
        postRun(8, b0, cur, -1, A);
        int s = b0;
        if (fx::num(P["blur"]) > 0) s = blurIdx(b0, fx::num(P["blur"]) * k / 2, cur);
        const int out = freeIdx({cur, s});
        PostArgs B;
        postRun(9, out, cur, s, B);
        return out;
    }
    if (n == "outline") {
        const int w = std::min(24, std::max(1, (int)std::nearbyint(fx::num(P["width"]) * k)));
        A.a[0] = (float)w; A.a[1] = (float)fx::num(P["opacity"]);
        colorOf(P["color"], black, A.c);
        const int s = freeIdx({cur});
        postRun(10, s, cur, -1, A);
        const int out = freeIdx({cur, s});
        PostArgs B;
        postRun(9, out, cur, s, B);
        return out;
    }
    if (n == "glow") {
        A.a[0] = (float)fx::num(P["threshold"]);
        const bool hasCol = fx::truthy(P["color"]);
        A.a[1] = hasCol ? 1.f : 0.f;
        if (hasCol) colorOf(P["color"], white, A.c);
        const int g0 = freeIdx({cur});
        postRun(11, g0, cur, -1, A);
        const int g1 = blurIdx(g0, std::max(0.5, fx::num(P["radius"]) * k / 2), cur);
        PostArgs B;
        B.a[0] = (float)fx::num(P["strength"]);
        const int out = freeIdx({cur, g1});
        postRun(12, out, cur, g1, B);
        return out;
    }
    if (n == "chroma-key") {
        A.a[0] = (float)fx::num(P["similarity"]); A.a[1] = (float)fx::num(P["smooth"]); A.a[2] = (float)fx::num(P["spill"]);
        colorOf(P["color"], green, A.c);
        const int t = freeIdx({cur});
        postRun(13, t, cur, -1, A);
        return t;
    }
    if (n == "glitch") {
        const double g = fx::num(P["strength"]);
        const int every = std::max(1, (int)fx::num(P["every"], 2));
        const long long fr = (long long)std::nearbyint(f.t * tl_.fps) / every;
        PyRandom rnd((long long)fx::num(P["seed"]) * 100003 + fr);
        auto uni = [&](double a, double b) { return rnd.uniform(a, b); };
        int y = 0, ng = 0;
        const int H = tl_.H, W = tl_.W;
        while (y < H && ng < 128) {
            const int hh = (int)rnd.randint((int)(H * 0.01) + 1, (int)(H * 0.1) + 2);
            if (rnd.random() < 0.45 * g + 0.05) {
                A.g[ng][0] = (float)y; A.g[ng][1] = (float)(y + hh); A.g[ng][2] = (float)(int)(uni(-1, 1) * g * W * 0.06);
                ++ng;
            }
            y += hh;
        }
        A.ng = ng;
        A.a[0] = (float)std::nearbyint(g * 10 * k * uni(0.3, 1));
        const int t = freeIdx({cur});
        postRun(17, t, cur, -1, A);
        return t;
    }
    if (n == "motion-blur") {
        const double L = fx::num(P["length"]) * k;
        if (L <= 0.5) return cur;   // length=0 は layerCanvas が要素の動きから作る
        const double a = fx::num(P["angle"]) * M_PI / 180;
        A.a[0] = (float)(L * std::cos(a)); A.a[1] = (float)(L * std::sin(a)); A.a[2] = (float)std::max(2, (int)fx::num(P["samples"], 8));
        const int t = freeIdx({cur});
        postRun(18, t, cur, -1, A);
        return t;
    }
    static const QHash<QString, int> kinds{{"brightness", 1}, {"contrast", 2}, {"saturation", 3}, {"tint", 4}, {"grayscale", 5}, {"invert", 6}};
    if (kinds.contains(n)) {
        A.a[0] = (float)kinds[n]; A.a[1] = (float)fx::num(P["amount"]);
        if (n == "tint") { colorOf(P["black"], black, A.c); colorOf(P["white"], white, A.d); }
        const int t = freeIdx({cur});
        postRun(14, t, cur, -1, A);
        return t;
    }
    return cur;
}

int Renderer::chainFx(int cur, const std::vector<fx::PixFx>& list, double blurSigma, const QJsonArray& masks, double maskOpacity) {
    const double k = tl_.H / 1080.0;
    if (blurSigma >= 0.3) cur = blurIdx(cur, blurSigma);
    for (const auto& f : list) {
        const int p = fxPass(f, cur);
        if (p == cur) continue;
        QJsonObject reg;
        if (regionOf(f.P, &reg)) {
            PostArgs A;
            A.nm = 1;
            fillMask(A, 0, reg, k, 0);
            const int out = freeIdx({cur, p});
            postRun(15, out, cur, p, A);
            cur = out;
        } else {
            cur = p;
        }
    }
    if (!masks.isEmpty()) {
        PostArgs A;
        A.nm = std::min(4, (int)masks.size());
        for (int i = 0; i < A.nm; ++i) {
            const QJsonObject s = masks[i].toObject();
            const QString md = s["mode"].toString();
            fillMask(A, i, s, k, md == "subtract" ? 1 : md == "intersect" ? 2 : 0);
        }
        A.b[0] = maskOpacity >= 0 ? (float)maskOpacity : 1.f;   // mask_opacity
        const int out = freeIdx({cur});
        postRun(16, out, cur, -1, A);
        cur = out;
    }
    glActiveTexture(GL_TEXTURE0);
    return cur;
}

// v1_fx の揺れ以外(end が必要なもの)を出力時刻の範囲にしておく(fx.py v1_fx_groups)
void Renderer::initV1Fx() {
    v1fx_.clear();
    for (const auto& v : proj_.v1fx) {
        const QJsonObject f = v.toObject();
        const QString type = f["type"].toString();
        if (type == "shake" || f["end"].isNull() || f["end"].isUndefined()) continue;
        bool ok = false;
        fx::params("fx", type, QJsonObject(), &ok);
        if (!ok) continue;
        double a = parseTimeOr(f["start"], 0), b = parseTimeOr(f["end"], 0), oa, ob;
        if (proj_.timebase == "source") { if (!tl_.timemap.outRange(a, b, &oa, &ob)) continue; }
        else { oa = a; ob = std::min(b, tl_.total); }
        if (!(ob > oa)) continue;
        V1Fx x;
        x.a = oa; x.b = ob; x.type = type; x.P = fx::params("fx", type, f);
        v1fx_.push_back(x);
    }
}

void Renderer::drawV1Fx(double T) {
    std::vector<fx::PixFx> list;
    for (const auto& x : v1fx_) {
        if (T < x.a - 1e-9 || T >= x.b) continue;
        QJsonObject P = x.P;
        const double tau = T - x.a;
        if (fx::hasDyn(P)) {
            fx::Dyn d;
            d.dur = x.b - x.a; d.fps = tl_.fps;
            P = fx::pval(P, tau, d, fx::seedOf(x.type)).toObject();
        }
        list.push_back({x.type, P, tau});
    }
    if (list.empty()) return;
    copyToCanvas(0);
    const int r = chainFx(0, list, 0, QJsonArray(), -1);
    compositeCanvas(r, 1.0);
}

}  // namespace vn
