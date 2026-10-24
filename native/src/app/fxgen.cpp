#include "fxgen.h"
#include <QPainter>
#include <QPolygonF>
#include <algorithm>
#include <cmath>
#include <vector>
#include "core/fxcore.h"
#include "core/pyrand.h"

namespace vn::fxgen {

static void rgbOf(const QJsonValue& v, const int def[3], int out[3]) { fx::parseColor(v, out, def); }

// RGBA8888 の4チャンネルを別々に(PIL の GaussianBlur と同じ扱い)ぼかす。縁は端の値で延ばす
static void gaussBlur(QImage& im, double sigma) {
    if (sigma < 0.05) return;
    const int W = im.width(), H = im.height();
    const int R = std::max(1, (int)std::ceil(sigma * 3));
    std::vector<float> k(2 * R + 1);
    float sum = 0;
    for (int i = -R; i <= R; ++i) sum += (k[i + R] = std::exp(-0.5f * i * i / (float)(sigma * sigma)));
    for (auto& v : k) v /= sum;
    std::vector<float> tmp((size_t)W * H * 4);
    for (int y = 0; y < H; ++y) {
        const uchar* row = im.constScanLine(y);
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 4; ++c) {
                float a = 0;
                for (int i = -R; i <= R; ++i) {
                    const int xx = std::min(W - 1, std::max(0, x + i));
                    a += k[i + R] * row[xx * 4 + c];
                }
                tmp[((size_t)y * W + x) * 4 + c] = a;
            }
    }
    for (int y = 0; y < H; ++y) {
        uchar* row = im.scanLine(y);
        for (int x = 0; x < W; ++x)
            for (int c = 0; c < 4; ++c) {
                float a = 0;
                for (int i = -R; i <= R; ++i) {
                    const int yy = std::min(H - 1, std::max(0, y + i));
                    a += k[i + R] * tmp[((size_t)yy * W + x) * 4 + c];
                }
                row[x * 4 + c] = (uchar)std::min(255.f, std::max(0.f, a + 0.5f));
            }
    }
}

QImage speedlines(int W, int H, double fps, const QJsonObject& P, double tau) {
    const double k = H / 1080.0;
    long long seed = (long long)fx::num(P["seed"]);
    if (fx::truthy(P["animate"])) {
        const long long every = std::max(1LL, (long long)fx::num(P["every"]));
        const long long fr = (long long)std::nearbyint(tau * fps);
        seed += (long long)std::floor((double)fr / every);
    }
    PyRandom rnd(seed);
    const int n = (int)fx::num(P["count"]);
    const double inner = fx::num(P["inner"]);
    int col[3];
    const int white[3] = {255, 255, 255};
    rgbOf(P["color"], white, col);
    QImage im(W, H, QImage::Format_ARGB32_Premultiplied);
    im.fill(Qt::transparent);
    {
        QPainter p(&im);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setCompositionMode(QPainter::CompositionMode_Source);   // PIL の ImageDraw は混ぜずに上書き
        p.setPen(Qt::NoPen);
        const double cx = W / 2.0, cy = H / 2.0, R = std::hypot((double)W, (double)H) / 2 + 20 * k;
        for (int i = 0; i < n; ++i) {
            const double a = 2 * M_PI * (i + rnd.uniform(-0.4, 0.4)) / n;
            const double r0 = R * rnd.uniform(inner, inner + 0.22);
            const double wd = rnd.uniform(0.006, 0.016);
            QPolygonF poly;
            poly << QPointF(cx + R * std::cos(a - wd), cy + R * std::sin(a - wd)) << QPointF(cx + R * std::cos(a + wd), cy + R * std::sin(a + wd))
                 << QPointF(cx + r0 * std::cos(a), cy + r0 * std::sin(a));
            const int al = (int)rnd.randint(170, 240);
            p.setBrush(QColor(col[0], col[1], col[2], al));
            p.drawPolygon(poly);
        }
    }
    QImage out = im.convertToFormat(QImage::Format_RGBA8888);   // 掛け前に戻す
    const double bl = fx::num(P["blur"]);
    if (bl > 0) gaussBlur(out, bl * k);
    return out;
}

QImage gloom(int W, int H, const QJsonObject& P, double tau) {
    const double k = H / 1080.0;
    PyRandom rnd((long long)fx::num(P["seed"]));
    int bg[3], lc[3];
    const int dbg[3] = {8, 6, 30}, dlc[3] = {150, 140, 255};
    rgbOf(P["bg"], dbg, bg);
    rgbOf(P["line_color"], dlc, lc);
    const int dark = (int)(fx::num(P["dark"]) * 255);
    const int lineA = (int)fx::num(P["line_alpha"]);
    const double growT = fx::num(P["grow"]);
    const double g = growT <= 0 ? 1.0 : fx::smooth(tau / growT);
    QImage ln(W, H, QImage::Format_RGBA8888);
    ln.fill(Qt::transparent);
    double x = 20 * k;
    while (x < W) {
        const double L = rnd.uniform(0.18, 0.42) * H * g;
        static const int ws[3] = {3, 4, 6};
        const double w = ws[rnd.randbelow(3)] * k;
        if (L > 0) {
            const int x0 = (int)x, x1 = std::min(W - 1, (int)(x + w)), y1 = std::min(H - 1, (int)L);
            for (int y = 0; y <= y1; ++y) {
                uchar* row = ln.scanLine(y);
                for (int xx = std::max(0, x0); xx <= x1; ++xx) {
                    row[xx * 4] = (uchar)lc[0]; row[xx * 4 + 1] = (uchar)lc[1]; row[xx * 4 + 2] = (uchar)lc[2]; row[xx * 4 + 3] = (uchar)lineA;
                }
            }
        }
        x += rnd.randint(26, 60) * k;
    }
    gaussBlur(ln, 1 * k);
    QImage out(W, H, QImage::Format_RGBA8888);
    for (int y = 0; y < H; ++y) {
        const float grad = std::pow(H > 1 ? 1.0f - (float)y / (H - 1) : 1.0f, 0.7f);   // 線は下へ行くほど薄く
        const uchar* s = ln.constScanLine(y);
        uchar* d = out.scanLine(y);
        for (int xx = 0; xx < W; ++xx) {
            const float sa = (uchar)(s[xx * 4 + 3] * grad) / 255.f, da = dark / 255.f;
            const float oa = sa + da * (1 - sa);
            for (int c = 0; c < 3; ++c) {
                const float v = oa > 1e-5f ? (s[xx * 4 + c] / 255.f * sa + bg[c] / 255.f * da * (1 - sa)) / oa : 0.f;
                d[xx * 4 + c] = (uchar)std::min(255.f, std::max(0.f, v * 255 + 0.5f));
            }
            d[xx * 4 + 3] = (uchar)std::min(255.f, std::max(0.f, oa * 255 + 0.5f));
        }
    }
    return out;
}

}  // namespace vn::fxgen
