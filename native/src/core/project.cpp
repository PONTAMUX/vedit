#include "project.h"
#include "presets.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

namespace vn {

// fx.py の needs_bake(ファイルのある要素)
static bool overlayNeedsFx(const QJsonObject& o) {
    auto tr = [&](const char* k) {
        const QJsonValue v = o[k];
        if (v.isNull() || v.isUndefined()) return false;
        if (v.isBool()) return v.toBool();
        if (v.isDouble()) return v.toDouble() != 0;
        if (v.isString()) return !v.toString().isEmpty();
        if (v.isArray()) return !v.toArray().isEmpty();
        return !v.toObject().isEmpty();
    };
    if (tr("fx") || tr("keys") || tr("mask") || tr("matte") || tr("parent") || tr("_preset")) return true;
    if (o.contains("blend") && !o["blend"].isNull() && o["blend"].toString() != "normal") return true;
    for (const char* side : {"in_anim", "out_anim"}) {
        const QJsonValue v = o[side];
        if (v.isObject()) return true;
        if (v.isString()) {
            static const QSet<QString> ok{"none", "fade", "slide-left", "slide-right", "slide-up", "slide-down"};
            if (!ok.contains(v.toString())) return true;
        }
    }
    return false;
}

QString Project::resolve(const QString& p) const {
    QFileInfo fi(p);
    return fi.isAbsolute() ? p : QDir(dir).filePath(p);
}

QString Project::cacheDir() const {
    return QDir(dir).filePath(QFileInfo(path).completeBaseName() + ".cache");
}

static TimeMap::Ranges readRanges(const QJsonValue& v, bool secondIsLen) {
    TimeMap::Ranges r;
    for (const QJsonValue& e : v.toArray()) {
        const QJsonArray a = e.toArray();
        if (a.size() < 2) continue;
        double x = parseTimeOr(a[0], 0), y = secondIsLen ? a[1].toDouble() : parseTimeOr(a[1], 0);
        r.push_back({x, y});
    }
    return r;
}

static QString animName(const QJsonValue& v) {
    if (!v.isString()) return QString();
    const QString s = v.toString();
    static const QSet<QString> ok{"fade", "slide-left", "slide-right", "slide-up", "slide-down"};
    return ok.contains(s) ? s : QString();
}

bool Project::load(const QString& path, Project* out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { out->error = "読めません: " + path; return false; }
    return loadBytes(path, f.readAll(), out);
}

bool Project::loadBytes(const QString& path, const QByteArray& bytes, Project* out) {
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (!doc.isObject()) { out->error = "JSON が不正: " + pe.errorString(); return false; }
    QFileInfo fi(path);
    const QJsonObject d = expandPresets(doc.object(), fi.absolutePath());
    out->path = fi.absoluteFilePath();
    out->dir = fi.absolutePath();
    out->source = d["source"].toString();
    out->timebase = d.contains("timebase") ? d["timebase"].toString() : "source";
    out->cuts = readRanges(d["cuts"], false);
    out->gaps = readRanges(d["gaps"], true);
    out->vmute = readRanges(d["vmute"], false);
    for (const QJsonValue& h : d["hide"].toArray()) out->hide.insert(h.toString());
    for (const QJsonValue& h : d["mute"].toArray()) out->mute.insert(h.toString());
    out->amute = readRanges(d["amute"], false);
    for (const QJsonValue& e : d["aslip"].toArray()) {
        const QJsonObject o = e.toObject();
        out->aslip.push_back({parseTimeOr(o["a"], 0), parseTimeOr(o["b"], 0), o["d"].toDouble(0)});
    }
    if (d["cut_fx"].isObject() && d["cut_fx"].toObject().contains("audio_fade_ms"))
        out->audioFadeMs = d["cut_fx"].toObject()["audio_fade_ms"].toDouble(8);
    for (const QJsonValue& ev : d["sfx"].toArray()) {
        const QJsonObject o = ev.toObject();
        if (o["file"].toString().isEmpty()) continue;
        if (out->mute.contains(o["kind"].toString() == "bgm" ? "bgm" : "se")) continue;
        SfxSpec s;
        s.file = o["file"].toString();
        if (!parseTime(o["start"], &s.start)) continue;
        if (!o["end"].isNull() && o.contains("end")) { s.hasEnd = parseTime(o["end"], &s.end); }
        s.in = parseTimeOr(o["in"], 0);
        s.gainDb = o["gain_db"].toDouble(0);
        if (o.contains("gap_at") && !o["gap_at"].isNull()) { s.hasGapAt = true; s.gapAt = parseTimeOr(o["gap_at"], 0); }
        out->sfx.push_back(s);
    }
    if (d["bgm"].isObject() && !out->mute.contains("bgm")) {
        const QJsonObject b = d["bgm"].toObject();
        if (!b.isEmpty() && !b["file"].toString().isEmpty()) {
            out->bgm.on = true;
            out->bgm.file = b["file"].toString();
            out->bgm.volumeDb = b["volume_db"].toDouble(-23.5);
            out->bgm.duck = b["duck"].toBool();
        }
    }
    out->outFps = d["output"].toObject()["fps"].toDouble(0);
    // 差し込み 1 つ分を読む。comp(プリコンポーズ)の中の層は isSub=true(時刻は comp の頭から)。戻り値: overlays の番号(読めなければ -1)
    auto readOv = [&](const QJsonObject& o, bool isSub, int pyIdx) -> int {
        OverlaySpec s;
        s.file = o["file"].toString();
        s.kind = o["kind"].toString();
        if (s.file.isEmpty()) {   // 生成要素以外(調整レイヤー・プリコンポーズ等)は対象外
            if (s.kind == "speedlines" || s.kind == "gloom" || s.kind == "flash" || s.kind == "adjust") s.gen = s.kind;
            else if (s.kind == "comp" && o["layers"].isArray() && !o["project"].isString()) s.gen = "comp";
            else if (s.kind == "comp" && o["project"].isString() && !o["project"].toString().isEmpty()) { s.gen = "subproj"; s.subProject = o["project"].toString(); }
            else if (isSub && s.kind.isEmpty() && !o["text"].toString().trimmed().isEmpty()) {
                // comp 内のテロップ(text 子): Python は {**L,"_telop":True} で track 900 のテロップ層にする(telop_norm は通らない)
                SubSpec t;
                t.text = o["text"].toString();
                if (!parseTime(o["start"], &t.start) || !parseTime(o["end"], &t.end)) return -1;
                if (o.contains("style")) t.style = o["style"].toString();
                if (o["pos"].isArray() && o["pos"].toArray().size() >= 2) { t.hasPos = true; t.px = o["pos"].toArray()[0].toDouble(); t.py = o["pos"].toArray()[1].toDouble(); }
                if (o.contains("anim_dur")) t.animDur = o["anim_dur"].toDouble(0.2);
                t.needsBake = true;
                t.raw = o;
                t.raw["_insub"] = true;
                out->telops.push_back(t);
                s.gen = "telop";
                s.telop = (int)out->telops.size() - 1;
                s.track = 900;
            }
            else return -1;
        }
        if (!parseTime(o["start"], &s.start) || !parseTime(o["end"], &s.end)) return -1;
        if (o.contains("pos") && !o["pos"].isNull()) {
            s.hasPos = true;
            if (o["pos"].isArray()) {
                const QJsonArray a = o["pos"].toArray();
                s.posIsXY = a.size() >= 2;
                if (s.posIsXY) { s.px = a[0].toDouble(); s.py = a[1].toDouble(); }
            } else s.posName = o["pos"].toString();
        }
        if (o.contains("scale") && !o["scale"].isNull()) { s.hasScale = true; s.scale = o["scale"].toDouble(0.3); }
        s.fit = o["fit"].toString();
        s.in = parseTimeOr(o["in"], 0);
        s.inAnim = animName(o["in_anim"]);
        s.outAnim = animName(o["out_anim"]);
        s.animDur = o.contains("anim_dur") ? o["anim_dur"].toDouble(0.3) : 0.3;
        if (o.contains("track") && !o["track"].isNull()) {
            double tv = o["track"].toDouble(2);
            s.track = std::max(2, (int)std::nearbyint(tv == 0 ? 2 : tv));   // Python: round(float(track or 2))
        }
        if (s.gen == "telop") s.track = 900;
        if (o.contains("gap_at") && !o["gap_at"].isNull()) { s.hasGapAt = true; s.gapAt = parseTimeOr(o["gap_at"], 0); }
        s.audio = o["audio"].toBool();
        s.volumeDb = o["volume_db"].toDouble(0);
        s.raw = o;
        if (s.gen == "telop") { s.raw["_telop"] = true; s.raw["track"] = 900; }
        s.fxPath = !s.gen.isEmpty() || overlayNeedsFx(o);
        s.sub = isSub;
        s.raw["_i"] = pyIdx;
        out->overlays.push_back(s);
        return (int)out->overlays.size() - 1;
    };
    int pyIdx = 0;
    for (const QJsonValue& ev : d["overlays"].toArray()) {
        const QJsonObject o = ev.toObject();
        const int ci = readOv(o, false, pyIdx++);
        if (ci < 0 || out->overlays[ci].gen != "comp") continue;
        const double compLen = out->overlays[ci].end - out->overlays[ci].start;
        std::vector<int> subs;
        int si = 0;
        for (const QJsonValue& lv : o["layers"].toArray()) {
            QJsonObject lo = lv.toObject();
            if (lo["end"].isNull() || lo["end"].isUndefined()) lo["end"] = compLen;   // 省略 = comp の終わりまで
            if (!lo.contains("start")) lo["start"] = 0;
            const int k = readOv(lo, true, si++);
            if (k >= 0) subs.push_back(k);
        }
        std::stable_sort(subs.begin(), subs.end(), [&](int x, int y) { return out->overlays[x].track < out->overlays[y].track; });
        out->overlays[ci].subs = subs;
        for (int k : subs) out->overlays[k].compParent = ci;
    }
    out->styles = d["styles"].toObject();
    out->v1fx = d["v1_fx"].toArray();
    out->transitions = d["transitions"].toArray();
    out->cutFx = d["cut_fx"].toObject();
    // telops / topics(空の文字は出さない。Python の shown())
    auto readSubs = [&](const char* key, bool topic, std::vector<SubSpec>* dst) {
        for (const QJsonValue& ev : d[key].toArray()) {
            const QJsonObject o = ev.toObject();
            SubSpec s;
            s.text = o["text"].toString();
            if (s.text.trimmed().isEmpty()) continue;
            if (!parseTime(o["start"], &s.start) || !parseTime(o["end"], &s.end)) continue;
            if (o.contains("gap_at") && !o["gap_at"].isNull()) { s.hasGapAt = true; s.gapAt = parseTimeOr(o["gap_at"], 0); }
            if (o.contains("style")) s.style = o["style"].toString();
            if (!topic && o["pos"].isArray() && o["pos"].toArray().size() >= 2) {
                s.hasPos = true;
                s.px = o["pos"].toArray()[0].toDouble();
                s.py = o["pos"].toArray()[1].toDouble();
            }
            if (o.contains("anim_dur")) s.animDur = o["anim_dur"].toDouble(0.2);
            if (topic) {
                if (o.contains("anim") && !(o["anim"].isString() && o["anim"].toString() == "none")) s.anim = "fade";
            } else {
                const QJsonValue a = o["anim"];
                const QString an = a.isString() ? a.toString() : QString("none");
                static const QSet<QString> legacy{"fade", "pop", "slide-up"};
                if (legacy.contains(an)) s.anim = an;
                // fx.py 側で焼く指定(fx・dict の anim・新しい anim・keys 等・in/out_anim)
                auto nz = [&](const char* k) { return o.contains(k) && !o[k].isNull() && !(o[k].isString() && o[k].toString() == "none"); };
                s.needsBake = (o.contains("fx") && !o["fx"].isNull() && !(o["fx"].isArray() && o["fx"].toArray().isEmpty())) ||
                              a.isObject() || (a.isString() && an != "none" && !legacy.contains(an)) || nz("in_anim") || nz("out_anim") ||
                              nz("keys") || nz("mask") || nz("matte") || nz("parent") || nz("animator") || nz("_preset") ||
                              (o.contains("blend") && !o["blend"].isNull() && o["blend"].toString() != "normal");
            }
            if (!topic) s.raw = o;
            dst->push_back(s);
        }
    };
    if (!out->hide.contains("t")) readSubs("telops", false, &out->telops);
    if (!out->hide.contains("tb")) readSubs("topics", true, &out->topics);
    // 動きを付けたテロップ(fx.py telop_items / telop_norm): gen="telop" の差し込み(トラック 900 = 差し込みの上・トランジションの下)として描く
    for (int ti = 0; ti < (int)out->telops.size(); ++ti) {
        const SubSpec& t = out->telops[ti];
        if (!t.needsBake || t.raw["_insub"].toBool()) continue;
        const QJsonObject& o = t.raw;
        auto isNone = [](const QJsonValue& v) { return v.isNull() || v.isUndefined() || (v.isString() && v.toString() == "none"); };
        QJsonObject it = o;
        it["_telop"] = true;
        it["track"] = 900;
        if (isNone(o["in_anim"]) && isNone(o["out_anim"])) {
            const QJsonValue a = o["anim"];
            const bool legacy = isNone(a) || (a.isString() && (a.toString() == "fade" || a.toString() == "pop" || a.toString() == "slide-up"));
            if (a.isObject() || !legacy) it["in_anim"] = a;
            else if (!isNone(a)) {   // fx と一緒に付いた今までの fade/pop/slide-up は出入りのフェードに
                it["in_anim"] = "fade";
                it["out_anim"] = "fade";
                if (!it.contains("anim_dur")) it["anim_dur"] = 0.2;
            }
        }
        OverlaySpec s;
        s.gen = "telop";
        s.telop = ti;
        s.start = t.start;
        s.end = t.end;
        s.hasGapAt = t.hasGapAt;
        s.gapAt = t.gapAt;
        s.track = 900;
        s.fxPath = true;
        s.raw = it;
        s.raw["_i"] = -1;
        out->overlays.push_back(s);
    }
    return true;
}

}  // namespace vn
