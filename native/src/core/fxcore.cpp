#include "fxcore.h"
#include <QJsonDocument>
#include <QMap>
#include <algorithm>
#include <cmath>
#include "expr.h"
#include "pyrand.h"
#include "timeutil.h"

namespace vn::fx {

// ---------------------------------------------------------------- 値の読み出し
double num(const QJsonValue& v, double def) {
    if (v.isDouble()) return v.toDouble();
    if (v.isBool()) return v.toBool() ? 1.0 : 0.0;
    if (v.isString()) {
        bool ok = false;
        const double d = v.toString().trimmed().toDouble(&ok);
        return ok ? d : def;
    }
    return def;
}

bool truthy(const QJsonValue& v) {
    if (v.isNull() || v.isUndefined()) return false;
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) return !v.toString().isEmpty();
    if (v.isArray()) return !v.toArray().isEmpty();
    if (v.isObject()) return !v.toObject().isEmpty();
    return false;
}

bool parseColor(const QJsonValue& v, int rgb[3], const int def[3]) {
    if (v.isString()) {
        QString s = v.toString();
        while (s.startsWith('#')) s.remove(0, 1);
        if (s.size() >= 6) {
            bool ok = true;
            for (int i = 0; i < 3; ++i) rgb[i] = s.mid(i * 2, 2).toInt(&ok, 16);
            return true;
        }
    } else if (v.isArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() >= 3) {
            for (int i = 0; i < 3; ++i) rgb[i] = (int)num(a[i]);
            return true;
        }
    }
    for (int i = 0; i < 3; ++i) rgb[i] = def[i];
    return false;
}

// ---------------------------------------------------------------- イージング
double smooth(double u) {
    u = std::min(1.0, std::max(0.0, u));
    return 3 * u * u - 2 * u * u * u;
}
double backEase(double u, double s) {
    u -= 1;
    return 1 + (s + 1) * u * u * u + s * u * u;
}
static double elastic(double u) {
    if (u <= 0 || u >= 1) return std::min(1.0, std::max(0.0, u));
    return std::pow(2.0, -10 * u) * std::sin((u * 10 - 0.75) * 2 * M_PI / 3) + 1;
}
double bounceEase(double u) {
    const double n = 7.5625, d = 2.75;
    if (u < 1 / d) return n * u * u;
    if (u < 2 / d) { u -= 1.5 / d; return n * u * u + 0.75; }
    if (u < 2.5 / d) { u -= 2.25 / d; return n * u * u + 0.9375; }
    u -= 2.625 / d;
    return n * u * u + 0.984375;
}
static double bezier(double a, double b, double c, double d, double u) {
    if (u <= 0 || u >= 1) return std::min(1.0, std::max(0.0, u));
    double lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const double t = (lo + hi) / 2;
        const double x = 3 * a * t * (1 - t) * (1 - t) + 3 * c * t * t * (1 - t) + t * t * t;
        if (x < u) lo = t; else hi = t;
    }
    const double t = (lo + hi) / 2;
    return 3 * b * t * (1 - t) * (1 - t) + 3 * d * t * t * (1 - t) + t * t * t;
}

double ease(const QString& name, double u) {
    u = std::min(1.0, std::max(0.0, u));
    if (name == "linear") return u;
    if (name == "smoothstep" || name == "ease-in-out") return smooth(u);
    if (name == "ease-in") return u * u;
    if (name == "ease-out") return 1 - (1 - u) * (1 - u);
    if (name == "back") return backEase(u);
    if (name == "elastic") return elastic(u);
    if (name == "bounce") return bounceEase(u);
    if (name == "hold") return u >= 1 ? 1.0 : 0.0;
    QString s = name;
    s.remove(' ');
    if (s.startsWith("cubic-bezier(") && s.endsWith(")")) {
        const QStringList p = s.mid(13, s.size() - 14).split(',');
        if (p.size() == 4) return bezier(p[0].toDouble(), p[1].toDouble(), p[2].toDouble(), p[3].toDouble(), u);
    }
    return u;
}

// ---------------------------------------------------------------- キーフレーム・式
static QJsonValue lerpVal(const QJsonValue& a, const QJsonValue& b, double u) {
    if (a.isString() && b.isString() && a.toString().startsWith('#') && b.toString().startsWith('#')) {
        const int d0[3] = {255, 255, 255};
        int ca[3], cb[3];
        parseColor(a, ca, d0);
        parseColor(b, cb, d0);
        QString s = "#";
        for (int i = 0; i < 3; ++i) s += QString("%1").arg((int)std::nearbyint(ca[i] + (cb[i] - ca[i]) * u), 2, 16, QChar('0')).toUpper();
        return s;
    }
    if (a.isArray() && b.isArray() && a.toArray().size() == b.toArray().size()) {
        QJsonArray r;
        const QJsonArray x = a.toArray(), y = b.toArray();
        for (int i = 0; i < x.size(); ++i) r.append(lerpVal(x[i], y[i], u));
        return r;
    }
    if (a.isDouble() && b.isDouble()) return a.toDouble() + (b.toDouble() - a.toDouble()) * u;
    return u >= 1 ? b : a;
}

QJsonValue keyValue(const QJsonArray& keys, double t, const QString& loop) {
    std::vector<QJsonArray> ks;
    for (const QJsonValue& k : keys)
        if (k.isArray() && k.toArray().size() >= 2) ks.push_back(k.toArray());
    if (ks.empty()) return 0.0;
    std::stable_sort(ks.begin(), ks.end(), [](const QJsonArray& x, const QJsonArray& y) { return num(x[0]) < num(y[0]); });
    const double t0 = num(ks.front()[0]), t1 = num(ks.back()[0]);
    if (!loop.isEmpty() && t1 > t0 && t > t1) {
        const double L = t1 - t0;
        const double n = std::floor((t - t0) / L);
        double r = (t - t0) - n * L;
        t = t0 + ((loop == "pingpong" && ((long long)n) % 2) ? L - r : r);
    }
    if (t <= t0) return ks.front()[1];
    if (t >= t1) return ks.back()[1];
    for (size_t i = 0; i + 1 < ks.size(); ++i) {
        const QJsonArray &a = ks[i], &b = ks[i + 1];
        if (num(a[0]) <= t && t <= num(b[0])) {
            QString e = "linear";
            if (b.size() > 2 && truthy(b[2])) e = b[2].toString();
            else if (a.size() > 2 && truthy(a[2])) e = a[2].toString();
            const double span = num(b[0]) - num(a[0]);
            const double u = span > 0 ? ease(e, (t - num(a[0])) / span) : 1.0;
            return lerpVal(a[1], b[1], u);
        }
    }
    return ks.back()[1];
}

bool isDyn(const QJsonValue& v) {
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        return o.contains("keys") || o.contains("expr");
    }
    return v.isString() && v.toString().startsWith('=');
}

bool hasDyn(const QJsonValue& v) {
    if (isDyn(v)) return true;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it)
            if (hasDyn(it.value())) return true;
    } else if (v.isArray()) {
        for (const QJsonValue& x : v.toArray())
            if (hasDyn(x)) return true;
    }
    return false;
}

QJsonValue pval(const QJsonValue& v0, double tau, const Dyn& d, long long seed, double base) {
    QJsonValue v = v0;
    if (v.isString() && v.toString().startsWith('=')) {
        QJsonObject o;
        o["expr"] = v.toString().mid(1);
        v = o;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.contains("keys")) return keyValue(o["keys"].toArray(), tau, o["loop"].toString());
        if (o.contains("expr")) {
            ExprCtx c;
            c.seed = seed;
            c.vars["t"] = tau;
            c.vars["dur"] = d.dur;
            c.vars["value"] = o.contains("value") ? num(o["value"]) : base;
            c.vars["fps"] = d.fps;
            c.vars["frame"] = std::nearbyint(tau * d.fps);
            return evalExpr(o["expr"].toString(), c);
        }
        QJsonObject r;
        int i = 0;
        for (auto it = o.begin(); it != o.end(); ++it, ++i) r[it.key()] = pval(it.value(), tau, d, seed + i, base);
        return r;
    }
    if (v.isArray()) {
        const QJsonArray a = v.toArray();
        bool any = false;
        for (const QJsonValue& x : a) if (x.isObject()) any = true;
        if (!any) return v;
        QJsonArray r;
        for (int i = 0; i < a.size(); ++i) r.append(pval(a[i], tau, d, seed + i, base));
        return r;
    }
    return v;
}

static uint32_t crc32(const QByteArray& b) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char x : b) {
        c ^= x;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

long long seedOf(const QString& name, int extra) { return (long long)(crc32(name.toUtf8()) % 100000) + (long long)extra * 7; }

// ---------------------------------------------------------------- 既定値
static const char* kDefaults = R"JSON({
"anim": {
 "fade": {"dur": 0.3},
 "slide-left": {"dur": 0.3}, "slide-right": {"dur": 0.3}, "slide-up": {"dur": 0.3}, "slide-down": {"dur": 0.3},
 "pop": {"dur": 0.4, "scales": [1.9,1.55,1.28,1.1,0.95,0.98,1.02,1.0], "amps": [0,0,0,0,14,12,10,8,6,4,2,0], "alphas": [0.5,0.75], "seed": 0},
 "zoom-in": {"dur": 0.3, "from": 0.3, "ease": "ease-out", "fade": true},
 "zoom-out": {"dur": 0.3, "from": 2.0, "ease": "ease-out", "fade": true},
 "bounce": {"dur": 0.6, "height": 220, "fade": true},
 "stamp": {"dur": 0.45, "from": 2.4, "hit": 0.35, "amp": 10, "seed": 0},
 "blur-in": {"dur": 0.3, "radius": 30, "ease": "ease-out"},
 "blur-out": {"dur": 0.3, "radius": 30, "ease": "ease-out"},
 "rotate-in": {"dur": 0.4, "angle": -180, "from": 0.2, "ease": "ease-out"},
 "wipe-left": {"dur": 0.3, "feather": 30, "ease": "ease-out"}, "wipe-right": {"dur": 0.3, "feather": 30, "ease": "ease-out"},
 "wipe-up": {"dur": 0.3, "feather": 30, "ease": "ease-out"}, "wipe-down": {"dur": 0.3, "feather": 30, "ease": "ease-out"},
 "shake-in": {"dur": 0.4, "amp": [30,18], "freq": [70,55], "fade": 0.1},
 "typewriter": {"dur": 0, "interval": 0.06}
},
"fx": {
 "shake": {"amp": [12,7], "freq": [95,77], "dur": 0.5, "decay": "linear"},
 "zoom": {"from": 1.0, "to": 1.15, "center": [0.5,0.5], "dur": null, "ease": "linear"},
 "drift": {"to": [60,0], "dur": null, "ease": "linear"},
 "mosaic": {"rect": null, "shape": "rect", "points": null, "size": 24, "feather": 0, "invert": false},
 "blur-region": {"rect": [760,440,400,200], "shape": "rect", "points": null, "radius": 20, "feather": 8, "invert": false},
 "blur": {"radius": 8},
 "motion-blur": {"length": 0, "angle": 0, "shutter": 0.5, "samples": 8},
 "glow": {"radius": 20, "strength": 1.0, "threshold": 0.6, "color": null},
 "shadow": {"x": 8, "y": 8, "blur": 8, "color": "#000000", "opacity": 0.6},
 "brightness": {"amount": 0.2}, "contrast": {"amount": 0.2}, "saturation": {"amount": 0.3},
 "tint": {"black": "#000000", "white": "#FFB040", "amount": 1.0},
 "grayscale": {"amount": 1.0}, "invert": {"amount": 1.0},
 "vignette": {"amount": 0.6, "radius": 0.6, "soft": 0.5, "color": "#000000"},
 "chroma-key": {"color": "#00FF00", "similarity": 0.35, "smooth": 0.1, "spill": 0.5},
 "rgb-split": {"amount": 8, "angle": 0},
 "glitch": {"strength": 1.0, "every": 2, "seed": 7},
 "mirror": {"axis": "x", "half": false},
 "outline": {"width": 6, "color": "#000000", "opacity": 1.0}
},
"gen": {
 "speedlines": {"count": 110, "inner": 0.42, "color": "#FFFFFF", "seed": 3, "blur": 1.2, "animate": false, "every": 2},
 "gloom": {"dark": 0.5, "bg": "#08061E", "line_color": "#968CFF", "line_alpha": 150, "seed": 5, "grow": 0},
 "flash": {"color": "#FFFFFF", "opacity": 0.6, "dur": 0.14, "fade": 0.1}
},
"trans": {
 "wipe-diag": {"angle": 18, "edge_color": "#C8141B", "edge_w": 90, "color": "#0E0E12", "dur": 0.4, "ease": "smoothstep"},
 "dip-black": {"dur": 0.15, "color": "#000000", "ease": "linear"},
 "dip-white": {"dur": 0.15, "color": "#FFFFFF", "ease": "linear"},
 "flash": {"dur": 0.12, "color": "#FFFFFF", "opacity": 1.0, "ease": "ease-in"},
 "iris": {"dur": 0.35, "color": "#000000", "center": [0.5,0.5], "soft": 3, "ease": "ease-in-out"},
 "crossfade": {"dur": 0.25, "ease": "linear"},
 "push-left": {"dur": 0.3, "ease": "smoothstep"}, "push-right": {"dur": 0.3, "ease": "smoothstep"},
 "push-up": {"dur": 0.3, "ease": "smoothstep"}, "push-down": {"dur": 0.3, "ease": "smoothstep"},
 "zoom": {"dur": 0.25, "scale": 1.8, "blur": 0.08},
 "whip": {"dur": 0.2, "dist": 0.6, "blur": 0.12},
 "glitch": {"dur": 0.15, "strength": 1.0, "seed": 7}
},
"text": {
 "pop": {"stagger": 0.05, "dur": 0.3, "order": "forward", "out": false, "overshoot": 2.2},
 "fade": {"stagger": 0.05, "dur": 0.3, "order": "forward", "out": false},
 "rise": {"stagger": 0.05, "dur": 0.3, "order": "forward", "out": false, "dist": 40},
 "drop": {"stagger": 0.05, "dur": 0.3, "order": "forward", "out": false, "dist": 80},
 "wave": {"stagger": 0.05, "dur": 0.3, "order": "forward", "out": false, "amp": 12, "freq": 1.2, "phase": 0.5},
 "shake": {"stagger": 0.05, "dur": 0.3, "order": "forward", "out": false, "amp": 4, "every": 2, "seed": 5}
}
})JSON";

QJsonObject params(const char* cat, const QString& name, const QJsonObject& user, bool* ok) {
    static const QJsonObject all = QJsonDocument::fromJson(QByteArray(kDefaults)).object();
    const QJsonObject c = all[cat].toObject();
    if (!c.contains(name)) { if (ok) *ok = false; return QJsonObject(); }
    if (ok) *ok = true;
    QJsonObject r = c[name].toObject();
    for (auto it = user.begin(); it != user.end(); ++it)
        if (it.key() != "type") r[it.key()] = it.value();
    return r;
}

// ---------------------------------------------------------------- 動き
bool St::identity() const {
    auto r = [](double v, double p) { const double m = std::pow(10.0, p); return std::nearbyint(v * m) / m; };
    return r(sx, 5) == 1.0 && r(sy, 5) == 1.0 && r(rot, 4) == 0.0 && std::nearbyint(dx) == 0 && std::nearbyint(dy) == 0 && r(op, 4) == 1.0 &&
           r(blur, 2) == 0.0 && !clipped() && r(feather, 1) == 0.0;
}

void St::clipTo(double l, double t, double r, double b, bool hl, bool ht, bool hr, bool hb) {
    if (hl) clip[0] = std::max(clip[0], l);
    if (ht) clip[1] = std::max(clip[1], t);
    if (hr) clip[2] = std::min(clip[2], r);
    if (hb) clip[3] = std::min(clip[3], b);
}

static void pair2(const QJsonValue& v, double* a, double* b) {
    if (v.isArray() && v.toArray().size() >= 2) { *a = num(v.toArray()[0]); *b = num(v.toArray()[1]); }
    else { *a = *b = num(v); }
}

void shakeXY(double tau, const QJsonObject& P, double k, double* dx, double* dy) {
    *dx = *dy = 0;
    const double d = num(P["dur"]);
    if (tau < 0 || tau >= d) return;
    const QString dc = P.contains("decay") ? P["decay"].toString() : "linear";
    const double dec = dc == "exp" ? std::exp(-4 * tau / d) : dc == "none" ? 1.0 : 1 - tau / d;
    double ax, ay, fx, fy;
    pair2(P["amp"], &ax, &ay);
    pair2(P["freq"], &fx, &fy);
    *dx = ax * k * std::sin(tau * fx) * dec;
    *dy = ay * k * std::cos(tau * fy) * dec;
}

std::vector<double> randList(long long seed, int n) {
    PyRandom r(seed);
    std::vector<double> v(n);
    for (int i = 0; i < n; ++i) v[i] = r.uniform(-1, 1);
    return v;
}

void ElemFx::init(const QJsonObject& item, double a_, double b_) {
    it = item;
    a = a_; b = b_;
    anims.clear(); typer.clear(); fxs.clear();
    auto present = [&](const char* k) {
        const QJsonValue v = it[k];
        return truthy(v) && !(v.isString() && v.toString() == "none");
    };
    for (const char* side : {"in", "out"}) {
        const QString key = QString(side) + "_anim";
        const QJsonValue spec = it[key];
        if (!truthy(spec) || (spec.isString() && spec.toString() == "none")) continue;
        const QString name = spec.isObject() ? spec.toObject()["type"].toString() : spec.toString();
        bool ok = false;
        QJsonObject P = params("anim", name, spec.isObject() ? spec.toObject() : QJsonObject(), &ok);
        if (!ok) continue;
        if (it.contains("anim_dur") && !(spec.isObject() && spec.toObject().contains("dur"))) P["dur"] = it["anim_dur"];
        const bool isIn = QString(side) == "in";
        if (name == "typewriter") { typer.push_back({isIn, std::max(1e-3, num(P["interval"]))}); continue; }
        const double dur = std::min(num(P["dur"]), (b - a) / ((present("in_anim") && present("out_anim")) ? 2 : 1));
        anims.push_back({isIn, name, P, std::max(1e-3, dur)});
    }
    keys = it["keys"].toObject();
    const QJsonArray fl = it["fx"].toArray();
    for (int i = 0; i < fl.size(); ++i) {
        const QJsonObject f = fl[i].toObject();
        bool ok = false;
        const QString type = f["type"].toString();
        QJsonObject P = params("fx", type, f, &ok);
        if (!ok) continue;
        for (auto kt = keys.begin(); kt != keys.end(); ++kt) {
            const int dot = kt.key().indexOf('.');
            if (dot < 0) continue;
            const QString tgt = kt.key().left(dot), prm = kt.key().mid(dot + 1);
            if (tgt == type || tgt == QString("fx[%1]").arg(i)) P[prm] = kt.value();
        }
        FxUse u;
        u.type = type;
        u.P = P;
        u.s = parseTimeOr(f["start"], 0);
        u.hasE = f.contains("end") && !f["end"].isNull();
        if (u.hasE) u.e = parseTimeOr(f["end"], 0);
        u.dyn = hasDyn(P);
        u.seed = seedOf(type, i);
        u.state = (type == "shake" || type == "zoom" || type == "drift");
        fxs.push_back(u);
    }
}

void computeState(const ElemFx& E, double tau, const StateCtx& c, St* stp, std::vector<PixFx>* pix) {
    St& st = *stp;
    const double L = E.b - E.a, k = c.k;
    auto bounded = [](double u) { return std::min(1.0, std::max(0.0, u)); };
    (void)bounded;
    for (const AnimUse& A : E.anims) {
        const double q = A.in ? tau : L - tau;
        if (!(q < A.dur)) continue;
        const double t = std::max(0.0, q), dur = A.dur;
        const QJsonObject& P = A.P;
        const QString& n = A.name;
        if (n == "fade") st.op *= t / dur;
        else if (n.startsWith("slide-")) {
            const double p = 1 - t / dur;
            if (n == "slide-left") st.dx += -p * (c.rx + c.rw);
            else if (n == "slide-right") st.dx += p * (c.W - c.rx);
            else if (n == "slide-up") st.dy += p * (c.H - c.ry);
            else st.dy += -p * (c.ry + c.rh);
        } else if (n == "pop") {
            const double step = dur / 12;
            const double kk = std::round(t / step * 1e6) / 1e6;
            const int i = (int)kk;
            const QJsonArray sc = P["scales"].toArray();
            const int ns = sc.size();
            double s = 1.0;
            if (ns > 0) {
                s = num(sc[ns - 1]);
                if (!(i >= ns - 1)) s = num(sc[i]) + (num(sc[i + 1]) - num(sc[i])) * (kk - i);
                if (i >= ns) s = 1.0;
            }
            st.sx *= s; st.sy *= s;
            const QJsonArray amps = P["amps"].toArray();
            const double a = (i < amps.size() ? num(amps[i]) : 0) * k;
            if (a != 0) {
                const auto r = randList((long long)num(P["seed"]), 2 * amps.size());
                st.dx += r[2 * i] * a;
                st.dy += r[2 * i + 1] * a;
            }
            const QJsonArray al = P["alphas"].toArray();
            if (i < al.size()) st.op *= num(al[i]);
        } else if (n == "zoom-in" || n == "zoom-out") {
            const double u = ease(P["ease"].toString(), t / dur);
            const double f = num(P["from"]);
            const double s = f + (1 - f) * u;
            st.sx *= s; st.sy *= s;
            if (truthy(P["fade"])) st.op *= std::min(1.0, t / dur);
        } else if (n == "bounce") {
            const double u = t / dur;
            st.dy -= (1 - bounceEase(u)) * num(P["height"]) * k;
            if (truthy(P["fade"])) st.op *= std::min(1.0, u * 5);
        } else if (n == "stamp") {
            const double th = dur * num(P["hit"]);
            if (t < th) {
                const double u = t / th;
                const double s = num(P["from"]) + (1 - num(P["from"])) * u * u;
                st.sx *= s; st.sy *= s;
                st.op *= std::min(1.0, u * 1.5);
            } else {
                const double rest = std::max(1e-6, dur - th);
                const double a = num(P["amp"]) * k * (1 - (t - th) / rest);
                const long long i = (long long)std::nearbyint((t - th) * c.fps);
                const auto r = randList((long long)num(P["seed"]), 128);
                st.dx += r[((2 * i) % 128 + 128) % 128] * a;
                st.dy += r[((2 * i + 1) % 128 + 128) % 128] * a;
            }
        } else if (n == "blur-in" || n == "blur-out") {
            const double u = ease(P["ease"].toString(), t / dur);
            st.blur += num(P["radius"]) * k * (1 - u);
            st.op *= u;
        } else if (n == "rotate-in") {
            const double u = ease(P["ease"].toString(), t / dur);
            st.rot += num(P["angle"]) * (1 - u);
            const double f = num(P["from"]);
            st.sx *= f + (1 - f) * u;
            st.sy *= f + (1 - f) * u;
            st.op *= std::min(1.0, 2 * t / dur);
        } else if (n.startsWith("wipe-")) {
            const double u = ease(P["ease"].toString(), t / dur);
            const double fe = num(P["feather"]) * k;
            st.feather = std::max(st.feather, fe);
            const double w = std::max(1.0, c.bbw), h = std::max(1.0, c.bbh);
            if (n == "wipe-left") st.clipTo(0, 0, u * (1 + fe / w), 0, false, false, true, false);
            else if (n == "wipe-right") st.clipTo(1 - u * (1 + fe / w), 0, 0, 0, true, false, false, false);
            else if (n == "wipe-down") st.clipTo(0, 0, 0, u * (1 + fe / h), false, false, false, true);
            else st.clipTo(0, 1 - u * (1 + fe / h), 0, 0, false, true, false, false);
        } else if (n == "shake-in") {
            QJsonObject sp;
            sp["amp"] = P["amp"]; sp["freq"] = P["freq"]; sp["dur"] = dur; sp["decay"] = "linear";
            double dx, dy;
            shakeXY(t, sp, k, &dx, &dy);
            st.dx += dx; st.dy += dy;
            if (num(P["fade"]) > 0) st.op *= std::min(1.0, t / num(P["fade"]));
        }
    }
    Dyn dy;
    dy.dur = L;
    dy.fps = c.fps;
    if (!E.keys.isEmpty()) {
        static const char* props[] = {"x", "y", "scale", "scale_x", "scale_y", "rot", "opacity", "anchor_x", "anchor_y"};
        for (const char* pr : props) {
            if (!E.keys.contains(pr)) continue;
            const double v = num(pval(E.keys[pr], tau, dy, seedOf(pr)));
            const QString p = pr;
            if (p == "x") st.dx += v * k;
            else if (p == "y") st.dy += v * k;
            else if (p == "scale") { st.sx *= v; st.sy *= v; }
            else if (p == "scale_x") st.sx *= v;
            else if (p == "scale_y") st.sy *= v;
            else if (p == "rot") st.rot += v;
            else if (p == "opacity") st.op *= std::max(0.0, std::min(1.0, v));
            else if (p == "anchor_x") st.ax = v;
            else st.ay = v;
        }
    }
    for (const FxUse& f : E.fxs) {
        if (tau < f.s || (f.hasE && tau >= f.e)) continue;
        QJsonObject P = f.P;
        if (f.dyn) P = pval(P, tau - f.s, dy, f.seed).toObject();
        P["_len"] = (f.hasE ? f.e : L) - f.s;
        if (!f.state) { if (pix) pix->push_back({f.type, P, tau - f.s}); continue; }
        const double t = tau - f.s;
        auto dur = [&]() {
            double d = num(P["dur"]);
            if (d == 0) d = num(P["_len"]);
            if (d == 0) d = L;
            return d;
        };
        if (f.type == "shake") {
            double dx, dyy;
            shakeXY(t, P, k, &dx, &dyy);
            st.dx += dx; st.dy += dyy;
        } else if (f.type == "zoom") {
            const double d = dur();
            const double u = d > 0 ? ease(P["ease"].toString(), t / d) : 1.0;
            const double s = num(P["from"]) + (num(P["to"]) - num(P["from"])) * u;
            st.sx *= s; st.sy *= s;
            pair2(P["center"], &st.ax, &st.ay);
        } else if (f.type == "drift") {
            const double d = dur();
            const double u = d > 0 ? ease(P["ease"].toString(), t / d) : 1.0;
            double tx, ty;
            pair2(P["to"], &tx, &ty);
            st.dx += tx * k * u;
            st.dy += ty * k * u;
        }
    }
}

// ---------------------------------------------------------------- 文字アニメーション
void textAnimState(const QString& type, const QJsonObject& P, St* st, double u, double tau, int i, int n, double k, double fps) {
    (void)n;
    if (type == "pop") {
        const double s = u < 1 ? std::max(0.0, backEase(std::min(1.0, u), num(P["overshoot"]))) : 1.0;
        st->sx *= s; st->sy *= s;
        st->op *= std::min(1.0, u * 4);
    } else if (type == "fade") {
        st->op *= u;
    } else if (type == "rise") {
        st->dy += (1 - ease("ease-out", u)) * num(P["dist"]) * k;
        st->op *= u;
    } else if (type == "drop") {
        st->dy -= (1 - bounceEase(std::min(1.0, u))) * num(P["dist"]) * k;
        st->op *= std::min(1.0, u * 3);
    } else if (type == "wave") {
        st->dy += std::sin(2 * M_PI * num(P["freq"]) * tau - i * num(P["phase"])) * num(P["amp"]) * k;
        st->op *= u;
    } else if (type == "shake") {
        PyRandom r((long long)num(P["seed"]) * 1000003 + (long long)i * 7919 + (long long)std::nearbyint(tau * fps) / std::max(1LL, (long long)num(P["every"])));
        st->dx += r.uniform(-1, 1) * num(P["amp"]) * k;
        st->dy += r.uniform(-1, 1) * num(P["amp"]) * k;
        st->op *= u;
    }
}

std::vector<int> charOrder(int n, const QString& order, long long seed) {
    std::vector<int> idx(n);
    for (int i = 0; i < n; ++i) idx[i] = i;
    if (order == "backward") std::reverse(idx.begin(), idx.end());
    else if (order == "center") {
        const double c = (n - 1) / 2.0;
        std::stable_sort(idx.begin(), idx.end(), [c](int a, int b) { return std::fabs(a - c) < std::fabs(b - c); });
    } else if (order == "random") {
        PyRandom(seed).shuffle(idx);
    }
    std::vector<int> rank(n, 0);
    for (int r = 0; r < n; ++r) rank[idx[r]] = r;
    return rank;
}

}  // namespace vn::fx
