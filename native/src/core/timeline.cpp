#include "timeline.h"
#include "fxcore.h"
#include <algorithm>
#include <cmath>

namespace vn {

static double clamp01(double x) { return std::min(1.0, std::max(0.0, x)); }
static int roundEven(double v) { return 2 * (int)std::nearbyint(v / 2.0); }

double Timeline::snapUp(double x, double start, double f) const {
    return start + std::ceil((x - start) * f - 1e-4) / f;
}

bool Timeline::init(const Project& p, MediaProvider prov) {
    p_ = p;
    prov_ = prov;
    src = prov(p.resolve(p.source));
    if (!src.ok || src.isImage) { error = "source を開けません: " + p.source; return false; }
    W = src.w; H = src.h;
    fps = p.outFps > 0 ? p.outFps : std::nearbyint(src.fps * 1000) / 1000;
    TimeMap tm(src.duration, p.cuts, p.gaps);
    total = tm.total;
    timemap = tm;
    double o = 0;
    for (const auto& pc : tm.pieces()) {
        PieceInfo pi{pc.gap, pc.gap ? 0 : pc.a, pc.gap ? 0 : pc.b, o, o + pc.len()};
        pieces.push_back(pi);
        o += pc.len();
    }
    hideV1_ = p.hide.contains("v1");
    for (auto [a, b] : p.vmute) {
        double oa, ob;
        if (tm.outRange(a, b, &oa, &ob)) black_.push_back({oa, ob});
    }
    // 差し込み: 非表示トラックを除き、トラック番号の小さい順(同じトラック内はリスト順)
    std::vector<int> idx;
    for (int i = 0; i < (int)p.overlays.size(); ++i)
        if (!p.overlays[i].sub && !p.hide.contains(QString("v%1").arg(p.overlays[i].track))) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(), [&](int x, int y) { return p.overlays[x].track < p.overlays[y].track; });
    for (int i : idx) {
        const OverlaySpec& s = p.overlays[i];
        double ra, rb;
        if (s.hasGapAt) {
            double g0, gl;
            if (!tm.gapAt(s.gapAt, &g0, &gl)) continue;
            ra = g0 + std::max(0.0, s.start);
            rb = g0 + std::min(s.end, gl);
        } else if (p.timebase == "source") {
            if (!tm.outRange(s.start, s.end, &ra, &rb)) continue;
        } else {
            ra = s.start; rb = std::min(s.end, total);
        }
        if (!(rb > ra)) continue;
        LayerSpec L;
        L.ov = i; L.a = ra; L.b = rb;
        L.gen = s.gen;
        L.path = s.gen.isEmpty() ? p.resolve(s.file) : QString();
        const QString ext = L.path.section('.', -1).toLower();
        L.isImage = !s.gen.isEmpty() || (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "bmp");
        layers.push_back(L);
        // comp の中の層: 時刻は comp の頭から(絶対時刻に直す)。自分では画面に出ない(comp が呼ぶ)
        for (int k : s.subs) {
            const OverlaySpec& q = p.overlays[k];
            LayerSpec S;
            S.ov = k; S.sub = true;
            S.a = ra + std::max(0.0, q.start);
            S.b = std::min(rb, ra + q.end);
            if (!(S.b > S.a)) continue;
            S.gen = q.gen;
            S.path = q.gen.isEmpty() ? p.resolve(q.file) : QString();
            const QString e2 = S.path.section('.', -1).toLower();
            S.isImage = !q.gen.isEmpty() || (e2 == "png" || e2 == "jpg" || e2 == "jpeg" || e2 == "webp" || e2 == "bmp");
            layers.push_back(S);
        }
    }
    // テロップ / トピック(render.py の Plan.conv)
    auto conv = [&](const std::vector<SubSpec>& in, std::vector<SubPlaced>* out) {
        for (int i = 0; i < (int)in.size(); ++i) {
            const SubSpec& s = in[i];
            if (s.needsBake) continue;   // 動きを付けたテロップは gen="telop" の差し込みで描く(ASS には出さない)
            double ra, rb;
            if (s.hasGapAt) {
                double g0, gl;
                if (!tm.gapAt(s.gapAt, &g0, &gl)) continue;
                ra = g0 + std::max(0.0, s.start);
                rb = g0 + std::min(s.end, gl);
            } else if (p.timebase == "source") {
                if (!tm.outRange(s.start, s.end, &ra, &rb)) continue;
            } else {
                ra = s.start; rb = std::min(s.end, total);
            }
            if (rb > ra) out->push_back({i, ra, rb});
        }
    };
    // v1_fx の揺れ(fx.py v1_items / v1_zoom)
    {
        const double k = H / 1080.0;
        double s = 1.0;
        for (const auto& v : p.v1fx) {
            const QJsonObject f = v.toObject();
            if (f["type"].toString() != "shake") continue;
            const QJsonObject P = fx::params("fx", "shake", f);
            const double t0 = parseTimeOr(f["start"], 0);
            const double o = p.timebase == "source" ? tm.out(t0) : t0;
            double e = o + fx::num(P["dur"]);
            if (!f["end"].isNull() && !f["end"].isUndefined()) {
                const double t1 = parseTimeOr(f["end"], 0);
                e = std::min(e, p.timebase == "source" ? tm.out(t1) : t1);
            }
            if (!(e > o)) continue;
            shakes.push_back({o, e, P});
            const QJsonValue am = P["amp"];
            double ax, ay;
            if (am.isArray()) { ax = am.toArray().at(0).toDouble(); ay = am.toArray().at(1).toDouble(); }
            else ax = ay = am.toDouble();
            s = std::max({s, 1 + 2 * (std::abs(ax) * k + 1) / W, 1 + 2 * (std::abs(ay) * k + 1) / H});
        }
        shakeZoom = s;
    }
    // トランジション
    {
        auto joint = [&](double o, Trans* t) {
            double acc = 0;
            t->hasA = t->hasB = false;
            for (const auto& pc : pieces) {
                const double L = pc.o1 - pc.o0;
                if (!pc.gap) {
                    if (std::abs(acc + L - o) < 1e-3) { t->jA = pc.b; t->hasA = true; }
                    else if (std::abs(acc - o) < 1e-3) { t->jB = pc.a; t->hasB = true; }
                    else if (acc < o && o < acc + L) { t->jA = t->jB = pc.a + o - acc; t->hasA = t->hasB = true; return; }
                }
                acc += L;
            }
        };
        static const QSet<QString> kSrc{"crossfade", "push-left", "push-right", "push-up", "push-down", "zoom", "whip", "glitch"};
        static const QSet<QString> kAll{"wipe-diag", "dip-black", "dip-white", "flash", "iris", "crossfade", "push-left", "push-right", "push-up", "push-down", "zoom", "whip", "glitch"};
        auto add = [&](double o, const QJsonValue& spec) {
            const QString name = spec.isObject() ? spec.toObject()["type"].toString() : spec.toString();
            if (!kAll.contains(name)) return;
            Trans t;
            t.name = name;
            t.P = fx::params("trans", name, spec.isObject() ? spec.toObject()["params"].toObject() : QJsonObject());
            if (spec.isObject() && spec.toObject().contains("dur") && !spec.toObject()["dur"].isNull()) t.P["dur"] = spec.toObject()["dur"].toDouble();
            if (spec.isObject()) t.se = spec.toObject()["se"].toObject();
            t.o = o;
            const double dur = fx::num(t.P["dur"]);
            t.a = std::max(0.0, o - dur);
            t.b = std::min(total, o + dur);
            t.src = kSrc.contains(name);
            joint(o, &t);
            trans.push_back(t);
        };
        for (const auto& v : p.transitions) {
            const QJsonObject t = v.toObject();
            if (t.contains("gap") && !t["gap"].isNull()) {
                double g0, gl;
                if (!tm.gapAt(parseTimeOr(t["gap"], 0), &g0, &gl)) continue;
                const QString sides = t.contains("sides") ? t["sides"].toString() : "both";
                if (sides == "both" || sides == "in") add(g0, t);
                if (sides == "both" || sides == "out") add(g0 + gl, t);
            } else if (t.contains("at") && !t["at"].isNull()) {
                const double a = parseTimeOr(t["at"], 0);
                add(p.timebase == "source" ? tm.out(a) : a, t);
            }
        }
        const size_t nOwn = trans.size();
        QJsonValue cv = p.cutFx["video"];
        if (cv.isString() && p.cutFx.contains("dur") && !p.cutFx["dur"].isNull()) {
            QJsonObject o; o["type"] = cv.toString(); o["dur"] = p.cutFx["dur"]; cv = o;
        }
        const QString cvName = cv.isObject() ? cv.toObject()["type"].toString() : cv.toString();
        if (cvName == "dip-black" || cvName == "dip-white") {
            dipColor = cvName == "dip-white" ? "#FFFFFF" : "#000000";
            dipDur = p.cutFx.contains("dur") ? p.cutFx["dur"].toDouble(0.12) : 0.12;
        } else if (kAll.contains(cvName)) {
            double acc = 0;
            bool prevSrc = false, first = true;
            for (const auto& pc : pieces) {
                if (!first && prevSrc && !pc.gap) {
                    bool own = false;
                    for (size_t i = 0; i < nOwn; ++i) if (std::abs(trans[i].o - acc) < 1e-3) own = true;
                    if (!own) add(acc, cv);
                }
                acc += pc.o1 - pc.o0;
                prevSrc = !pc.gap;
                first = false;
            }
        }
    }
    conv(p.telops, &telopsOut);
    conv(p.topics, &topicsOut);
    return true;
}

bool Timeline::shaking(double T) const {
    for (const auto& s : shakes) if (T >= s.a - 1e-9 && T <= s.b + 1e-9) return true;
    return false;
}

void Timeline::shakeAt(double T, double* dx, double* dy) const {
    *dx = *dy = 0;
    const double k = H / 1080.0;
    for (const auto& s : shakes) {
        if (T < s.a - 1e-9 || T > s.b + 1e-9) continue;
        double x, y;
        fx::shakeXY(T - s.a, s.P, k, &x, &y);
        *dx += x; *dy += y;
    }
}

int Timeline::pieceAt(double T) const {
    if (pieces.empty()) return -1;
    int lo = 0, hi = (int)pieces.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (pieces[mid].o0 <= T + 1e-9) lo = mid; else hi = mid - 1;
    }
    return lo;
}

Timeline::V1State Timeline::v1At(double T) const {
    V1State s;
    int pi = pieceAt(T);
    s.piece = pi;
    if (pi < 0 || pieces[pi].gap || hideV1_) return s;
    for (auto [a, b] : black_)
        if (T >= a && T <= b) return s;
    const PieceInfo& pc = pieces[pi];
    double x = pc.a + (T - pc.o0);
    s.srcT = std::fabs(src.fps - fps) < 0.01 ? snapUp(x, src.start, src.fps) : x;
    s.black = false;
    return s;
}

std::vector<int> Timeline::activeLayers(double T) const {
    std::vector<int> r;
    for (int i = 0; i < (int)layers.size(); ++i)
        if (!layers[i].sub && T >= layers[i].a && T <= layers[i].b) r.push_back(i);
    return r;
}

std::vector<int> Timeline::layersIn(double t0, double t1) const {
    std::vector<int> r;
    for (int i = 0; i < (int)layers.size(); ++i)
        if (layers[i].a <= t1 && layers[i].b >= t0) r.push_back(i);
    return r;
}

bool Timeline::prepare(int li) {
    LayerSpec& L = layers[li];
    if (L.propsDone) return !L.bad;
    L.propsDone = true;
    if (!L.gen.isEmpty()) {   // 生成要素: 画面いっぱい
        L.m.ok = true; L.m.isImage = true; L.m.w = W; L.m.h = H;
        Geom& g = L.g;
        g.lw = W; g.lh = H; g.cx = g.cy = 0; g.cw = W; g.ch = H; g.texW = W; g.texH = H; g.done = true;
        return true;
    }
    L.m = prov_(L.path);
    if (!L.m.ok || L.m.w <= 0 || L.m.h <= 0) { L.bad = true; return false; }
    L.isImage = L.m.isImage;
    makeGeom(L);
    return true;
}

void Timeline::makeGeom(LayerSpec& L) {
    const OverlaySpec& s = p_.overlays[L.ov];
    Geom& g = L.g;
    const double iw = L.m.w, ih = L.m.h;
    const double k = H / 1080.0;
    const int m = (int)std::nearbyint(40 * k);
    const QString pos = s.hasPos ? (s.posIsXY ? QString("xy") : s.posName) : (s.hasScale ? "top-right" : "full");
    double kk;
    if (pos == "full") {
        const bool cover = s.fit == "cover";
        kk = cover ? std::max(W / iw, H / ih) : std::min(W / iw, H / ih);
        g.lw = W; g.lh = H;
        const double sw = iw * kk, sh = ih * kk;
        if (cover) {
            g.cx = 0; g.cy = 0; g.cw = W; g.ch = H;
            g.u0 = (sw - W) / 2 / sw; g.u1 = 1 - g.u0;
            g.v0 = (sh - H) / 2 / sh; g.v1 = 1 - g.v0;
            g.texW = (int)std::nearbyint(sw); g.texH = (int)std::nearbyint(sh);
        } else {
            g.cw = (int)std::nearbyint(sw); g.ch = (int)std::nearbyint(sh);
            g.cx = s.fxPath ? (W - g.cw) / 2 : ((W - g.cw) / 2) & ~1;
            g.cy = s.fxPath ? (H - g.ch) / 2 : ((H - g.ch) / 2) & ~1;
            g.pad = g.cw < W || g.ch < H;
            g.texW = g.cw; g.texH = g.ch;
        }
        g.bx = 0; g.by = 0;
    } else {
        const int w = (int)(W * s.scale / 2) * 2;
        const int h = std::max(2, roundEven(w * ih / iw));
        g.lw = w; g.lh = h; g.cx = 0; g.cy = 0; g.cw = w; g.ch = h;
        g.texW = w; g.texH = h;
        kk = w / iw;
        if (pos == "xy") { g.bx = std::nearbyint(s.px * k); g.by = std::nearbyint(s.py * k); }
        else if (pos == "center") { g.bx = (W - w) / 2.0; g.by = (H - h) / 2.0; if (s.fxPath) { g.bx = std::nearbyint(g.bx); g.by = std::nearbyint(g.by); } }
        else if (pos == "top-left") { g.bx = m; g.by = m; }
        else if (pos == "bottom-left") { g.bx = m; g.by = H - h - m; }
        else if (pos == "bottom-right") { g.bx = W - w - m; g.by = H - h - m; }
        else { g.bx = W - w - m; g.by = m; }   // top-right(知らない名前は Python では KeyError)
    }
    // 縮めても小さくならない画像(k>=0.95)は元の大きさのまま持つ(Python の _small_image と同じ)
    if (kk >= 0.95) { g.texW = L.m.w; g.texH = L.m.h; }
    g.done = true;
}

Timeline::LayerState Timeline::state(int li, double T) {
    LayerSpec& L = layers[li];
    const OverlaySpec& s = p_.overlays[L.ov];
    const Geom& g = L.g;
    LayerState st{li, g.bx, g.by, 1.0, 0.0};
    if (s.fxPath) {   // 動き・フェードはエフェクトの計算(fxcore)で描く側が付ける
        if (!L.isImage) {
            const double in0 = L.m.fps > 0 ? snapUp(s.in, L.m.start, L.m.fps) : s.in;
            st.srcT = in0 + (T - L.a);
        }
        return st;
    }
    const double a = L.a, b = L.b;
    const double dur = std::min(s.animDur, (b - a) / 2);
    const double pIn = dur > 0 ? 1.0 - clamp01((T - a) / dur) : 0.0;
    const double pOut = dur > 0 ? clamp01((T - (b - dur)) / dur) : 0.0;
    double x = g.bx, y = g.by;
    const double lw = g.lw, lh = g.lh;
    const std::pair<QString, double> seq[2] = {{s.inAnim, pIn}, {s.outAnim, pOut}};
    for (const auto& [kind, p] : seq) {
        if (kind == "slide-left") x -= p * (x + lw);
        else if (kind == "slide-right") x += p * (W - x);
        else if (kind == "slide-up") y += p * (H - y);
        else if (kind == "slide-down") y -= p * (y + lh);
    }
    st.x = (double)((long long)x & ~1LL);   // overlay(yuv420)は小数を切り捨てて偶数にそろえる
    st.y = (double)((long long)y & ~1LL);
    if (s.inAnim == "fade") st.alpha *= dur <= 0 ? 1.0 : clamp01((T - a) / dur);
    if (s.outAnim == "fade") st.alpha *= dur <= 0 ? 0.0 : 1.0 - clamp01((T - (b - dur)) / dur);
    if (!L.isImage) {
        const double in0 = L.m.fps > 0 ? snapUp(s.in, L.m.start, L.m.fps) : s.in;
        st.srcT = in0 + (T - a);
    }
    return st;
}

}  // namespace vn
