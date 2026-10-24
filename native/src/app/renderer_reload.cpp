// 編集中の内容の反映(Renderer::reload)。保存して読み直すのではなく、JSON のバイト列から新しい Project / Timeline を作り、
// 古い状態のうち使い回せるものを引き継ぐ:
//   - 本編のデコーダ(slots_)は元素材が同じなら残し、区間の番号だけ付け替える
//   - 差し込み動画のデコーダ(ovs_)は同じファイルのものを新しい層へ引き継ぐ(NVDEC の作り直しを避ける)
//   - 画像のテクスチャは画像ファイル+大きさがキーなのでそのまま、生成要素・エフェクトの計算・外部 project の comp は
//     その層の指定が同じ(署名が同じ)ものだけ残す
//   - ASS(字幕)・動きを付けたテロップは中身が同じなら libass の描画結果ごと残す
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <cmath>
#include "renderer.h"

namespace vn {

void reapStream(std::unique_ptr<VideoStream> s);   // renderer.cpp

namespace {
void reapLater(std::unique_ptr<VideoStream> s) { reapStream(std::move(s)); }   // join で描画を止めない片付け(renderer.cpp)

QString jsonSig(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

// 層の指定が変わったかどうかの署名。この層のキャッシュ(生成した絵・エフェクトの計算)を残してよいかの判定に使う
std::vector<QString> layerSigs(const Project& p, const Timeline& tl) {
    std::vector<QString> v;
    v.reserve(tl.layers.size());
    for (const auto& L : tl.layers) {
        QString s = jsonSig(p.overlays[L.ov].raw);
        s += QString("|%1|%2|%3|%4|%5|%6|%7|%8").arg(L.a, 0, 'f', 6).arg(L.b, 0, 'f', 6).arg(L.gen, L.path).arg(tl.W).arg(tl.H).arg(tl.fps, 0, 'f', 4).arg(L.sub ? 1 : 0);
        v.push_back(s);
    }
    return v;
}

// 動きを付けたテロップ(gen="telop")の署名: 層の指定+字幕の指定+styles
QString telopSignature(const Project& p, const Timeline& tl, const std::vector<QString>& sigs) {
    QString s = jsonSig(p.styles) + QString("|%1x%2").arg(tl.W).arg(tl.H);
    for (int li = 0; li < (int)tl.layers.size(); ++li) {
        const auto& L = tl.layers[li];
        if (L.gen != "telop") continue;
        const OverlaySpec& os = p.overlays[L.ov];
        s += QString("#%1:").arg(li) + sigs[li];
        if (os.telop >= 0 && os.telop < (int)p.telops.size()) {
            const SubSpec& t = p.telops[os.telop];
            s += "|" + t.text + "|" + t.style + "|" + jsonSig(t.raw);
        }
    }
    return s;
}

// 音に関わる指定の署名(A1 のカット割り・amute/aslip・効果音・BGM・差し込みの音)。変わった時だけ AudioPlayer を作り直す
QString audioSignature(const Project& p, const Timeline& tl) {
    QString s = QString("%1|%2|%3|%4").arg(p.source).arg(tl.total, 0, 'f', 6).arg(p.audioFadeMs).arg(p.cacheDir());
    for (const auto& pc : tl.pieces) s += QString("|p%1,%2,%3,%4").arg(pc.gap).arg(pc.a, 0, 'f', 6).arg(pc.b, 0, 'f', 6).arg(pc.o0, 0, 'f', 6);
    for (auto [a, b] : p.amute) s += QString("|m%1,%2").arg(a, 0, 'f', 6).arg(b, 0, 'f', 6);
    for (const auto& k : p.aslip) s += QString("|s%1,%2,%3").arg(k.a, 0, 'f', 6).arg(k.b, 0, 'f', 6).arg(k.d, 0, 'f', 6);
    for (const auto& e : p.sfx) s += QString("|e%1,%2,%3,%4,%5,%6,%7").arg(e.file).arg(e.start, 0, 'f', 6).arg(e.end, 0, 'f', 6).arg(e.in, 0, 'f', 6).arg(e.gainDb).arg(e.hasEnd).arg(e.hasGapAt ? e.gapAt : -1.0, 0, 'f', 6);
    s += QString("|b%1,%2,%3,%4").arg(p.bgm.on).arg(p.bgm.file).arg(p.bgm.volumeDb).arg(p.bgm.duck);
    QStringList mu(p.mute.begin(), p.mute.end());
    mu.sort();
    s += "|M" + mu.join(",");
    for (const auto& L : tl.layers)
        if (p.overlays[L.ov].audio) s += QString("|o%1,%2,%3,%4").arg(L.path).arg(L.a, 0, 'f', 6).arg(L.b, 0, 'f', 6).arg(p.overlays[L.ov].volumeDb);
    return s;
}

int keyLayer(const QString& key) {   // "gen|<li>|<fk>" の li。違えば -1
    if (!key.startsWith("gen|")) return -1;
    const int a = 4, b = (int)key.indexOf('|', a);
    return b < 0 ? -1 : key.mid(a, b - a).toInt();
}
}  // namespace

bool Renderer::applyLive() {
    if (!dirty_.exchange(false) || !live_) return false;
    QString err;
    if (!reload(live_(), &err)) { qWarning("reload: %s", qPrintable(err)); return false; }
    return true;
}

bool Renderer::reload(const QByteArray& json, QString* err) {
    QElapsedTimer clk;
    clk.start();
    Project np;
    if (!Project::loadBytes(pathOverride_.isEmpty() ? proj_.path : pathOverride_, json, &np)) { if (err) *err = np.error; return false; }
    Timeline nt;
    if (!nt.init(np, [](const QString& p) { return probeMedia(p); })) { if (err) *err = nt.error; return false; }
    if (!glOk_) {   // GL を始める前なら入れ替えるだけ
        proj_ = std::move(np);
        tl_ = std::move(nt);
        srcPath_ = proj_.resolve(proj_.source);
        openedDecode_ = decodePath();
        return true;
    }

    // 裏の絵の生成(makeGenImage は proj_ / tl_ を読む)のうち、走っている分だけ終わるのを待つ。まだ順番待ちの分は世代を進めて捨てる
    // (前は順番待ちの画像読み込みまで全部待っていて、テロップの多い案件で Q/E のたびに 10 秒ほど固まった)
    {
        QMutexLocker l(&imgMu_);
        ++genEpoch_;
        for (const auto& k : std::as_const(genPending_)) pending_.remove(k);
        genPending_.clear();
    }
    while (genRunning_.load() > 0) QThread::msleep(1);
    stopProber(false);   // 先読みは古い一覧の写しで動くので待たない(前は join で 10 秒ほど固まった)

    // ---- 古い状態の控え
    const std::vector<QString> oldSig = layerSigs(proj_, tl_);
    std::vector<QString> oldGen;
    for (const auto& L : tl_.layers) oldGen.push_back(L.gen);
    const std::vector<Timeline::PieceInfo> oldPieces = tl_.pieces;
    const std::vector<Timeline::Trans> oldTrans = tl_.trans;
    const QString oldAss = subPlan_.ass();
    const QString oldAudio = audioSignature(proj_, tl_);
    const bool hadSubs = (bool)subs_;
    std::vector<QString> oldPath;
    for (const auto& L : tl_.layers) oldPath.push_back(L.path);
    const std::string oldDecode = openedDecode_;
    const double oldSrcFps = tl_.src.fps;
    const int oldW = tl_.W, oldH = tl_.H;

    // ---- 入れ替え
    proj_ = std::move(np);
    tl_ = std::move(nt);
    srcPath_ = proj_.resolve(proj_.source);
    const std::vector<QString> newSig = layerSigs(proj_, tl_);
    const int nLayers = (int)tl_.layers.size();
    auto same = [&](int li) { return li >= 0 && li < nLayers && li < (int)oldSig.size() && oldSig[li] == newSig[li]; };

    // ---- 大きさが変わった: 作業用の絵(FBO・キャンバス)を作り直す
    if (tl_.W != oldW || tl_.H != oldH) {
        postRelease();
        fbo_ = std::make_unique<QOpenGLFramebufferObject>(tl_.W, tl_.H);
        glOk_ = fbo_->isValid();
        if (glOk_ && !postInit()) qWarning("post pipeline unavailable");
    }

    // ---- 本編のデコーダ: 元素材が同じなら残し、区間の番号を新しい並びに付け替える(同じ区間 = 元素材の a が同じ)
    if (decodePath() != oldDecode || std::fabs(tl_.src.fps - oldSrcFps) > 1e-6) {
        slots_.clear();
        freeVideo(base_);
        for (auto& o : tsrc_) { freeVideo(o.gpu); reapLater(std::move(o.s)); }
        tsIdx_[0] = tsIdx_[1] = -1;
        openedDecode_ = decodePath();
    } else {
        std::vector<char> taken(tl_.pieces.size(), 0);
        for (auto& s : slots_) {
            int np2 = -1;
            if (s.piece >= 0 && s.piece < (int)oldPieces.size() && !oldPieces[s.piece].gap) {
                for (int k = 0; k < (int)tl_.pieces.size(); ++k)
                    if (!tl_.pieces[k].gap && !taken[k] && std::fabs(tl_.pieces[k].a - oldPieces[s.piece].a) < 1e-4) { np2 = k; break; }
            }
            if (np2 >= 0) taken[np2] = 1;
            s.piece = np2;
        }
        // 前後の映像を使うトランジション: 同じ並びなら続ける
        bool sameTrans = tl_.trans.size() == oldTrans.size();
        for (size_t i = 0; sameTrans && i < oldTrans.size(); ++i) {
            const auto &a = oldTrans[i], &b = tl_.trans[i];
            sameTrans = a.src == b.src && a.hasA == b.hasA && a.hasB == b.hasB && std::fabs(a.o - b.o) < 1e-6 && std::fabs(a.jA - b.jA) < 1e-6 && std::fabs(a.jB - b.jB) < 1e-6;
        }
        if (!sameTrans) tsIdx_[0] = tsIdx_[1] = -1;   // 使う時にその位置で開き直す
    }

    // ---- 差し込み動画のデコーダ: 同じファイルの層へ引き継ぐ(前と同じ番号を優先)
    {
        std::map<int, OvStream> old = std::move(ovs_);
        ovs_.clear();
        std::vector<std::pair<int, OvStream*>> pool;
        for (auto& [li, o] : old) pool.push_back({li, &o});
        auto take = [&](int li, bool sameIndexOnly) -> bool {
            const QString& path = tl_.layers[li].path;
            for (auto& pr : pool) {
                if (!pr.second || !pr.second->s) continue;
                if (sameIndexOnly && pr.first != li) continue;
                if (pr.first < (int)oldPath.size() && oldPath[pr.first] == path) {
                    ovs_[li] = std::move(*pr.second);
                    pr.second = nullptr;
                    return true;
                }
            }
            return false;
        };
        for (int li = 0; li < nLayers; ++li) if (!tl_.layers[li].path.isEmpty() && !tl_.layers[li].isImage) take(li, true);
        for (int li = 0; li < nLayers; ++li) if (!tl_.layers[li].path.isEmpty() && !tl_.layers[li].isImage && !ovs_.count(li)) take(li, false);
        for (auto& pr : pool) if (pr.second) { freeVideo(pr.second->gpu); reapLater(std::move(pr.second->s)); }
    }

    // ---- 層ごとのキャッシュ: 指定が変わった層の分だけ捨てる
    for (auto it = efx_.begin(); it != efx_.end();) it = same(it->first) ? std::next(it) : efx_.erase(it);
    for (auto it = lastGen_.begin(); it != lastGen_.end();) it = same(it->first) ? std::next(it) : lastGen_.erase(it);
    for (auto it = subr_.begin(); it != subr_.end();) {
        if (same(it->first)) { ++it; continue; }
        if (it->second) it->second->releaseGL();
        it = subr_.erase(it);
    }
    comp_.clear();
    compBuilt_ = false;
    compBBLi_ = -1;
    compCv_ = -1;

    // ---- 動きを付けたテロップ
    const QString tsig = telopSignature(proj_, tl_, newSig);
    bool telopsRebuilt = false;
    if (tsig != telopSig_) {
        initTelops();
        telopSig_ = tsig;
        telopsRebuilt = true;
    }
    auto dropGen = [&](int li) {   // li 番の層の生成物(テクスチャ・待ち)を捨てるか
        if (telopsRebuilt && li >= 0 && li < (int)oldGen.size() && oldGen[li] == "telop") return true;
        return !same(li);
    };
    for (auto it = imgs_.begin(); it != imgs_.end();) {
        const int li = keyLayer(it->first);
        if (li >= 0 && dropGen(li)) { glDeleteTextures(1, &it->second.tex); it = imgs_.erase(it); }
        else ++it;
    }
    {
        QMutexLocker l(&imgMu_);
        for (auto it = ready_.begin(); it != ready_.end();) { const int li = keyLayer(it.key()); if (li >= 0 && dropGen(li)) it = ready_.erase(it); else ++it; }
        failed_.clear();   // 失敗の記録は全部消す(ファイルが直った場合も読み直す)
    }

    // ---- 字幕(ASS): 中身が同じなら libass の結果ごと残す
    subPlan_.init(proj_, tl_);
    if (subPlan_.ass() != oldAss || (hadSubs != !subPlan_.empty()) || tl_.W != oldW || tl_.H != oldH) {
        subs_.reset();
        for (auto& [k, t] : subTex_) if (t.tex) glDeleteTextures(1, &t.tex);
        subTex_.clear();
        if (!subPlan_.empty()) subs_ = std::make_unique<SubRender>(tl_.W, tl_.H, subPlan_.ass());
    }

    initV1Fx();
    startProber();
    ++reloads_;
    reloadMs_ = clk.nsecsElapsed() * 1e-6;
    if (onReloaded_) onReloaded_(audioSignature(proj_, tl_) != oldAudio);
    return true;
}

}  // namespace vn
