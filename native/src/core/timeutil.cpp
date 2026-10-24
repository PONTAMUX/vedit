#include "timeutil.h"
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <map>

namespace vn {

bool parseTime(const QJsonValue& v, double* out) {
    if (v.isDouble()) { *out = v.toDouble(); return true; }
    if (!v.isString()) return false;
    const QString s = v.toString().trimmed();
    static const QRegularExpression num(QStringLiteral("^\d+(\.\d+)?$"));
    bool ok = false;
    if (num.match(s).hasMatch()) { *out = s.toDouble(&ok); return ok; }
    const QStringList parts = s.split(':');
    if (parts.size() < 2 || parts.size() > 3) return false;
    double sec = 0;
    for (const QString& p : parts) {
        double x = p.toDouble(&ok);
        if (!ok) return false;
        sec = sec * 60 + x;
    }
    *out = sec;
    return true;
}

double parseTimeOr(const QJsonValue& v, double dflt) {
    double r;
    return parseTime(v, &r) ? r : dflt;
}

TimeMap::TimeMap(double dur, const Ranges& cutsIn, const Ranges& gapsIn) {
    duration = dur;
    Ranges c;
    for (auto [a, b] : cutsIn) c.push_back({std::max(0.0, a), std::min(dur, b)});
    std::sort(c.begin(), c.end());
    for (auto [a, b] : c) {
        if (b <= a) continue;
        if (!cuts.empty() && a <= cuts.back().second) cuts.back().second = std::max(cuts.back().second, b);
        else cuts.push_back({a, b});
    }
    double pos = 0;
    for (auto [a, b] : cuts) {
        if (a - pos >= MIN_KEEP) keeps.push_back({pos, a});
        pos = b;
    }
    if (dur - pos >= MIN_KEEP) keeps.push_back({pos, dur});
    // 空白 = 元時間 at の直前に dur 秒を挿入。カット内の at はカットの終わりへ寄せる
    std::map<double, double> gm;
    for (auto [at0, d] : gapsIn) {
        double at = std::min(std::max(0.0, at0), dur);
        if (d < MIN_KEEP) continue;
        for (auto [a, b] : cuts)
            if (a <= at && at < b) at = b;
        at = std::nearbyint(at * 1e4) / 1e4;
        gm[at] += d;
    }
    gaps.assign(gm.begin(), gm.end());
    total = 0;
    for (auto [a, b] : keeps) total += b - a;
    for (auto [g, d] : gaps) total += d;
}

std::vector<TimeMap::Piece> TimeMap::pieces() const {
    std::vector<Piece> out;
    size_t gi = 0;
    for (auto [a, b] : keeps) {
        while (gi < gaps.size() && gaps[gi].first <= a + 1e-6) { out.push_back({true, gaps[gi].second, 0}); ++gi; }
        while (gi < gaps.size() && gaps[gi].first < b - 1e-6) {
            out.push_back({false, a, gaps[gi].first});
            a = gaps[gi].first;
            out.push_back({true, gaps[gi].second, 0});
            ++gi;
        }
        if (b - a >= 1e-4) out.push_back({false, a, b});
    }
    while (gi < gaps.size()) { out.push_back({true, gaps[gi].second, 0}); ++gi; }
    return out;
}

double TimeMap::out(double t) const {
    double acc = 0;
    for (auto [a, b] : keeps) {
        if (t <= a) break;
        acc += std::min(t, b) - a;
    }
    for (auto [at, d] : gaps)
        if (at <= t + 1e-9) acc += d;
    return acc;
}

double TimeMap::outEnd(double t) const {
    double r = out(t);
    for (auto [at, d] : gaps)
        if (std::fabs(at - t) <= 1e-6) r -= d;
    return r;
}

bool TimeMap::outRange(double a, double b, double* oa, double* ob) const {
    *oa = out(a);
    *ob = outEnd(b);
    return *ob - *oa > 1e-3;
}

bool TimeMap::gapAt(double at0, double* outStart, double* len) const {
    double at = std::min(std::max(0.0, at0), duration);
    for (auto [a, b] : cuts)
        if (a <= at && at < b) at = b;
    at = std::nearbyint(at * 1e4) / 1e4;
    for (auto [g, d] : gaps)
        if (std::fabs(g - at) < 1e-3) { *outStart = out(g) - d; *len = d; return true; }
    return false;
}

}  // namespace vn
