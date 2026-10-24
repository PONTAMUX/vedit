#include "ass.h"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace vn {

namespace {

long long pyRound(double v) { return (long long)std::nearbyint(v); }   // Python の round(半分は偶数へ)

QString bgr(const QString& hex) {
    QString h = hex;
    while (h.startsWith('#')) h.remove(0, 1);
    h = h.leftJustified(6, '0');
    return ("&H" + h.mid(4, 2) + h.mid(2, 2) + h.mid(0, 2) + "&").toUpper();
}

QString alphaTag(double opacity) {
    return QString("&H%1&").arg(QString::number((int)pyRound((1 - opacity) * 255), 16).toUpper().rightJustified(2, '0'));
}

long long cs(double sec) { return pyRound(sec * 100); }

QString tstr(double sec) {
    long long c = cs(sec);
    const long long h = c / 360000; c %= 360000;
    const long long m = c / 6000; c %= 6000;
    const long long s = c / 100; c %= 100;
    return QString("%1:%2:%3.%4").arg(h).arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')).arg(c, 2, 10, QChar('0'));
}

QString esc(QString t) {
    t.replace("\\", QString(QChar(0xFF3C))).replace("{", QString(QChar(0xFF5B))).replace("}", QString(QChar(0xFF5D)));
    t.replace("\n", "\\N");
    return t;
}

double textWidth(const QString& text, double size) {
    const QStringList lines = text.split('\n');
    QString best = lines.value(0);
    int bestLen = (int)best.toUcs4().size();
    for (const QString& l : lines) {   // max(key=len): 同じ長さなら先のもの
        const int n = (int)l.toUcs4().size();
        if (n > bestLen) { best = l; bestLen = n; }
    }
    double w = 0;
    for (char32_t ch : best.toUcs4()) w += ch > 0x2E80 ? size : size * 0.55;
    return w;
}

QString num1(double v) { return QString::number(v, 'f', 1); }

QJsonObject defaultStyle(const QString& name) {
    QJsonObject o;
    if (name == "topic") {
        o = {{"font", "HGP創英角ｺﾞｼｯｸUB"}, {"size", 43}, {"bold", false}, {"color", "#FFFFFF"}, {"bar_color", "#3C3C3C"},
             {"bar_alpha", 1.0}, {"accent_color", "#C82828"}, {"accent_w", 10}, {"pad_x", 19}, {"pad_y", 12}, {"x", 0},
             {"y", 40}, {"min_width", 0}};
    } else {
        QJsonArray outs;
        outs.append(QJsonObject{{"color", "#000000"}, {"width", 6}});
        o = {{"font", "HGP創英角ｺﾞｼｯｸUB"}, {"size", 64}, {"bold", false}, {"color", "#FFFFFF"}, {"outlines", outs},
             {"align", "bottom"}, {"margin_v", 70}};
    }
    return o;
}

QJsonObject styleOf(const Project& p, const QString& name) {   // project.style(): {**base, **styles.get(name, {})}
    QJsonObject base = defaultStyle(name == "topic" ? "topic" : "telop");
    const QJsonObject ov = p.styles[name].toObject();
    for (auto it = ov.begin(); it != ov.end(); ++it) base[it.key()] = it.value();
    return base;
}

bool truthy(const QJsonValue& v) {
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) return !v.toString().isEmpty();
    return !v.isNull() && !v.isUndefined();
}

QString animTag(const SubSpec& it, double k, long long x, long long y) {
    const long long ms = (long long)(it.animDur * 1000);
    const QString pos = QString("\\pos(%1,%2)").arg(x).arg(y);
    if (it.anim == "fade") return QString("\\fad(%1,%1)").arg(ms) + pos;
    if (it.anim == "pop")
        return QString("\\fad(%1,%1)\\fscx70\\fscy70\\t(0,%2,\\fscx100\\fscy100)").arg(ms / 2).arg(ms) + pos;
    if (it.anim == "slide-up")
        return QString("\\fad(%1,%1)\\move(%2,%3,%2,%4,0,%1)").arg(ms).arg(x).arg(y + pyRound(60 * k)).arg(y);
    return pos;
}

}  // namespace

// テロップ 1 つ分の Dialogue(ass.py build の telops 部分)。hideFrom / showOnly は文字の番号(-1=なし)、layers: 0=両方 1=縁だけ 2=文字だけ
static QStringList telopEv(const Project& p, const SubSpec& it, int W, int H, double ta, double tb, int hideFrom, int showOnly, int layers) {
    const double k = H / 1080.0;
    QStringList ev;
    const QJsonObject st = styleOf(p, it.style);
    int an;
    long long x, y;
    if (it.hasPos) {
        an = 5; x = pyRound(it.px * W); y = pyRound(it.py * H);
    } else {
        const QString align = st["align"].isString() ? st["align"].toString() : "bottom";
        an = align == "top" ? 8 : align == "center" ? 5 : 2;
        const long long mv = pyRound(st["margin_v"].toDouble(70) * k);
        x = W / 2;
        y = align == "top" ? mv : align == "center" ? H / 2 : H - mv;
    }
    const QString base = QString("\\an%1%2\\fs%3\\b%4\\fn%5\\shad0\\1c%6")
                             .arg(an).arg(animTag(it, k, x, y)).arg(pyRound(st["size"].toDouble() * k))
                             .arg(truthy(st["bold"]) ? 1 : 0).arg(st["font"].toString()).arg(bgr(st["color"].toString()));
    QString text = esc(it.text);
    const QList<uint> cp = it.text.toUcs4();
    auto sub = [&](int from, int to) { return QString::fromUcs4(cp.constData() + from, to - from); };
    if (hideFrom >= 0) text = esc(sub(0, std::min<int>(hideFrom, cp.size()))) + "{\\alpha&HFF&}" + esc(sub(std::min<int>(hideFrom, cp.size()), cp.size()));
    if (showOnly >= 0 && showOnly < cp.size())
        text = "{\\alpha&HFF&}" + esc(sub(0, showOnly)) + "{\\alpha&H00&}" + esc(sub(showOnly, showOnly + 1)) + "{\\alpha&HFF&}" + esc(sub(showOnly + 1, cp.size()));
    const QJsonArray outs = st["outlines"].toArray();
    const int n = outs.size();
    if (layers != 2) {
        for (int layer = 0; layer < n; ++layer) {
            double sum = 0;
            for (int j = layer; j < n; ++j) sum += outs[j].toObject()["width"].toDouble();
            const double w = std::nearbyint(sum * k * 10) / 10;   // round(x, 1)
            ev << QString("Dialogue: %1,%2,%3,Default,,0,0,0,,{%4\\bord%5\\3c%6}%7")
                      .arg(layer).arg(tstr(ta), tstr(tb), base, num1(w), bgr(outs[layer].toObject()["color"].toString()), text);
        }
    }
    if (layers != 1) ev << QString("Dialogue: %1,%2,%3,Default,,0,0,0,,{%4\\bord0}%5").arg(n).arg(tstr(ta), tstr(tb), base, text);
    return ev;
}


static QString assHead(int W, int H) {
    static const char* head =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: %1\nPlayResY: %2\nWrapStyle: 2\nScaledBorderAndShadow: yes\n\n"
        "[V4+ Styles]\nFormat: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,OutlineColour,BackColour,"
        "Bold,Italic,Underline,StrikeOut,ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,Alignment,"
        "MarginL,MarginR,MarginV,Encoding\n"
        "Style: Default,Yu Gothic UI,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,100,100,0,0,1,0,0,2,0,0,0,1\n\n"
        "[Events]\nFormat: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n";
    return QString(head).arg(W).arg(H);
}

QString buildAss(const Project& p, int W, int H, const std::vector<Timeline::SubPlaced>& telops,
                 const std::vector<Timeline::SubPlaced>& topics) {
    const double k = H / 1080.0;
    QStringList ev;
    for (const auto& pl : telops) {
        ev << telopEv(p, p.telops[pl.spec], W, H, pl.a, pl.b, -1, -1, 0);
    }
    for (const auto& pl : topics) {
        const SubSpec& it = p.topics[pl.spec];
        const QJsonObject st = styleOf(p, "topic");
        const long long size = pyRound(st["size"].toDouble() * k);
        const long long px = pyRound(st["pad_x"].toDouble() * k), py = pyRound(st["pad_y"].toDouble() * k);
        const long long x = pyRound(st["x"].toDouble() * k), y = pyRound(st["y"].toDouble() * k);
        const long long aw = pyRound(st["accent_w"].toDouble(0) * k);
        const double w = std::max(st["min_width"].toDouble() * k, aw + textWidth(it.text, (double)size) + px * 2);
        const long long h = size + py * 2;
        const long long ms = (long long)(it.animDur * 1000);
        const QString fx = it.anim != "none" ? QString("\\fad(%1,%1)").arg(ms) : QString();
        const QString a = tstr(pl.a), b = tstr(pl.b);
        const QString rect = QString("m 0 0 l %1 0 %1 %2 0 %2").arg(pyRound(w)).arg(h);
        ev << QString("Dialogue: 10,%1,%2,Default,,0,0,0,,{%3\\an7\\pos(%4,%5)\\p1\\bord0\\shad0\\1c%6\\1a%7}%8{\\p0}")
                  .arg(a, b, fx).arg(x).arg(y).arg(bgr(st["bar_color"].toString()), alphaTag(st["bar_alpha"].toDouble(1.0)), rect);
        if (aw) {
            const QString acc = QString("m 0 0 l %1 0 %1 %2 0 %2").arg(aw).arg(h);
            ev << QString("Dialogue: 11,%1,%2,Default,,0,0,0,,{%3\\an7\\pos(%4,%5)\\p1\\bord0\\shad0\\1c%6}%7{\\p0}")
                      .arg(a, b, fx).arg(x).arg(y).arg(bgr(st["accent_color"].toString()), acc);
        }
        ev << QString("Dialogue: 12,%1,%2,Default,,0,0,0,,{%3\\an4\\pos(%4,%5)\\fs%6\\b%7\\fn%8\\bord0\\shad0\\1c%9}%10")
                  .arg(a, b, fx).arg(x + aw + px).arg(pyRound(y + h / 2.0)).arg(size).arg(truthy(st["bold"]) ? 1 : 0)
                  .arg(st["font"].toString(), bgr(st["color"].toString()), esc(it.text));
    }
    return assHead(W, H) + ev.join("\n") + "\n";
}

QString buildTelopVariantsAss(const Project& p, int W, int H, const std::vector<TelopVariant>& vars) {
    QStringList ev;
    for (int j = 0; j < (int)vars.size(); ++j) {
        SubSpec s = p.telops[vars[j].spec];
        s.anim = "none";   // 焼くテロップの出入りは fx 側(fx.py の _telop_base は anim=none)
        ev << telopEv(p, s, W, H, j * 0.1, (j + 1) * 0.1, vars[j].hideFrom, vars[j].showOnly, vars[j].layers);
    }
    return assHead(W, H) + ev.join("\n") + "\n";
}

void SubPlan::init(const Project& p, const Timeline& tl) {
    items_.clear();
    ass_.clear();
    if (tl.telopsOut.empty() && tl.topicsOut.empty()) return;
    // needsBake(fx 付きなど)のテロップは Timeline が除いてある(gen="telop" の差し込みとして描く)
    ass_ = buildAss(p, tl.W, tl.H, tl.telopsOut, tl.topicsOut);
    auto add = [&](int id, const Timeline::SubPlaced& pl, int ms) {
        items_.push_back({id, cs(pl.a) * 10, cs(pl.b) * 10, ms, ms});
    };
    for (int i = 0; i < (int)tl.telopsOut.size(); ++i) {
        const SubSpec& s = p.telops[tl.telopsOut[i].spec];
        add(i, tl.telopsOut[i], s.anim == "none" ? 0 : (int)(s.animDur * 1000));
    }
    for (int i = 0; i < (int)tl.topicsOut.size(); ++i) {
        const SubSpec& s = p.topics[tl.topicsOut[i].spec];
        add(100000 + i, tl.topicsOut[i], s.anim == "none" ? 0 : (int)(s.animDur * 1000));
    }
}

long long SubPlan::nowMs(double T) { return (long long)std::floor(T * 1000 + 0.5); }

QString SubPlan::keyAt(long long now) const {
    QString key;
    for (const Item& it : items_) {
        if (now < it.a || now >= it.b) continue;
        key += QString::number(it.id);
        // 出入りの動きの最中は時刻ごとに別の絵。それ以外は要素の組が同じなら同じ絵
        if (it.inMs + it.outMs > 0 && (now < it.a + it.inMs + 1 || now >= it.b - it.outMs - 1)) key += '@' + QString::number(now);
        key += ';';
    }
    return key;
}

}  // namespace vn
