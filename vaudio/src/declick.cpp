// vedit/declick.py の移植(numpy 相当の演算順を保つ)。polyfit は列正規化つき最小二乗(Householder QR)。
#include "declick.h"

#include <algorithm>
#include <cmath>

namespace vaudio {

namespace {

constexpr int CONTEXT = 12;

// np.polyfit(u, y, 3) 相当。coef は高次から (c3,c2,c1,c0)。
bool polyfit3(const std::vector<double>& u, const std::vector<double>& y, double coef[4]) {
    const int m = (int)u.size(), n = 4;
    std::vector<double> A((size_t)m * n);  // 列優先ではなく行優先 A[r*4+c]、c = 次数(昇順)
    for (int r = 0; r < m; r++) {
        double p = 1.0;
        for (int c = 0; c < n; c++) { A[(size_t)r * n + c] = p; p *= u[(size_t)r]; }
    }
    // numpy は x**k ではなく vander(x, 4) を使う(同値)。列ノルムで正規化
    double sc[4];
    for (int c = 0; c < n; c++) {
        double s = 0;
        for (int r = 0; r < m; r++) s += A[(size_t)r * n + c] * A[(size_t)r * n + c];
        sc[c] = std::sqrt(s);
        if (sc[c] == 0) return false;
        for (int r = 0; r < m; r++) A[(size_t)r * n + c] /= sc[c];
    }
    std::vector<double> b(y);
    for (int k = 0; k < n; k++) {
        double nrm = 0;
        for (int r = k; r < m; r++) nrm += A[(size_t)r * n + k] * A[(size_t)r * n + k];
        nrm = std::sqrt(nrm);
        if (nrm == 0) return false;
        double alpha = A[(size_t)k * n + k] > 0 ? -nrm : nrm;
        std::vector<double> v((size_t)m, 0.0);
        for (int r = k; r < m; r++) v[(size_t)r] = A[(size_t)r * n + k];
        v[(size_t)k] -= alpha;
        double vn = 0;
        for (int r = k; r < m; r++) vn += v[(size_t)r] * v[(size_t)r];
        if (vn == 0) continue;
        for (int c = k; c < n; c++) {
            double d = 0;
            for (int r = k; r < m; r++) d += v[(size_t)r] * A[(size_t)r * n + c];
            d = 2 * d / vn;
            for (int r = k; r < m; r++) A[(size_t)r * n + c] -= d * v[(size_t)r];
        }
        double d = 0;
        for (int r = k; r < m; r++) d += v[(size_t)r] * b[(size_t)r];
        d = 2 * d / vn;
        for (int r = k; r < m; r++) b[(size_t)r] -= d * v[(size_t)r];
    }
    double c_[4];
    for (int k = n - 1; k >= 0; k--) {
        double s = b[(size_t)k];
        for (int c = k + 1; c < n; c++) s -= A[(size_t)k * n + c] * c_[c];
        double diag = A[(size_t)k * n + k];
        if (std::fabs(diag) < 1e-12) return false;
        c_[k] = s / diag;
    }
    for (int k = 0; k < n; k++) coef[3 - k] = c_[k] / sc[k];
    return true;
}

void repair(std::vector<double>& x, long a, long b) {
    long N = (long)x.size();
    long lo = std::max(0L, a - CONTEXT), hi = std::min(N, b + CONTEXT);
    std::vector<long> idx;
    for (long k = lo; k < a; k++) idx.push_back(k);
    for (long k = b; k < hi; k++) idx.push_back(k);
    if (idx.size() < 6) return;
    double t0 = (double)(a + b) / 2.0;
    double scale = std::max(1.0, (double)(hi - lo) / 2.0);
    std::vector<double> u(idx.size()), y(idx.size());
    for (size_t k = 0; k < idx.size(); k++) {
        u[k] = ((double)idx[k] - t0) / scale;
        y[k] = x[(size_t)idx[k]];
    }
    double c[4];
    if (!polyfit3(u, y, c)) return;
    for (long k = a; k < b; k++) {
        double t = ((double)k - t0) / scale;
        double v = c[0];
        v = v * t + c[1];
        v = v * t + c[2];
        v = v * t + c[3];
        x[(size_t)k] = v;
    }
}

int declickChannel(std::vector<double>& x, int sr, const DeclickParams& p) {
    const long N = (long)x.size();
    if (N < 64) return 0;
    // d2 = |diff(x,2,prepend=x[0],append=x[-1])|[1:-1] を両端複製で N 個に
    std::vector<double> z((size_t)N + 2);
    z[0] = x[0];
    for (long k = 0; k < N; k++) z[(size_t)k + 1] = x[(size_t)k];
    z[(size_t)N + 1] = x[(size_t)N - 1];
    std::vector<double> D((size_t)N);
    for (long k = 0; k < N; k++) {
        double d1a = z[(size_t)k + 1] - z[(size_t)k];
        double d1b = z[(size_t)k + 2] - z[(size_t)k + 1];
        D[(size_t)k] = std::fabs(d1b - d1a);
    }
    std::vector<double> d2((size_t)N);
    d2[0] = D[1];
    for (long k = 1; k <= N - 2; k++) d2[(size_t)k] = D[(size_t)k];
    d2[(size_t)N - 1] = D[(size_t)N - 2];

    long win = std::max(8L, (long)(sr * p.window_ms / 1000));
    std::vector<double> cs((size_t)N + 1);
    auto cumsum = [&](const std::vector<double>& v) {
        cs[0] = 0.0;
        double s = 0.0;
        for (long k = 0; k < N; k++) { s += v[(size_t)k]; cs[(size_t)k + 1] = s; }
    };
    auto around = [&](std::vector<double>& out) {
        out.resize((size_t)N);
        for (long i = 0; i < N; i++) {
            long lo = std::max(0L, i - win), hi = std::min(N, i + 1 + win);
            double left = (cs[(size_t)i] - cs[(size_t)lo]) / (double)std::max(1L, i - lo);
            double right = (cs[(size_t)hi] - cs[(size_t)i + 1]) / (double)std::max(1L, hi - (i + 1));
            out[(size_t)i] = std::max(left, right) + 1e-6;
        }
    };
    std::vector<double> lvl, cap((size_t)N);
    cumsum(d2);
    around(lvl);
    for (long k = 0; k < N; k++) cap[(size_t)k] = std::min(d2[(size_t)k], 3 * lvl[(size_t)k]);
    cumsum(cap);
    around(lvl);
    std::vector<char> hit((size_t)N);
    for (long k = 0; k < N; k++) hit[(size_t)k] = (d2[(size_t)k] > p.threshold * lvl[(size_t)k]) && (d2[(size_t)k] > 1e-4);

    long w = (long)(sr * p.widen_ms / 1000);
    if (w) {
        std::vector<long> pre((size_t)N + 1, 0);
        for (long k = 0; k < N; k++) pre[(size_t)k + 1] = pre[(size_t)k] + hit[(size_t)k];
        std::vector<char> h2((size_t)N);
        for (long k = 0; k < N; k++) {
            long a = std::max(0L, k - w), b = std::min(N, k + w + 1);
            h2[(size_t)k] = pre[(size_t)b] - pre[(size_t)a] > 0;
        }
        hit.swap(h2);
    }
    long maxLen = (long)(sr * p.max_ms / 1000);
    int fixed = 0;
    long k = 0;
    while (k < N) {
        if (!hit[(size_t)k]) { k++; continue; }
        long a = k;
        while (k < N && hit[(size_t)k]) k++;
        long b = k;
        if (b - a <= maxLen) { repair(x, a, b); fixed++; }
    }
    return fixed;
}

}  // namespace

DeclickParams declickParamsFrom(const vj::Value& v) {
    DeclickParams p;
    if (v.isObj()) {
        p.threshold = v.getNum("threshold", p.threshold);
        p.widen_ms = v.getNum("widen_ms", p.widen_ms);
        p.max_ms = v.getNum("max_ms", p.max_ms);
        p.window_ms = v.getNum("window_ms", p.window_ms);
    }
    return p;
}

Audio declick(const Audio& audio, int sr, const DeclickParams& p, int& clicks) {
    Audio out(audio.size());
    for (size_t c = 0; c < audio.size(); c++) {
        std::vector<double> x(audio[c].begin(), audio[c].end());
        clicks += declickChannel(x, sr, p);
        out[c].resize(x.size());
        for (size_t k = 0; k < x.size(); k++) out[c][k] = (float)x[k];
    }
    return out;
}

}  // namespace vaudio
