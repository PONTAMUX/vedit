#include "presets.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <cmath>
#include <map>
#include "expr.h"
#include "timeutil.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace vn {

namespace {

const char* TARGETS[] = {"overlays", "telops", "sfx", "v1_fx", "transitions"};

bool truthy(const QJsonValue& v) {
    if (v.isNull() || v.isUndefined()) return false;
    if (v.isBool()) return v.toBool();
    if (v.isDouble()) return v.toDouble() != 0;
    if (v.isString()) return !v.toString().isEmpty();
    if (v.isArray()) return !v.toArray().isEmpty();
    return !v.toObject().isEmpty();
}

QString presetDir() {
    QStringList c;
    const QByteArray env = qgetenv("VEDIT_PRESET_DIR");
    if (!env.isEmpty()) c << QString::fromLocal8Bit(env);
    QDir d(QCoreApplication::applicationDirPath());
    for (int i = 0; i < 6; ++i) {
        c << d.filePath("vedit/presets") << d.filePath("presets");
        if (!d.cdUp()) break;
    }
    c << QString::fromUtf8("C:/vedit/vedit/presets");
    for (const QString& p : c)
        if (QFileInfo(p + "/impact.json").isFile()) return p;
    return QString();
}

const std::map<QString, QJsonObject>& loadPresets() {
    static std::map<QString, QJsonObject> m;
    static bool done = false;
    if (done) return m;
    done = true;
    const QString dir = presetDir();
    if (dir.isEmpty()) return m;
    for (const QFileInfo& fi : QDir(dir).entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        QFile f(fi.absoluteFilePath());
        if (f.open(QIODevice::ReadOnly)) m[fi.completeBaseName()] = QJsonDocument::fromJson(f.readAll()).object();
    }
    return m;
}

// ---- 効果音の音量そろえ
QString findSound(const QString& name, const QString& projDir) {
    QStringList cands;
    cands << (QFileInfo(name).isAbsolute() ? name : QDir(projDir).filePath(name)) << name;
    QStringList dirs;
    for (const QString& s : QString::fromLocal8Bit(qgetenv("VEDIT_SE_DIRS")).split(';', Qt::SkipEmptyParts)) dirs << s;
    dirs << QString::fromUtf8("se") << QString::fromUtf8("se");
    for (const QString& d : dirs) {
        cands << d + "/" + name;
        for (const char* e : {".mp3", ".wav", ".m4a", ".ogg", ".flac"}) cands << d + "/" + name + e;
    }
    for (const QString& c : cands)
        if (QFileInfo(c).isFile()) return c;
    return QString();
}

QString peakFile() {
    QString base = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
    if (base.isEmpty()) base = QDir::homePath();
    return base + "/vedit/peaks.json";
}

QString peakKey(const QString& path) {
    const QFileInfo fi(path);
    QString canon = fi.canonicalFilePath();
    if (canon.isEmpty()) canon = fi.absoluteFilePath();
    canon = QDir::toNativeSeparators(canon);
    long long ns = 0;
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExW((const wchar_t*)QDir::toNativeSeparators(path).utf16(), GetFileExInfoStandard, &a)) {
        const unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        ns = (long long)(t - 116444736000000000ULL) * 100;
    }
#endif
    return QString("%1|%2|%3").arg(canon).arg(fi.size()).arg(ns);
}

double peakDb(const QString& path) {
    static QMutex mu;
    static QJsonObject cache;
    static bool loaded = false;
    QMutexLocker l(&mu);
    if (!loaded) {
        loaded = true;
        QFile f(peakFile());
        if (f.open(QIODevice::ReadOnly)) cache = QJsonDocument::fromJson(f.readAll()).object();
    }
    const QString key = peakKey(path);
    if (cache.contains(key)) return cache[key].toDouble();
    QProcess p;
#ifdef _WIN32
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= 0x08000000; });   // CREATE_NO_WINDOW
#endif
    QString ff = QString::fromLocal8Bit(qgetenv("VEDIT_FFMPEG"));
    if (ff.isEmpty()) ff = "ffmpeg";
    p.start(ff, {"-hide_banner", "-nostats", "-i", path, "-af", "apad=pad_dur=1,ebur128=peak=true", "-f", "null", "-"});
    if (!p.waitForFinished(60000)) { p.kill(); return 0; }
    const QString err = QString::fromUtf8(p.readAllStandardError());
    QRegularExpressionMatchIterator it = QRegularExpression(R"(Peak:\s+(-?[\d.]+|-inf) dBFS)").globalMatch(err);
    QString last;
    while (it.hasNext()) last = it.next().captured(1);
    if (last.isEmpty()) return 0;
    const double v = last == "-inf" ? -120.0 : last.toDouble();
    cache[key] = v;
    QDir().mkpath(QFileInfo(peakFile()).absolutePath());
    QFile f(peakFile());
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(cache).toJson(QJsonDocument::Compact));
    return v;
}

bool parseMatch(const QJsonValue& g, QString* ref, double* off) {
    if (!g.isString()) return false;
    static const QRegularExpression re(R"(^match:(.+?)([+-]\d+(?:\.\d+)?)?$)");
    const auto m = re.match(g.toString().trimmed());
    if (!m.hasMatch()) return false;
    *ref = m.captured(1).trimmed();
    *off = m.captured(2).toDouble();
    return true;
}

double matchGain(const QJsonValue& g, const QString& file, const QString& projDir) {
    if (!g.isString()) return g.toDouble(0);
    QString refName;
    double off = 0;
    if (!parseMatch(g, &refName, &off)) return g.toString().toDouble();
    const QString ref = findSound(refName, projDir);
    if (ref.isEmpty()) return 0;
    const QString f = QFileInfo(file).isAbsolute() ? file : QDir(projDir).filePath(file);
    const double v = 1.0 + peakDb(ref) - peakDb(f);
    return std::nearbyint((std::max(-12.0, std::min(6.0, v)) + off) * 10) / 10;
}

// ---- 展開
QJsonValue sub(const QJsonValue& v, const QJsonObject& P, int n) {
    if (v.isString()) {
        const QString s = v.toString();
        if (s.startsWith("$=")) {
            QStringList names;
            ExprCtx ctx;
            for (auto it = P.begin(); it != P.end(); ++it) {
                if (it.value().isBool()) { names.append(it.key()); ctx.vars[it.key()] = it.value().toBool() ? 1.0 : 0.0; }
                else if (it.value().isDouble()) { names.append(it.key()); ctx.vars[it.key()] = it.value().toDouble(); }
            }
            return std::nearbyint(evalExpr(s.mid(2), ctx, names) * 1e4) / 1e4;
        }
        if (s.startsWith('$') && P.contains(s.mid(1))) return P[s.mid(1)];   // null も返す(辞書の中では後で落とす)
        return v;
    }
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        if (o.contains("$pick")) {
            const QJsonArray lst = o["$pick"].toArray();
            if (lst.isEmpty()) return QJsonValue(QJsonValue::Null);
            return sub(lst[n % lst.size()], P, n);
        }
        QJsonObject out;
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QJsonValue y = sub(it.value(), P, n);
            if (!y.isNull()) out[it.key()] = y;
        }
        return out;
    }
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue& x : v.toArray()) out.append(sub(x, P, n));
        return out;
    }
    return v;
}

bool gapLen(const QJsonObject& data, double at, double* len) {
    for (const QJsonValue& g : data["gaps"].toArray()) {
        const QJsonArray a = g.toArray();
        if (a.size() >= 2 && std::fabs(parseTimeOr(a[0], 0) - at) < 1e-3) { *len = a[1].toDouble(); return true; }
    }
    return false;
}

struct Inst { QJsonArray out[5]; QJsonObject styles; };

bool instance(const QJsonObject& data, const QJsonObject& el, int n, Inst* r) {
    const QString name = el["preset"].toString();
    const auto& all = loadPresets();
    const auto it = all.find(name);
    if (it == all.end()) return false;
    const QJsonObject& spec = it->second;
    QJsonObject P;
    const QJsonObject params = spec["params"].toObject();
    for (auto pi = params.begin(); pi != params.end(); ++pi)
        P[pi.key()] = (pi.value().isObject() && pi.value().toObject().contains("$pick")) ? sub(pi.value(), QJsonObject(), n) : pi.value();
    for (auto ei = el.begin(); ei != el.end(); ++ei)
        if (ei.key() != "preset") P[ei.key()] = ei.value();
    P["n"] = n;
    if (el.contains("gap_at") && !el["gap_at"].isNull()) {
        double gl = 0;
        const bool have = gapLen(data, parseTimeOr(el["gap_at"], 0), &gl);
        P["gap_len"] = have ? QJsonValue(gl) : QJsonValue(QJsonValue::Null);
        if (P["start"].isNull() || P["start"].isUndefined()) P["start"] = 0.0;
        if (P["end"].isNull() || P["end"].isUndefined()) P["end"] = have ? QJsonValue(gl) : QJsonValue(QJsonValue::Null);
    }
    for (const char* k : {"start", "end"}) {
        double t;
        if (!P[k].isNull() && !P[k].isUndefined() && parseTime(P[k], &t)) P[k] = t;
    }
    if (P["start"].isDouble() && P["end"].isDouble()) P["len"] = P["end"].toDouble() - P["start"].toDouble();
    for (const QJsonValue& xv : spec["items"].toArray()) {
        const QJsonObject x = xv.toObject();
        const QString cond = x["if"].toString();
        if (!cond.isEmpty()) {
            const bool neg = cond.startsWith('!');
            QString key = cond;
            while (key.startsWith('!')) key.remove(0, 1);
            if (truthy(P[key]) == neg) continue;
        }
        QJsonObject item = sub(x["item"], P, n).toObject();
        item["_preset"] = name;
        const QString to = x["to"].toString();
        for (int i = 0; i < 5; ++i)
            if (to == TARGETS[i]) r->out[i].append(item);
    }
    r->styles = spec["styles"].toObject();
    return true;
}

}  // namespace

QJsonObject expandPresets(const QJsonObject& data, const QString& projDir) {
    bool work = false;
    for (const char* k : {"overlays", "telops"})
        for (const QJsonValue& v : data[k].toArray())
            if (v.isObject() && truthy(v.toObject()["preset"])) work = true;
    for (const QJsonValue& s : data["sfx"].toArray())
        if (s.toObject()["gain_db"].isString()) work = true;
    for (const QJsonValue& t : data["transitions"].toArray())
        if (t.toObject()["se"].toObject()["gain_db"].isString()) work = true;
    if (!work) return data;

    QJsonObject d = data;
    QJsonArray lists[5];
    QJsonObject styles;
    std::map<QString, int> count;
    for (int ki = 0; ki < 2; ++ki) {
        for (const QJsonValue& ev : data[TARGETS[ki]].toArray()) {
            const QJsonObject el = ev.toObject();
            if (ev.isObject() && truthy(el["preset"])) {
                const QString nm = el["preset"].toString();
                const int n = count[nm]++;
                Inst in;
                if (!instance(data, el, n, &in)) continue;
                for (int t = 0; t < 5; ++t)
                    for (const QJsonValue& x : in.out[t]) lists[t].append(x);
                for (auto si = in.styles.begin(); si != in.styles.end(); ++si)
                    if (!styles.contains(si.key())) styles[si.key()] = si.value();
            } else lists[ki].append(ev);
        }
    }
    for (int t = 0; t < 5; ++t) {
        if (t < 2) { d[TARGETS[t]] = lists[t]; continue; }
        QJsonArray own = data[TARGETS[t]].toArray();
        for (const QJsonValue& x : lists[t]) own.append(x);
        d[TARGETS[t]] = own;
    }
    if (!styles.isEmpty()) {
        QJsonObject st = styles;
        const QJsonObject mine = data["styles"].toObject();
        for (auto it = mine.begin(); it != mine.end(); ++it) st[it.key()] = it.value();
        d["styles"] = st;
    }
    QJsonArray sf;
    for (const QJsonValue& sv : d["sfx"].toArray()) {
        QJsonObject s = sv.toObject();
        if (s["gain_db"].isString()) s["gain_db"] = matchGain(s["gain_db"], s["file"].toString(), projDir);
        sf.append(s);
    }
    d["sfx"] = sf;
    QJsonArray tr;
    for (const QJsonValue& tv : d["transitions"].toArray()) {
        QJsonObject t = tv.toObject();
        QJsonObject se = t["se"].toObject();
        if (!se.isEmpty() && se["gain_db"].isString()) {
            se["gain_db"] = matchGain(se["gain_db"], se["file"].toString(), projDir);
            t["se"] = se;
        }
        tr.append(t);
    }
    d["transitions"] = tr;
    return d;
}

}  // namespace vn
