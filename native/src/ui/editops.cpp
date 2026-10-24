// EditModel の第2部の操作(項目の追加・トラック操作・移動の計算・ヒストリー・別名保存)。app.js からの移植
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <set>
#include "editmodel.h"

namespace vui {

static double tf3(double v) { return toFixedNum(v, 3); }

// ---------------- トラックのロックの保存(Web 版は localStorage "vedit.locks:<dir>/<name>") ----------------
QString EditModel::locksFile() {
    const QByteArray env = qgetenv("VEDIT_LOCKS");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + "/vedit/locks.json";
}

void EditModel::loadLocks() {
    locks_.clear();
    QFile f(locksFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    OVal v;
    if (!OVal::parse(f.readAll(), &v) || !v.isObj()) return;
    const OVal* a = v.get(lockKey());
    if (a && a->isArr()) for (const OVal& x : a->items()) if (x.isStr()) locks_.insert(x.asStr());
}

void EditModel::saveLocks() const {
    OVal all = OVal::object();
    {
        QFile f(locksFile());
        if (f.open(QIODevice::ReadOnly)) { OVal v; if (OVal::parse(f.readAll(), &v) && v.isObj()) all = v; }
    }
    QStringList ids(locks_.begin(), locks_.end());
    ids.sort();
    OVal a = OVal::array();
    for (const QString& s : ids) a.push(OVal::str(s));
    all.set(lockKey(), a);
    QDir().mkpath(QFileInfo(locksFile()).absolutePath());
    QFile f(locksFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(all.dump(true).toUtf8());
}

QString EditModel::relPath(const QString& p) const {
    QString n = p; n.replace('\\', '/');
    QString d = dir(); d.replace('\\', '/');
    d += "/";
    if (n.toLower().startsWith(d.toLower())) return n.mid(d.size());
    return n;
}

// ---------------- 画面の設定 ----------------
static QString prefsFile() {
    const QByteArray env = qgetenv("VEDIT_PREFS");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    return qEnvironmentVariable("APPDATA", QDir::homePath()) + "/vedit/native.json";
}
QString prefGet(const QString& key, const QString& def) {
    QFile f(prefsFile());
    if (!f.open(QIODevice::ReadOnly)) return def;
    OVal v;
    if (!OVal::parse(f.readAll(), &v) || !v.isObj()) return def;
    return v.strOr(key, def);
}
void prefSet(const QString& key, const QString& val) {
    OVal v = OVal::object();
    {
        QFile f(prefsFile());
        if (f.open(QIODevice::ReadOnly)) { OVal o; if (OVal::parse(f.readAll(), &o) && o.isObj()) v = o; }
    }
    v.set(key, OVal::str(val));
    QDir().mkpath(QFileInfo(prefsFile()).absolutePath());
    QFile f(prefsFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(v.dump(true).toUtf8());
}

// ---------------- 項目の追加・削除 ----------------
int EditModel::addItem(const QString& kind, const OVal& item) {
    mutate([&] { p_.get(kind)->push(item); sortList(kind); });
    const OVal& arr = *p_.get(kind);
    int idx = 0;
    for (int i = 0; i < arr.size(); ++i) if (arr.at(i) == item) { idx = i; }
    select(kind, idx);
    return idx;
}

int EditModel::addTelop() {
    OVal it = OVal::object();
    it.set("start", OVal::num(toFixedNum(tcur_, 2)));
    it.set("end", OVal::num(toFixedNum(std::min(dur_, tcur_ + 3), 2)));
    it.set("text", OVal::str(QStringLiteral("テロップ")));
    return addItem("telops", it);
}
int EditModel::addTopic() {
    OVal it = OVal::object();
    it.set("start", OVal::num(toFixedNum(tcur_, 2)));
    it.set("end", OVal::num(toFixedNum(std::min(dur_, tcur_ + 10), 2)));
    it.set("text", OVal::str(QStringLiteral("トピック")));
    return addItem("topics", it);
}
int EditModel::addOverlay(const QString& file) {
    OVal it = OVal::object();
    it.set("file", OVal::str(relPath(file)));
    it.set("start", OVal::num(toFixedNum(tcur_, 2)));
    it.set("end", OVal::num(toFixedNum(std::min(dur_, tcur_ + 3), 2)));
    it.set("pos", OVal::str("full"));
    if (vTarget > 2) it.set("track", OVal::num(vTarget));
    return addItem("overlays", it);
}
int EditModel::addSfx(const QString& file, double duration) {
    const QString f = relPath(file);
    if (duration > 0) sfxDur_.insert(f, duration);
    const OVal& sf = *p_.get("sfx");
    double g = 0;
    if (sf.size()) g = sf.at(sf.size() - 1).numOr("gain_db", 0);
    OVal it = OVal::object();
    it.set("file", OVal::str(f));
    it.set("start", OVal::num(tf3(tcur_)));
    it.set("end", OVal::num(tf3(tcur_ + (duration > 0 ? duration : 1))));
    it.set("gain_db", OVal::num(g));
    return addItem("sfx", it);
}

void EditModel::deleteSel() {
    std::vector<std::pair<QString, int>> keys;
    if (!selSet.isEmpty()) {
        for (const QString& s : selSet) keys.push_back({s.left(s.indexOf(':')), s.mid(s.indexOf(':') + 1).toInt()});
    } else if (!selKind.isEmpty()) keys.push_back({selKind, selI});
    if (keys.empty()) return;
    std::sort(keys.begin(), keys.end(), [](auto& x, auto& y) { return x.second > y.second; });
    selKind.clear(); selI = -1; selSet.clear();
    mutate([&] {
        for (auto& k : keys) {
            OVal* a = p_.get(k.first);
            if (a && k.second >= 0 && k.second < a->size()) a->erase(k.second);
        }
    });
}

// ---------------- ビデオトラック ----------------
OVal* EditModel::vtList() {
    const int c = vCount();
    OVal a = OVal::array();
    for (int k = 0; k < c; ++k) a.push(OVal::str(vName(k + 2)));
    p_.set("vtracks", a);
    return p_.get("vtracks");
}

void EditModel::shiftVTracks(int n, int d) {
    static const QRegularExpression re("^v(\\d+)$");
    auto sh = [&](const QString& id) {
        const auto m = re.match(id);
        if (m.hasMatch()) { const int x = m.captured(1).toInt(); if (x >= n && x != 1) return "v" + QString::number(x + d); }
        return id;
    };
    for (OVal& it : p_.get("overlays")->items()) if (trackOf(it) >= n) it.set("track", OVal::num(trackOf(it) + d));
    if (OVal* h = p_.get("hide"); h && h->isArr()) for (OVal& v : h->items()) if (v.isStr()) v = OVal::str(sh(v.asStr()));
    QSet<QString> nl;
    for (const QString& s : locks_) nl.insert(sh(s));
    locks_ = nl;
    saveLocks();
}

void EditModel::addVTrack(int n) {
    mutate([&] {
        OVal* v = vtList();
        shiftVTracks(n, 1);
        v->insert(n - 2, OVal::str(""));
        vTarget = n;
    });
}

int EditModel::overlaysOn(int n) const {
    int c = 0;
    for (const OVal& it : p_.get("overlays")->items()) if (trackOf(it) == n) ++c;
    return c;
}

bool EditModel::deleteVTrack(int n) {
    if (vCount() <= 1) { emit toast(QStringLiteral("最後のビデオトラックは削除できません")); return false; }
    select(QString(), -1);
    mutate([&] {
        OVal* v = vtList();
        OVal* ov = p_.get("overlays");
        for (int i = ov->size() - 1; i >= 0; --i) if (trackOf(ov->at(i)) == n) ov->erase(i);
        if (OVal* h = p_.get("hide"); h && h->isArr()) {
            for (int i = h->size() - 1; i >= 0; --i) if (h->at(i).isStr() && h->at(i).asStr() == "v" + QString::number(n)) h->erase(i);
        }
        locks_.remove("v" + QString::number(n));
        shiftVTracks(n + 1, -1);
        v->erase(n - 2);
        for (OVal& it : ov->items()) if (trackOf(it) == 2) it.remove("track");
        vTarget = std::min(vTarget, vCount() + 1);
    });
    return true;
}

bool EditModel::deleteEmptyVTracks() {
    std::set<int> used;
    for (const OVal& it : p_.get("overlays")->items()) used.insert(trackOf(it));
    std::vector<int> empty;
    for (int n = vCount() + 1; n >= 2; --n) if (!used.count(n)) empty.push_back(n);
    if ((int)empty.size() >= vCount()) empty.pop_back();
    if (empty.empty()) { emit toast(QStringLiteral("空のビデオトラックはありません")); return false; }
    mutate([&] {
        OVal* v = vtList();
        for (int n : empty) {
            if (OVal* h = p_.get("hide"); h && h->isArr())
                for (int i = h->size() - 1; i >= 0; --i) if (h->at(i).isStr() && h->at(i).asStr() == "v" + QString::number(n)) h->erase(i);
            locks_.remove("v" + QString::number(n));
            shiftVTracks(n + 1, -1);
            v->erase(n - 2);
        }
        vTarget = std::min(vTarget, vCount() + 1);
    });
    return true;
}

void EditModel::renameVTrack(int n, const QString& name) {
    mutate([&] { OVal* v = vtList(); if (n - 2 >= 0 && n - 2 < v->size()) v->at(n - 2) = OVal::str(name.trimmed()); });
}

bool EditModel::moveToTrack(int d) {
    std::vector<int> idx;
    for (const QString& s : selSet) if (s.startsWith("overlays:")) idx.push_back(s.mid(9).toInt());
    if (idx.empty()) return false;
    const int top = vCount() + 1;
    OVal* ov = p_.get("overlays");
    for (int j : idx) { const int t = trackOf(ov->at(j)); if (t + d < 2 || t + d > top + (d > 0 ? 1 : 0)) { emit toast(QStringLiteral("それ以上は動かせません")); return false; } }
    mutate([&] {
        bool grow = false;
        for (int j : idx) if (trackOf(ov->at(j)) + d > top) grow = true;
        if (grow) { OVal* v = vtList(); v->push(OVal::str("")); }
        for (int j : idx) {
            OVal& it = ov->at(j);
            const int nt = trackOf(it) + d;
            if (nt == 2) it.remove("track"); else it.set("track", OVal::num(nt));
        }
    });
    return true;
}

void EditModel::remapVTracks(const std::map<int, int>& map) {
    static const QRegularExpression re("^v(\\d+)$");
    auto mp = [&](int n) { auto it = map.find(n); return it == map.end() ? n : it->second; };
    OVal* v = vtList();
    std::vector<OVal> names = v->items();
    for (OVal& it : p_.get("overlays")->items()) {
        const int nt = mp(trackOf(it));
        if (nt == 2) it.remove("track"); else it.set("track", OVal::num(nt));
    }
    for (size_t k = 0; k < names.size(); ++k) { const int dst = mp((int)k + 2) - 2; if (dst >= 0 && dst < v->size()) v->at(dst) = names[k]; }
    auto sh = [&](const QString& id) {
        const auto m = re.match(id);
        if (m.hasMatch()) { const int x = m.captured(1).toInt(); auto f = map.find(x); if (f != map.end() && f->second) return "v" + QString::number(f->second); }
        return id;
    };
    if (OVal* h = p_.get("hide"); h && h->isArr()) for (OVal& x : h->items()) if (x.isStr()) x = OVal::str(sh(x.asStr()));
    QSet<QString> nl;
    for (const QString& s : locks_) nl.insert(sh(s));
    locks_ = nl;
    saveLocks();
    vTarget = mp(vTarget);
}

void EditModel::moveVTrack(int vn, int slot) {
    const int c = vCount();
    std::vector<int> ord;
    for (int n = c + 1; n >= 2; --n) ord.push_back(n);
    const int i = (int)(std::find(ord.begin(), ord.end(), vn) - ord.begin());
    if (i >= (int)ord.size()) return;
    ord.erase(ord.begin() + i);
    ord.insert(ord.begin() + (slot > i ? slot - 1 : slot), vn);
    std::map<int, int> map;
    bool same = true;
    for (int j = 0; j < (int)ord.size(); ++j) { map[ord[j]] = c + 1 - j; if (map[ord[j]] != ord[j]) same = false; }
    if (same) return;
    mutate([&] { remapVTracks(map); });
    emit toast(QString("V%1 → V%2(上のトラックほど手前に重なります)").arg(vn).arg(map[vn]));
}

// ---------------- V1/A1 クリップの移動の計算(app.js planMove) ----------------
EditModel::MovePlan EditModel::planMove(double a, double b, double dx, double snapTol) {
    struct P { bool gap = false; double s = 0, e = 0, len = 0, o = 0; bool me = false; double span() const { return gap ? len : e - s; } };
    const double L = b - a;
    std::vector<P> seq;
    for (const Piece& p : pieces()) {
        if (p.gap) { P q; q.gap = true; q.len = p.len; seq.push_back(q); continue; }
        if (p.e <= a + 1e-6 || p.s >= b - 1e-6) { P q; q.s = p.s; q.e = p.e; seq.push_back(q); continue; }
        if (a - p.s > 1e-4) { P q; q.s = p.s; q.e = a; seq.push_back(q); }
        { P q; q.gap = true; q.len = L; q.me = true; seq.push_back(q); }
        if (p.e - b > 1e-4) { P q; q.s = b; q.e = p.e; seq.push_back(q); }
    }
    double o = 0;
    for (P& p : seq) { p.o = o; o += p.span(); }
    MovePlan res;
    int mi = -1;
    for (int i = 0; i < (int)seq.size(); ++i) if (seq[i].me) { mi = i; break; }
    if (mi < 0) return res;
    const double o0 = seq[mi].o;
    const P* prev = nullptr; const P* next = nullptr;
    for (int i = mi - 1; i >= 0; --i) if (!seq[i].gap) { prev = &seq[i]; break; }
    for (int i = mi + 1; i < (int)seq.size(); ++i) if (!seq[i].gap) { next = &seq[i]; break; }
    const double lo = prev ? std::max(prev->o, prev->o + prev->e - prev->s - L) : 0;
    const double hi = next ? std::min(next->o, next->o + next->e - next->s - L) : 1e300;
    double n = o0 + dx;
    if (snapOn && snapTol > 0) {
        std::vector<double> pts = {outTime(tcur_)};
        for (const P& p : seq) if (!p.me) { pts.push_back(p.o); pts.push_back(p.o + p.span()); }
        double bd = snapTol, best = n;
        for (double q : pts) for (double off : {0.0, L}) if (std::fabs(q - off - n) < bd) { bd = std::fabs(q - off - n); best = q - off; }
        n = best;
    }
    n = clampd(n, lo, hi);
    struct O { bool gap = false; double s = 0, e = 0, len = 0; };
    std::vector<O> out;
    const double n1 = n + L;
    bool placed = false;
    auto place = [&] { if (!placed) { O q; q.s = a; q.e = b; out.push_back(q); placed = true; } };
    auto pushP = [&](const P& p) { O q; if (p.me) { q.gap = true; q.len = p.len; } else if (p.gap) { q.gap = true; q.len = p.len; } else { q.s = p.s; q.e = p.e; } out.push_back(q); };
    for (const P& p : seq) {
        const double len = p.span(), pe = p.o + len;
        if (pe <= n + 1e-6) { pushP(p); continue; }
        if (p.o >= n1 - 1e-6) { place(); pushP(p); continue; }
        const double ka = std::max(p.o, n), kb = std::min(pe, n1);
        if (p.gap) {
            if (ka - p.o > 1e-4) { O q; q.gap = true; q.len = ka - p.o; out.push_back(q); }
            place();
            if (pe - kb > 1e-4) { O q; q.gap = true; q.len = pe - kb; out.push_back(q); }
            continue;
        }
        const double sa = p.s + (ka - p.o), sb = p.s + (kb - p.o);
        if (sa - p.s > 1e-4) { O q; q.s = p.s; q.e = sa; out.push_back(q); }
        place();
        res.cuts.push_back({tf3(sa), tf3(sb)});
        if (p.e - sb > 1e-4) { O q; q.s = sb; q.e = p.e; out.push_back(q); }
    }
    if (!placed) { if (n - o > 1e-4) { O q; q.gap = true; q.len = n - o; out.push_back(q); } place(); }
    double acc = 0;
    for (const O& q : out) {
        if (q.gap) acc += q.len;
        else { if (acc > 1e-4) res.gaps.push_back({tf3(q.s), tf3(acc)}); acc = 0; }
    }
    res.ok = true;
    return res;
}

// ---------------- シーク吸着 ----------------
std::vector<double> EditModel::seekPts() {
    std::vector<double> pts = {0, dur_};
    for (const OVal& s : p_.get("splits")->items()) pts.push_back(s.asNum());
    for (auto& g : gapsN()) pts.push_back(g.first);
    // 各トラックの白い分割線(前のクリップの終わり = 次の頭)
    std::map<QString, std::vector<const OVal*>> byLane;
    for (const char* k : {"overlays", "topics", "telops", "sfx"})
        for (const OVal& it : p_.get(k)->items()) if (!isGapIt(it)) byLane[laneOf(k, it)].push_back(&it);
    for (auto& kv : byLane) {
        std::vector<double> st;
        for (auto* it : kv.second) st.push_back(it->numOr("start", 0));
        std::sort(st.begin(), st.end());
        for (auto* it : kv.second) {
            const double e = it->numOr("end", 0);
            auto f = std::find_if(st.begin(), st.end(), [&](double s) { return s > e - 1e-3; });
            if (f != st.end() && *f < e + 1e-3) pts.push_back(e);
        }
    }
    for (auto& c : mergedCuts()) { pts.push_back(c.second); if (!collapsed) pts.push_back(c.first); }
    return pts;
}

double EditModel::seekSnapT(double t, double pxPerSec, bool force) {
    if (!force) return t;
    auto X = [&](double v) { return (collapsed ? outTime(v) : v) * pxPerSec; };
    double best = t, bd = 10;
    const double xt = X(t);
    for (double q : seekPts()) { const double d = std::fabs(X(q) - xt); if (d < bd) { bd = d; best = q; } }
    return best;
}

// ---------------- ヒストリー ----------------
static QString canon(const OVal& v) {
    if (v.isArr()) { QStringList p; for (const OVal& x : v.items()) p << canon(x); return "[" + p.join(",") + "]"; }
    if (v.isObj()) {
        QStringList ks;
        for (int i = 0; i < v.size(); ++i) ks << v.keyAt(i);
        ks.sort();
        QStringList p;
        for (const QString& k : ks) p << OVal::str(k).dump(false) + ":" + canon(*v.get(k));
        return "{" + p.join(",") + "}";
    }
    return v.dump(false);
}
static QString fname(const QString& f) {
    QString s = f.mid(std::max(f.lastIndexOf('/'), f.lastIndexOf('\\')) + 1);
    static const QRegularExpression re("\\.(png|jpe?g|webp|gif|bmp|mp3|wav|m4a|mp4|mov)$", QRegularExpression::CaseInsensitiveOption);
    s.remove(re);
    return s;
}
static QString jsSlice(QString s, int n) { s.replace('\n', ' '); return s.left(n); }
static QString histItemName(const QString& k, const OVal& it) {
    if (k == "telops" || k == "topics") {
        QString t = it.strOr("text", "");
        if (t.isEmpty()) t = fname(it.strOr("file", ""));
        return QString::fromUtf8("「") + jsSlice(t, 14) + QString::fromUtf8("」");
    }
    if (k == "sfx") return fname(it.strOr("file", ""));
    const QString preset = it.strOr("preset", "");
    if (!preset.isEmpty()) return preset + (it.strOr("text", "").isEmpty() ? QString() : QString::fromUtf8("「") + jsSlice(it.strOr("text", ""), 10) + QString::fromUtf8("」"));
    if (!it.strOr("kind", "").isEmpty() && it.strOr("file", "").isEmpty()) return it.strOr("kind", "");
    return fname(it.strOr("file", ""));
}
static QString histCat(const QString& k, const OVal& it) {
    static const QRegularExpression img("\\.(png|jpe?g|webp|gif|bmp)$", QRegularExpression::CaseInsensitiveOption);
    if (k == "overlays" && img.match(it.strOr("file", "")).hasMatch()) return QStringLiteral("画像");
    if (k == "overlays") return QStringLiteral("差し込み");
    if (k == "telops") return QStringLiteral("テロップ");
    if (k == "topics") return QStringLiteral("トピック");
    return QStringLiteral("効果音");
}

QString EditModel::histDescribe(const OVal& a, const OVal& b) {
    QStringList parts;
    static const QStringList LISTS = {"overlays", "telops", "topics", "sfx"};
    static const std::vector<std::pair<QString, QString>> HL = {{"overlays", "差し込み"}, {"telops", "テロップ"}, {"topics", "トピック"}, {"sfx", "効果音"}};
    static const std::map<QString, QString> HK = {{"cuts", "カット"}, {"gaps", "空白"}, {"splits", "分割点"}, {"vmute", "映像だけ黒"}, {"amute", "音声だけ無音"}, {"aslip", "音声だけずらす"}, {"hide", "トラックの表示"}, {"mute", "トラックのミュート"},
        {"vtracks", "ビデオトラック"}, {"audio", "音声設定"}, {"bgm", "BGM"}, {"styles", "スタイル"}, {"output", "書き出し設定"}, {"transitions", "トランジション"}, {"v1_fx", "背景のエフェクト"}, {"cut_fx", "継ぎ目の処理"}, {"source", "元動画"}, {"timebase", "時刻の基準"}};
    auto arr = [](const OVal& o, const QString& k) { const OVal* x = o.get(k); return x && x->isArr() ? x->items() : std::vector<OVal>(); };
    for (const auto& hl : HL) {
        const QString& k = hl.first;
        const auto A = arr(a, k), B = arr(b, k);
        std::map<QString, int> bs, as;
        for (const OVal& it : B) bs[canon(it)]++;
        std::vector<OVal> rem, add;
        for (const OVal& it : A) { auto& c = bs[canon(it)]; if (c > 0) --c; else rem.push_back(it); }
        for (const OVal& it : A) as[canon(it)]++;
        for (const OVal& it : B) { auto& c = as[canon(it)]; if (c > 0) --c; else add.push_back(it); }
        if (rem.empty() && add.empty()) continue;
        auto one = [&](const QString& verb, const OVal& it, int n) {
            return n == 1 ? QString("%1 %2 %3").arg(verb, histCat(k, it), histItemName(k, it)) : QString("%1 %2 %3件").arg(verb, histCat(k, it)).arg(n);
        };
        if (rem.empty()) { parts << one(QStringLiteral("追加"), add[0], (int)add.size()); continue; }
        if (add.empty()) { parts << one(QStringLiteral("削除"), rem[0], (int)rem.size()); continue; }
        if (rem.size() == 1 && add.size() == 2 && std::all_of(add.begin(), add.end(), [&](const OVal& x) { return fname(x.strOr("file", "")) == fname(rem[0].strOr("file", "")) && x.strOr("text", "") == rem[0].strOr("text", ""); })) {
            parts << one(QStringLiteral("分割"), rem[0], 1);
            continue;
        }
        if (rem.size() == add.size()) {
            auto so = [](std::vector<OVal> x) { std::stable_sort(x.begin(), x.end(), [](const OVal& p, const OVal& q) { return p.numOr("start", 0) < q.numOr("start", 0); }); return x; };
            const auto R = so(rem), N = so(add);
            QStringList kinds;
            for (size_t i = 0; i < R.size(); ++i) {
                const OVal& r = R[i]; const OVal& n = N[i];
                auto rest = [](OVal o) { o.remove("start"); o.remove("end"); o.remove("in"); o.remove("track"); return canon(o); };
                if (rest(r) != rest(n)) { kinds << QStringLiteral("変更"); continue; }
                const double ds = n.numOr("start", 0) - r.numOr("start", 0), de = n.numOr("end", 0) - r.numOr("end", 0);
                if (trackOf(r) != trackOf(n) || (r.numOr("track", 2) != n.numOr("track", 2))) { kinds << (std::fabs(ds) < 1e-6 ? QStringLiteral("トラック移動") : QStringLiteral("移動")); continue; }
                if (std::fabs(ds - de) < 1e-6 && std::fabs(ds) > 1e-6) { kinds << QStringLiteral("移動"); continue; }
                kinds << (std::fabs(ds) > 1e-6 || std::fabs(de) > 1e-6 ? QStringLiteral("トリム") : QStringLiteral("変更"));
            }
            bool allSame = true;
            for (const QString& s : kinds) if (s != kinds[0]) allSame = false;
            parts << one(allSame ? kinds[0] : QStringLiteral("変更"), N[0], (int)N.size());
            continue;
        }
        parts << QString("変更 %1 %2件").arg(hl.second).arg(std::max(rem.size(), add.size()));
    }
    QStringList keys;
    for (int i = 0; i < a.size(); ++i) keys << a.keyAt(i);
    for (int i = 0; i < b.size(); ++i) if (!keys.contains(b.keyAt(i))) keys << b.keyAt(i);
    for (const QString& k : keys) {
        if (LISTS.contains(k)) continue;
        const OVal* x = a.get(k); const OVal* y = b.get(k);
        const QString cx = x ? canon(*x) : "undefined", cy = y ? canon(*y) : "undefined";
        if (cx == cy) continue;
        const bool A = x && x->isArr(), B = y && y->isArr();
        const auto hk = HK.find(k);
        const QString nm = hk == HK.end() ? k : hk->second;
        if (k == "cuts" && A && B) {
            std::set<QString> sa, sb;
            for (const OVal& v : x->items()) sa.insert(canon(v));
            for (const OVal& v : y->items()) sb.insert(canon(v));
            int na = 0, nr = 0;
            for (const OVal& v : y->items()) if (!sa.count(canon(v))) ++na;
            for (const OVal& v : x->items()) if (!sb.count(canon(v))) ++nr;
            parts << (nr == 0 ? QString::fromUtf8("カット") + (na > 1 ? QString(" %1件").arg(na) : QString())
                              : na == 0 ? QString::fromUtf8("カットを戻す") + (nr > 1 ? QString(" %1件").arg(nr) : QString())
                                        : QStringLiteral("カットを変更(リップル)"));
        } else if (k == "splits" && A && B) {
            parts << (y->size() > x->size() ? QStringLiteral("分割") : y->size() < x->size() ? QStringLiteral("分割点を削除") : QStringLiteral("分割点を移動"));
        } else if (A && B && y->size() != x->size()) {
            parts << nm + QStringLiteral("を") + (y->size() > x->size() ? QStringLiteral("追加") : QStringLiteral("削除"));
        } else parts << nm + QStringLiteral("を変更");
    }
    if (parts.isEmpty()) return QString();
    if (parts.size() > 2) return parts.mid(0, 2).join(" / ") + QString(" ほか%1件").arg(parts.size() - 2);
    return parts.join(" / ");
}

std::vector<EditModel::HistRow> EditModel::histRows() {
    std::vector<QString> st = undoS_;
    st.push_back(p_.dump(false));
    for (auto it = redoS_.rbegin(); it != redoS_.rend(); ++it) st.push_back(*it);
    const int c = (int)undoS_.size();
    std::vector<HistRow> rows;
    rows.push_back({0, histBase, false, false});
    static QHash<QString, QString> cache;   // 前後の状態の組 → 操作名(重い JSON 比較を繰り返さない)
    if (cache.size() > 2000) cache.clear();
    for (int i = 1; i < (int)st.size(); ++i) {
        const QString key = QString::number(st[i - 1].size()) + ":" + QString::number(qHash(st[i - 1])) + "|" + QString::number(st[i].size()) + ":" + QString::number(qHash(st[i]));
        QString lb;
        auto f = cache.find(key);
        if (f != cache.end()) lb = f.value();
        else {
            OVal x, y;
            if (OVal::parse(st[i - 1].toUtf8(), &x) && OVal::parse(st[i].toUtf8(), &y)) lb = histDescribe(x, y);
            else lb = QStringLiteral("変更");
            cache.insert(key, lb);
        }
        if (!lb.isEmpty()) rows.push_back({i, lb, false, i > c});
    }
    int cur = 0;
    for (auto& r : rows) if (r.idx <= c) cur = r.idx;
    for (auto& r : rows) r.cur = r.idx == cur;
    return rows;
}

bool EditModel::histJump(int k) {
    const int c = (int)undoS_.size();
    if (k == c) return false;
    QString cur = p_.dump(false);
    while ((int)undoS_.size() > k) { redoS_.push_back(cur); cur = undoS_.back(); undoS_.pop_back(); }
    while ((int)undoS_.size() < k && !redoS_.empty()) { undoS_.push_back(cur); cur = redoS_.back(); redoS_.pop_back(); }
    OVal v;
    if (!OVal::parse(cur.toUtf8(), &v)) return false;
    p_ = v;
    selKind.clear(); selI = -1; selSet.clear(); segSel.clear(); gapSel.clear(); bgmSel = false; trkSel.clear();
    emit selectionChanged();
    commit(true);
    return true;
}

// ---------------- 別名で保存 ----------------
bool EditModel::saveAs(const QString& newPath, QString* err) {
    saveTimer_.stop();
    const QString old = path_;
    path_ = QFileInfo(newPath).absoluteFilePath();
    lastMtime_ = 0;
    const SaveResult r = save(true);
    if (r != Saved) { path_ = old; if (err) *err = QStringLiteral("保存できませんでした"); return false; }
    loadLocks();
    emit changed();
    return true;
}

}  // namespace vui
