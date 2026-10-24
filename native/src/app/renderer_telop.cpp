// 動きを付けたテロップ(fx.py の Elem のテロップ部分: _telop_imgs / _char_imgs / _animated_telop / type_count)。
// 絵の素は libass(専用の SubRender)で変種ごとに描き、文字アニメーター(animator)は文字ごとの動きを CPU で重ねる。
// 重ねた画面全体の絵は gen="telop" の差し込みとして、通常の fx・mask・変形の経路に流れる。
#include <QPainter>
#include <algorithm>
#include <cmath>
#include "renderer.h"

namespace vn {

namespace {

// premultiplied RGBA の絵(Img)の不透明部分の外枠(画面 px)。無ければ false
bool bboxOf(const SubRender::Img& im, int bb[4]) {
    if (im.img.isNull()) return false;
    int x0 = im.img.width(), y0 = im.img.height(), x1 = -1, y1 = -1;
    for (int y = 0; y < im.img.height(); ++y) {
        const uchar* p = im.img.constScanLine(y);
        int lo = -1, hi = -1;
        for (int x = 0; x < im.img.width(); ++x) if (p[x * 4 + 3]) { lo = x; break; }
        if (lo < 0) continue;
        for (int x = im.img.width() - 1; x >= lo; --x) if (p[x * 4 + 3]) { hi = x; break; }
        x0 = std::min(x0, lo); x1 = std::max(x1, hi);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
    }
    if (x1 < 0) return false;
    bb[0] = im.x + x0; bb[1] = im.y + y0; bb[2] = im.x + x1 + 1; bb[3] = im.y + y1 + 1;
    return true;
}

// Img のうち画面の [x0,x1)×[y0,y1) だけを新しい絵に切り出す(はみ出しは透明)
QImage cropTo(const SubRender::Img& im, int x0, int y0, int x1, int y1) {
    QImage out(x1 - x0, y1 - y0, QImage::Format_RGBA8888_Premultiplied);
    out.fill(0);
    if (im.img.isNull()) return out;
    const int sx0 = std::max(x0, im.x), sx1 = std::min(x1, im.x + im.img.width());
    const int sy0 = std::max(y0, im.y), sy1 = std::min(y1, im.y + im.img.height());
    if (sx1 <= sx0 || sy1 <= sy0) return out;
    for (int y = sy0; y < sy1; ++y)
        memcpy(out.scanLine(y - y0) + (sx0 - x0) * 4, im.img.constScanLine(y - im.y) + (sx0 - im.x) * 4, (size_t)(sx1 - sx0) * 4);
    return out;
}

}  // namespace

void Renderer::initTelops() {
    telop_.clear();
    telopSubs_.reset();
    std::vector<TelopVariant> vars;
    for (int li = 0; li < (int)tl_.layers.size(); ++li) {
        const auto& L = tl_.layers[li];
        if (L.gen != "telop") continue;
        const OverlaySpec& os = proj_.overlays[L.ov];
        if (os.telop < 0 || os.telop >= (int)proj_.telops.size()) continue;
        auto T = std::make_shared<TelopLayer>();
        T->spec = os.telop;
        fx::ElemFx e;
        e.init(os.raw, L.a, L.b);
        T->typer = e.typer;
        const QJsonValue an = os.raw["animator"];
        QJsonArray list;
        if (an.isObject()) list.append(an);
        else if (an.isArray()) list = an.toArray();
        for (const QJsonValue& v : list) {
            if (!v.isObject()) continue;
            bool ok = false;
            QJsonObject P = fx::params("text", v.toObject()["type"].toString(), v.toObject(), &ok);
            if (!ok) continue;
            P["type"] = v.toObject()["type"];
            T->anims.push_back(P);
        }
        T->animator = !T->anims.empty();
        const QList<uint> cp = proj_.telops[T->spec].text.toUcs4();
        for (int i = 0; i < cp.size(); ++i) if (!QChar::isSpace(cp[i])) T->cuts.push_back(i);
        T->slot0 = (int)vars.size();
        if (T->animator) {
            for (int c : T->cuts) {
                vars.push_back({T->spec, -1, c, 1});   // 縁だけ
                vars.push_back({T->spec, -1, c, 2});   // 文字だけ
            }
        } else if (!T->typer.empty()) {
            for (int c : T->cuts) vars.push_back({T->spec, c, -1, 0});
            vars.push_back({T->spec, -1, -1, 0});
        } else {
            vars.push_back({T->spec, -1, -1, 0});
        }
        telop_[li] = T;
    }
    if (!vars.empty()) telopSubs_ = std::make_unique<SubRender>(tl_.W, tl_.H, buildTelopVariantsAss(proj_, tl_.W, tl_.H, vars));
}

// 変種の絵を全部そろえる(最初の 1 回だけ。libass の描画を待つ)
bool Renderer::telopEnsure(int li) {
    auto it = telop_.find(li);
    if (it == telop_.end()) return false;
    TelopLayer& T = *it->second;
    if (T.ready) return true;
    std::lock_guard<std::mutex> l(T.mu);
    if (T.ready) return true;
    const int n = (int)T.cuts.size();
    auto fetch = [&](int slot, SubRender::Img* out) {
        const int j = T.slot0 + slot;
        if (!telopSubs_ || !telopSubs_->get(QString("v%1").arg(j), (long long)j * 100 + 50, out)) *out = SubRender::Img();
    };
    if (T.animator) {
        T.chars.assign(n, TelopLayer::Ch());
        for (int c = 0; c < n; ++c) {
            SubRender::Img o, f;
            fetch(2 * c, &o);
            fetch(2 * c + 1, &f);
            int bb[4];
            if (!bboxOf(o, bb) && !bboxOf(f, bb)) continue;
            TelopLayer::Ch& ch = T.chars[c];
            ch.ok = true;
            ch.x0 = bb[0]; ch.y0 = bb[1];
            ch.o = cropTo(o, bb[0], bb[1], bb[2], bb[3]);
            ch.f = cropTo(f, bb[0], bb[1], bb[2], bb[3]);
        }
    } else {
        const int cnt = T.typer.empty() ? 1 : n + 1;
        T.var.assign(cnt, SubRender::Img());
        for (int s = 0; s < cnt; ++s) fetch(s, &T.var[s]);
        T.hasBB = bboxOf(T.var[cnt - 1], T.bb);
    }
    T.ready = true;
    return true;
}

// その時刻の絵の種類: -1 = 1 枚だけ、typewriter = 見せる文字数、animator = コマの番号
long long Renderer::telopFk(int li, double tau) const {
    auto it = telop_.find(li);
    if (it == telop_.end()) return -1;
    const TelopLayer& T = *it->second;
    if (T.animator) return (long long)std::nearbyint(tau * tl_.fps);
    if (T.typer.empty()) return -1;
    const auto& L = tl_.layers[li];
    const int n = (int)T.cuts.size();
    const double len = L.b - L.a;
    int k = n;
    for (const auto& [in, iv] : T.typer) {
        const double q = in ? tau : len - tau;
        k = std::min(k, (int)(q / iv + 1e-6) + (in ? 1 : 0));
    }
    return std::max(0, std::min(n, k));
}

QImage Renderer::telopImage(int li, double tau) {
    QImage out(tl_.W, tl_.H, QImage::Format_RGBA8888_Premultiplied);
    out.fill(0);
    auto it = telop_.find(li);
    if (it == telop_.end() || !telopEnsure(li)) return out.convertToFormat(QImage::Format_RGBA8888);
    TelopLayer& T = *it->second;
    if (!T.animator) {
        const long long fk = telopFk(li, tau);
        const SubRender::Img& im = T.var[std::max<long long>(0, std::min<long long>(fk < 0 ? 0 : fk, (long long)T.var.size() - 1))];
        if (!im.img.isNull()) {
            const int x0 = std::max(0, im.x), x1 = std::min(tl_.W, im.x + im.img.width());
            const int y0 = std::max(0, im.y), y1 = std::min(tl_.H, im.y + im.img.height());
            for (int y = y0; y < y1 && x1 > x0; ++y)
                memcpy(out.scanLine(y) + x0 * 4, im.img.constScanLine(y - im.y) + (x0 - im.x) * 4, (size_t)(x1 - x0) * 4);
        }
        return out.convertToFormat(QImage::Format_RGBA8888);
    }
    // 文字アニメーター(fx.py _animated_telop): 文字ごとの動きを出して、縁を全部 → 文字を全部の順に重ねる
    const auto& L = tl_.layers[li];
    const double len = L.b - L.a, k = tl_.H / 1080.0;
    const int n = (int)T.chars.size();
    std::vector<fx::St> sts(n);
    for (const QJsonObject& P : T.anims) {
        const std::vector<int> rank = fx::charOrder(n, P["order"].toString());
        const double dur = std::max(1e-3, fx::num(P["dur"]));
        const bool outFlag = fx::truthy(P["out"]);
        for (int i = 0; i < n; ++i) {
            const double d0 = fx::num(P["stagger"]) * rank[i];
            double u = std::min(1.0, std::max(0.0, (tau - d0) / dur));
            if (outFlag) u = std::min(u, std::max(0.0, (len - tau - d0) / dur));
            fx::textAnimState(P["type"].toString(), P, &sts[i], u, tau, i, n, k, tl_.fps);
        }
    }
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (int layer = 0; layer < 2; ++layer) {
        for (int i = 0; i < n; ++i) {
            const TelopLayer::Ch& ch = T.chars[i];
            const fx::St& st = sts[i];
            if (!ch.ok || st.op <= 0.002 || st.sx <= 0.002 || st.sy <= 0.002) continue;
            const QImage& img = layer ? ch.f : ch.o;
            const double w0 = img.width(), h0 = img.height();
            const double cx = ch.x0 + w0 / 2 + st.dx, cy = ch.y0 + h0 / 2 + st.dy;
            p.setOpacity(std::min(1.0, st.op));
            if (st.rot == 0) {
                const int dw = std::max(1, (int)std::nearbyint(w0 * st.sx)), dh = std::max(1, (int)std::nearbyint(h0 * st.sy));
                const int x = (int)std::nearbyint(cx - dw / 2.0), y = (int)std::nearbyint(cy - dh / 2.0);
                if (dw == img.width() && dh == img.height()) p.drawImage(x, y, img);
                else p.drawImage(QRect(x, y, dw, dh), img);
            } else {
                QTransform t;
                t.translate(cx, cy);
                t.rotate(st.rot);
                t.scale(st.sx, st.sy);
                t.translate(-w0 / 2, -h0 / 2);
                p.setTransform(t);
                p.drawImage(0, 0, img);
                p.resetTransform();
            }
        }
    }
    p.end();
    return out.convertToFormat(QImage::Format_RGBA8888);
}

// anchor の基準の外枠。通常は全部出した時の外枠(Python の anchor())、animator はそのコマの外枠。取れない時は false(画面全体を使う)
bool Renderer::telopBox(int li, bool wait, double* bx, double* by, double* bw, double* bh) {
    auto it = telop_.find(li);
    if (it == telop_.end()) return false;
    TelopLayer& T = *it->second;
    const auto& L = tl_.layers[li];
    if (T.animator) {
        const double tau = playhead_.load() - L.a;
        const QString key = QString("gen|%1|%2").arg(li).arg(telopFk(li, tau));
        const unsigned tex = genTex(key, li, tau, wait);
        float bb[4];
        if (!tex || !texBBox(tex, bb)) return false;
        *bx = bb[0] * tl_.W; *by = bb[1] * tl_.H; *bw = (bb[2] - bb[0]) * tl_.W; *bh = (bb[3] - bb[1]) * tl_.H;
        return bb[2] > bb[0] && bb[3] > bb[1];
    }
    if (!T.ready && !(wait && telopEnsure(li))) return false;
    if (!T.hasBB) return false;
    *bx = T.bb[0]; *by = T.bb[1]; *bw = T.bb[2] - T.bb[0]; *bh = T.bb[3] - T.bb[1];
    return true;
}

}  // namespace vn
