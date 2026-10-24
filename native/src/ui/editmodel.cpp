#include "editmodel.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include "media/videostream.h"
#ifdef _WIN32
#include <windows.h>

#endif

namespace vui {

double clampd(double v, double a, double b) { return std::min(b, std::max(a, v)); }
static double jsRound(double x) { return std::floor(x + 0.5); }
static double tf3(double v) { return toFixedNum(v, 3); }
static const double NaN = std::nan("");

double parseT(const OVal& v) {
    if (v.isNum()) return v.asNum();
    if (!v.isStr()) return NaN;
    const QStringList parts = v.asStr().split(':');
    double acc = 0;
    for (int i = 0; i < parts.size(); ++i) {
        const QString s = parts[i].trimmed();
        double x = 0;
        if (!s.isEmpty()) { bool ok = false; x = s.toDouble(&ok); if (!ok) return NaN; }
        acc = i == 0 ? x : acc * 60 + x;
    }
    return acc;
}

QString fmtT(double t, bool cs) {
    t = cs ? jsRound(std::max(0.0, t) * 100) / 100 : std::max(0.0, t);
    const int m = (int)std::floor(t / 60);
    const double s = t - m * 60;
    const QString ss = cs ? QString::number(s, 'f', 2).rightJustified(5, '0') : QString::number((int)std::floor(s)).rightJustified(2, '0');
    return QString("%1:%2").arg(m).arg(ss);
}

template <class F> static int lowerIdx(int n, F ok) {
    int lo = 0, hi = n;
    while (lo < hi) { const int m = (lo + hi) >> 1; if (ok(m)) hi = m; else lo = m + 1; }
    return lo;
}

static OVal numArr(std::initializer_list<double> v) { OVal a = OVal::array(); for (double x : v) a.push(OVal::num(x)); return a; }

static double fileMtime(const QString& p) { return QFileInfo(p).lastModified().toMSecsSinceEpoch() / 1000.0; }

EditModel::EditModel(QObject* parent) : QObject(parent) {
    saveTimer_.setSingleShot(true);
    connect(&saveTimer_, &QTimer::timeout, this, [this] { if (autosaveOn_) save(false); });
    connect(&watchTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ || (isPlaying && isPlaying())) return;
        checkExternal();
    });
    connect(&backupTimer_, &QTimer::timeout, this, [this] { writeTimedBackup(); });
}

QString EditModel::name() const {
    return QFileInfo(path_).completeBaseName();   // Python の Path.stem と同じ
}
QString EditModel::dir() const { return QFileInfo(path_).absolutePath(); }
QString EditModel::resolve(const QString& rel) const {
    if (QFileInfo(rel).isAbsolute()) return rel;
    return QDir::cleanPath(dir() + "/" + rel);
}

bool EditModel::arrHas(const OVal* a, const QString& id) {
    if (!a || !a->isArr()) return false;
    for (const OVal& v : a->items()) if (v.isStr() && v.asStr() == id) return true;
    return false;
}

// ---------------- 読み込み ----------------
void EditModel::ensureTemplate() {
    auto setdef = [&](const char* k, const OVal& v) { if (!p_.has(k)) p_.set(k, v); };
    setdef("version", OVal::num(1));
    setdef("source", OVal::str(""));
    setdef("timebase", OVal::str("source"));
    {
        OVal audio = OVal::object(), chain = OVal::array();
        OVal a = OVal::object(); a.set("type", OVal::str("declick"));
        OVal pr = OVal::object(); pr.set("threshold", OVal::num(8)); a.set("params", pr); chain.push(a);
        OVal g = OVal::object(); g.set("type", OVal::str("gain")); g.set("db", OVal::num(18)); chain.push(g);
        OVal n = OVal::object(); n.set("type", OVal::str("normalize")); n.set("peak_db", OVal::num(-3)); chain.push(n);
        audio.set("chain", chain);
        setdef("audio", audio);
    }
    for (const char* k : {"splits", "cuts", "gaps", "vmute", "amute", "aslip", "telops", "topics", "overlays", "sfx"}) setdef(k, OVal::array());
    {
        OVal cf = OVal::object();
        cf.set("audio_fade_ms", OVal::num(8)); cf.set("video", OVal::str("none")); cf.set("dur", OVal::num(0.12));
        setdef("cut_fx", cf);
    }
    setdef("bgm", OVal::null());
    setdef("styles", OVal::object());
    {
        OVal o = OVal::object(); o.set("file", OVal::str("out/final.mp4")); o.set("fps", OVal::null());
        setdef("output", o);
    }
}

double EditModel::sfxFull(const OVal& it) const {
    const QString f = it.strOr("file", "");
    if (f.isEmpty()) return -1;
    auto i = sfxDur_.find(f);
    if (i != sfxDur_.end()) return i.value();
    const vn::MediaProps m = vn::probeMedia(resolve(f));
    const double d = m.ok && m.duration > 0 ? m.duration : -1;
    sfxDur_.insert(f, d);
    return d;
}
double EditModel::sfxLen(const OVal& it) const {
    const double full = sfxFull(it);
    const OVal* in = it.get("in");
    double iv = in && in->isNum() ? in->asNum() : 0;
    if (iv != iv) iv = 0;
    return std::max(0.05, (full >= 0 ? full : 1) - iv);
}
bool EditModel::sfxAuto(const OVal& it) const {
    if (sfxFull(it) < 0) return false;
    return std::fabs(it.numOr("end", NaN) - (it.numOr("start", NaN) + sfxLen(it))) < 2e-3;
}

void EditModel::normalize() {
    auto mapArr = [&](const char* key, const std::function<OVal(const OVal&)>& f) {
        OVal* a = p_.get(key);
        if (!a || !a->isArr()) { p_.set(key, OVal::array()); return; }
        OVal out = OVal::array();
        for (const OVal& v : a->items()) out.push(f(v));
        p_.set(key, out);
    };
    mapArr("cuts", [](const OVal& c) { return numArr({parseT(c.isArr() && c.size() > 0 ? c.at(0) : OVal()), parseT(c.isArr() && c.size() > 1 ? c.at(1) : OVal())}); });
    mapArr("splits", [](const OVal& v) { return OVal::num(parseT(v)); });
    if (!p_.get("sfx") || !p_.get("sfx")->isArr()) p_.set("sfx", OVal::array());
    OVal& sfx = *p_.get("sfx");
    for (OVal& it : sfx.items()) {
        const OVal* s = it.get("start");
        it.set("start", OVal::num(parseT(s ? *s : OVal())));
        const OVal* in = it.get("in");
        it.set("in", OVal::num(in && !in->isNull() ? parseT(*in) : 0));
        const OVal* e = it.get("end");
        if (!e || e->isNull()) it.set("end", OVal::num(it.numOr("start", 0) + sfxLen(it)));
    }
    for (const char* k : {"overlays", "topics", "telops", "sfx"}) {
        OVal* a = p_.get(k);
        if (!a || !a->isArr()) continue;
        for (OVal& it : a->items()) {
            const OVal* s = it.get("start");
            it.set("start", OVal::num(parseT(s ? *s : OVal())));
            const OVal* e = it.get("end");
            it.set("end", OVal::num(parseT(e ? *e : OVal())));
        }
    }
}

OVal EditModel::readParsed(QString* err, double* mtime) const {
    OVal v;
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) { if (err) *err = "開けません: " + path_; return OVal(); }
    const QByteArray b = f.readAll();
    f.close();
    if (mtime) *mtime = fileMtime(path_);
    if (!OVal::parse(b, &v, err) || !v.isObj()) { if (err && err->isEmpty()) *err = "project.json の形が不正です"; return OVal(); }
    return v;
}

bool EditModel::loadFile(QString* err) {
    double mt = 0;
    OVal v = readParsed(err, &mt);
    if (!v.isObj()) return false;
    p_ = v;
    ensureTemplate();
    c_ = Cache();
    sfxDur_.clear();
    const vn::MediaProps m = vn::probeMedia(resolve(p_.strOr("source", "")));
    dur_ = m.ok && m.duration > 0 ? m.duration : 60;
    fps_ = m.ok && m.fps > 0 ? m.fps : 30;
    normalize();
    lastMtime_ = mt;
    dirty_ = false;
    invalidate();
    return true;
}

bool EditModel::open(const QString& path, QString* err) {
    path_ = QFileInfo(path).absoluteFilePath();
    QString e;
    if (!loadFile(&e)) { if (err) *err = e; return false; }
    undoS_.clear(); redoS_.clear(); prevStr_.clear(); prevVal_ = OVal();
    histBase = QStringLiteral("開く");
    loadLocks();
    clearSel();
    tcur_ = 0;
    setSaveState(QStringLiteral("保存済み"));
    emit changed();
    return true;
}

bool EditModel::reloadFromDisk(QString* err) {
    QString e;
    if (!loadFile(&e)) { if (err) *err = e; return false; }
    undoS_.clear(); redoS_.clear(); prevStr_.clear(); prevVal_ = OVal();
    histBase = QStringLiteral("外部の変更を取り込み");
    clearSel();
    tcur_ = clampd(tcur_, 0, dur_);
    setSaveState(QStringLiteral("保存済み"));
    emit changed();
    return true;
}

// ---------------- 時刻の計算 ----------------
void EditModel::invalidate() { c_ = Cache(); }

const Rg& EditModel::mergedCuts() {
    if (c_.mcOk) return c_.mc;
    Rg cs;
    for (const OVal& c : p_.get("cuts")->items()) {
        const double a = c.size() > 0 ? c.at(0).asNum(NaN) : NaN, b = c.size() > 1 ? c.at(1).asNum(NaN) : NaN;
        cs.push_back({std::min(a, b), std::max(a, b)});
    }
    std::stable_sort(cs.begin(), cs.end(), [](auto& x, auto& y) { return x.first < y.first; });
    Rg out;
    for (auto& c : cs) {
        if (!out.empty() && c.first <= out.back().second) out.back().second = std::max(out.back().second, c.second);
        else out.push_back(c);
    }
    c_.mc = out;
    c_.mcOk = true;
    return c_.mc;
}

bool EditModel::cutAt(double t, std::pair<double, double>* out) {
    const Rg& mc = mergedCuts();
    const int i = lowerIdx((int)mc.size(), [&](int j) { return mc[j].second > t; });
    if (i < (int)mc.size() && t >= mc[i].first && t < mc[i].second) { if (out) *out = mc[i]; return true; }
    return false;
}

const Rg& EditModel::gapsN() {
    if (c_.gapsOk) return c_.gaps;
    const Rg& mc = mergedCuts();
    std::map<double, double> m;
    for (const OVal& g : p_.get("gaps")->items()) {
        if (!g.isArr() || g.size() < 2) continue;
        const double d = g.at(1).asNum(NaN);
        if (!(d >= 0.02)) continue;
        double at = clampd(g.at(0).asNum(NaN), 0, dur_);
        for (auto& c : mc) if (c.first <= at && at < c.second) at = c.second;
        at = jsRound(at * 1e4) / 1e4;
        m[at] = (m.count(at) ? m[at] : 0) + d;
    }
    Rg out(m.begin(), m.end());
    c_.gaps = out;
    c_.gapsOk = true;
    return c_.gaps;
}

double EditModel::outTime(double t) {
    const Rg& mc = mergedCuts();
    if (!c_.mcPreOk) { double c = 0; c_.mcPre = {0}; for (auto& x : mc) c_.mcPre.push_back(c += x.second - x.first); c_.mcPreOk = true; }
    const Rg& G = gapsN();
    if (!c_.gapPreOk) { double c = 0; c_.gapPre = {0}; for (auto& x : G) c_.gapPre.push_back(c += x.second); c_.gapPreOk = true; }
    const int k = lowerIdx((int)mc.size(), [&](int i) { return t <= mc[i].first; });
    double c = k ? c_.mcPre[k - 1] + (std::min(t, mc[k - 1].second) - mc[k - 1].first) : 0;
    const int g = lowerIdx((int)G.size(), [&](int i) { return G[i].first > t + 1e-9; });
    for (int i = 0; i < g; ++i) c -= G[i].second;
    return t - c;
}

double EditModel::outEnd(double t) {
    double s = 0;
    for (auto& g : gapsN()) s += std::fabs(g.first - t) <= 1e-6 ? g.second : 0;
    return outTime(t) - s;
}

double EditModel::inDelta(const OVal& it, double a, double b) { return isGapIt(it) ? b - a : outTime(b) - outTime(a); }

const std::vector<Piece>& EditModel::pieces() {
    if (c_.piecesOk) return c_.pieces;
    const Rg& mc = mergedCuts();
    const Rg& G = gapsN();
    std::vector<Piece> out;
    size_t gi = 0;
    double pos = 0, o = 0;
    Rg keeps;
    for (auto& c : mc) { if (c.first > pos) keeps.push_back({pos, std::min(c.first, dur_)}); pos = std::max(pos, c.second); }
    if (pos < dur_) keeps.push_back({pos, dur_});
    auto push = [&](Piece x) { x.o = o; o += x.span(); out.push_back(x); };
    auto pushGap = [&](size_t i) { Piece x; x.gap = true; x.at = G[i].first; x.len = G[i].second; push(x); };
    auto pushSeg = [&](double s, double e) { Piece x; x.s = s; x.e = e; push(x); };
    for (auto k : keeps) {
        double a = k.first, b = k.second;
        while (gi < G.size() && G[gi].first <= a + 1e-6) pushGap(gi++);
        while (gi < G.size() && G[gi].first < b - 1e-6) { pushSeg(a, G[gi].first); a = G[gi].first; pushGap(gi++); }
        if (b - a > 1e-4) pushSeg(a, b);
    }
    while (gi < G.size()) pushGap(gi++);
    c_.pieces = out;
    c_.piecesOk = true;
    return c_.pieces;
}

double EditModel::totalOut() {
    if (c_.totOk) return c_.tot;
    double cutSum = 0;
    for (auto& c : mergedCuts()) cutSum += std::min(c.second, dur_) - std::max(0.0, c.first);
    double gs = 0;
    for (auto& g : gapsN()) gs += g.second;
    c_.tot = dur_ - cutSum + gs;
    c_.totOk = true;
    return c_.tot;
}

int EditModel::pieceIdx(double o) {
    const auto& ps = pieces();
    return lowerIdx((int)ps.size(), [&](int j) { return o < ps[j].o + ps[j].span(); });
}
const Piece* EditModel::gapPieceAt(double o) {
    const auto& ps = pieces();
    const int i = pieceIdx(o);
    if (i < (int)ps.size() && ps[i].gap && o >= ps[i].o) return &ps[i];
    return nullptr;
}
double EditModel::srcOf(double o, bool coll) {
    if (!coll) return o;
    const auto& ps = pieces();
    const int i = pieceIdx(o);
    if (i >= (int)ps.size()) return dur_;
    const Piece& p = ps[i];
    return p.gap ? p.at : p.s + std::max(0.0, o - p.o);
}

double EditModel::gapKey(double at0) {
    double at = clampd(at0, 0, dur_);
    for (auto& c : mergedCuts()) if (c.first <= at && at < c.second) at = c.second;
    return jsRound(at * 1e4) / 1e4;
}
bool EditModel::gapOf(const OVal& it, double* at, double* d, double* o) {
    const double a = gapKey(it.numOr("gap_at", 0));
    for (const Piece& p : pieces())
        if (p.gap && std::fabs(p.at - a) < 1e-6) { if (at) *at = p.at; if (d) *d = p.len; if (o) *o = p.o; return true; }
    return false;
}

std::vector<std::pair<double, double>> EditModel::segments() {
    std::set<double> bs = {0, dur_};
    for (const OVal& s : p_.get("splits")->items()) bs.insert(s.asNum(NaN));
    for (auto& c : mergedCuts()) { bs.insert(c.first); bs.insert(c.second); }
    for (auto& g : gapsN()) bs.insert(g.first);
    std::vector<double> b;
    for (double t : bs) if (t >= 0 && t <= dur_) b.push_back(t);
    std::vector<std::pair<double, double>> out;
    for (size_t n = 0; n + 1 < b.size(); ++n)
        if (b[n + 1] - b[n] > 1e-4 && !cutAt((b[n] + b[n + 1]) / 2)) out.push_back({b[n], b[n + 1]});
    return out;
}

void EditModel::itRange(const OVal& it, bool coll, double* x0, double* x1) {
    const double s = it.numOr("start", 0), e = it.numOr("end", 0);
    if (isGapIt(it)) {   // 秒で返す。縮めない表示/見つからない時は呼び出し側が ±px を足す(x0==x1)
        double at = 0, d = 0, o = 0;
        if (!gapOf(it, &at, &d, &o)) { *x0 = *x1 = gapKey(it.numOr("gap_at", 0)); return; }
        if (!coll) { *x0 = *x1 = at; return; }
        *x0 = o + s; *x1 = o + std::min(e, d);
        return;
    }
    if (coll) { *x0 = outTime(s); *x1 = outEnd(e); }
    else { *x0 = s; *x1 = e; }
}

// ---------------- トラック ----------------
int EditModel::trackOf(const OVal& it) {
    const double t = it.numOr("track", 0);
    const double r = t == t && t != 0 ? jsRound(t) : 2;
    return (int)std::max(2.0, r);
}
int EditModel::vCount() const {
    int n = 1;
    const OVal* vt = p_.get("vtracks");
    if (vt && vt->isArr()) n = std::max(n, vt->size());
    for (const OVal& it : p_.get("overlays")->items()) n = std::max(n, trackOf(it) - 1);
    return n;
}
QString EditModel::vName(int n) const {
    const OVal* v = p_.get("vtracks");
    if (v && v->isArr()) { const int i = n - 2; return i >= 0 && i < v->size() && v->at(i).isStr() ? v->at(i).asStr() : QString(); }
    return n == 2 ? QStringLiteral("差込") : QString();
}
std::vector<LaneDef> EditModel::lanes() const {
    std::vector<LaneDef> L;
    for (int n = vCount() + 1; n >= 2; --n) L.push_back({"v" + QString::number(n), ("V" + QString::number(n) + " " + vName(n)).trimmed(), 34, "overlays", n});
    L.push_back({"tb", "TB 帯", 28, "topics", 0});
    L.push_back({"t", "T 字幕", 34, "telops", 0});
    L.push_back({"v1", "V1 映像", 34, "", 0});
    L.push_back({"a1", "A1 音声", 60, "", 0});
    L.push_back({"se", "SE 効果音", 34, "sfx", 0});
    L.push_back({"bgm", "BGM", 34, "", 0});
    return L;
}
QString EditModel::laneOf(const QString& kind, const OVal& it) const {
    if (kind == "overlays") return "v" + QString::number(trackOf(it));
    if (kind == "sfx" && it.strOr("kind", "") == "bgm") return "bgm";
    if (kind == "topics") return "tb";
    if (kind == "telops") return "t";
    if (kind == "sfx") return "se";
    return QString();
}
QStringList EditModel::sameSide(const QString& id) const {
    QStringList r;
    for (auto& l : lanes()) if (isAudioLane(l.id) == isAudioLane(id)) r << l.id;
    return r;
}
void EditModel::setLocks(const QStringList& ids, bool on) {
    for (auto& id : ids) { if (on) locks_.insert(id); else locks_.remove(id); }
    saveLocks();
    for (const QString& k : QSet<QString>(selSet)) {
        const int c = k.indexOf(':');
        const QString kind = k.left(c);
        const int j = k.mid(c + 1).toInt();
        if (kind == "cuts") continue;
        const OVal* a = p_.get(kind);
        if (a && j < a->size() && isLocked(laneOf(kind, a->at(j)))) selSet.remove(k);
    }
    if (!selKind.isEmpty() && selKind != "cuts") {
        const OVal* a = p_.get(selKind);
        if (a && selI >= 0 && selI < a->size() && isLocked(laneOf(selKind, a->at(selI)))) { selKind.clear(); selI = -1; }
    }
    if (isLocked("v1") || isLocked("a1")) { segSel.clear(); gapSel.clear(); trkSel.clear(); }
    if (isLocked("bgm")) bgmSel = false;
    emit selectionChanged();
    emit changed();
}
void EditModel::toggleLock(const QString& id, bool all) { setLocks(all ? sameSide(id) : QStringList{id}, !isLocked(id)); }
void EditModel::setTrackFlag(const QString& key, const QStringList& ids, bool on) {
    mutate([&] {
        std::vector<QString> cur;   // Set の挿入順
        if (const OVal* a = p_.get(key); a && a->isArr()) for (const OVal& v : a->items()) if (v.isStr() && std::find(cur.begin(), cur.end(), v.asStr()) == cur.end()) cur.push_back(v.asStr());
        for (const QString& id : ids) {
            if (on) { if (std::find(cur.begin(), cur.end(), id) == cur.end()) cur.push_back(id); }
            else cur.erase(std::remove(cur.begin(), cur.end(), id), cur.end());
        }
        if (!cur.empty()) { OVal a = OVal::array(); for (auto& s : cur) a.push(OVal::str(s)); p_.set(key, a); }
        else p_.remove(key);
    });
}
void EditModel::toggleHide(const QString& id, bool all) { setTrackFlag("hide", all ? sameSide(id) : QStringList{id}, !isHidden(id)); }
void EditModel::toggleMute(const QString& id, bool all) { setTrackFlag("mute", all ? sameSide(id) : QStringList{id}, !isMuted(id)); }

// ---------------- 再生ヘッド・選択 ----------------
void EditModel::seek(double t) {
    tcur_ = clampd(t, 0, dur_);
    outHint_ = -1;
    emit playheadChanged(tcur_);
}

void EditModel::seekOut(double o) {
    if (!collapsed) { seek(o); return; }
    tcur_ = clampd(srcOf(std::max(0.0, o), true), 0, dur_);
    outHint_ = std::max(0.0, o);
    emit playheadChanged(tcur_);
}

double EditModel::tout() {
    if (!collapsed) return tcur_;
    if (outHint_ >= 0 && std::fabs(srcOf(outHint_, true) - tcur_) < 1e-6) return outHint_;
    return outTime(tcur_);
}

void EditModel::select(const QString& kind, int i) {
    if (kind.isEmpty()) { selKind.clear(); selI = -1; selSet.clear(); }
    else { selKind = kind; selI = i; selSet = {kind + ":" + QString::number(i)}; segSel.clear(); gapSel.clear(); }
    bgmSel = false;
    trkSel.clear();
    emit selectionChanged();
}
void EditModel::toggleSel(const QString& kind, int i) {
    const QString key = kind + ":" + QString::number(i);
    if (selSet.contains(key)) selSet.remove(key); else selSet.insert(key);
    // JS の Set は挿入順。最後に足したものを sel にする(外す時は残りの任意の1つ)
    if (selSet.contains(key)) { selKind = kind; selI = i; }
    else if (!selSet.isEmpty()) { const QString k = *selSet.begin(); selKind = k.left(k.indexOf(':')); selI = k.mid(k.indexOf(':') + 1).toInt(); }
    else { selKind.clear(); selI = -1; }
    emit selectionChanged();
}
void EditModel::clearSel() {
    selKind.clear(); selI = -1; selSet.clear();
    segSel.clear(); gapSel.clear(); bgmSel = false; trkSel.clear();
    emit selectionChanged();
}
void EditModel::selectAll() {
    selSet.clear();
    for (const char* k : {"overlays", "topics", "telops", "sfx"}) {
        const OVal& a = *p_.get(k);
        for (int i = 0; i < a.size(); ++i) if (!isLocked(laneOf(k, a.at(i)))) selSet.insert(QString(k) + ":" + QString::number(i));
    }
    emit selectionChanged();
}
bool EditModel::hasAnySel() const { return !selSet.isEmpty() || !segSel.empty() || !gapSel.empty() || bgmSel || !trkSel.isEmpty(); }

// ---------------- 編集の土台 ----------------
void EditModel::snap() {
    undoS_.push_back(p_.dump(false));
    prevVal_ = p_;              // resolveOverlaps が控えを読み直さずに済むように(写しは 0.1ms、読み直しは 3ms)
    prevStr_ = undoS_.back();   // 同じ控えかは文字列の中身の置き場で見る。持っている間は置き場が使い回されない
    if (undoS_.size() > 200) { undoS_.erase(undoS_.begin()); histBase = QStringLiteral("(これより前は消えました)"); }
    redoS_.clear();
}

void EditModel::sortList(const QString& k) {
    OVal* a = p_.get(k);
    if (!a || !a->isArr()) return;
    std::stable_sort(a->items().begin(), a->items().end(), [](const OVal& x, const OVal& y) { return x.numOr("start", 0) < y.numOr("start", 0); });
}

void EditModel::resolveOverlaps() {
    if (undoS_.empty()) return;
    OVal parsed;
    const bool same = undoS_.back().constData() == prevStr_.constData() && undoS_.back().size() == prevStr_.size();
    if (!same && !OVal::parse(undoS_.back().toUtf8(), &parsed)) return;
    const OVal& prev = same ? prevVal_ : parsed;
    auto key = [&](const QString& k, const OVal& it) {
        return laneOf(k, it) + "|" + QString::number(it.numOr("start", NaN), 'f', 3) + "|" + QString::number(it.numOr("end", NaN), 'f', 3);
    };
    auto fin = [&](const OVal& it) {
        const double s = it.numOr("start", NaN), e = it.numOr("end", NaN);
        return !isGapIt(it) && std::isfinite(s) && std::isfinite(e) && e > s;
    };
    static const QRegularExpression imgRe(R"(\.(png|jpe?g|webp|bmp|gif)$)", QRegularExpression::CaseInsensitiveOption);
    // 選択をあとで引き直すための印
    std::map<QString, std::vector<int>> remap;
    bool changed = false;
    for (const char* kk : {"overlays", "telops", "topics", "sfx"}) {
        const QString k = kk;
        QSet<QString> old;
        if (const OVal* pa = prev.get(k); pa && pa->isArr()) for (const OVal& it : pa->items()) old.insert(key(k, it));
        OVal& cur = *p_.get(k);
        struct E { OVal v; int orig; };
        std::vector<E> list;
        for (int i = 0; i < cur.size(); ++i) list.push_back({cur.at(i), i});
        std::vector<int> placed;
        for (int i = 0; i < (int)list.size(); ++i) if (fin(list[i].v) && !old.contains(key(k, list[i].v))) placed.push_back(i);
        if (placed.empty()) continue;
        std::set<int> newOnes(placed.begin(), placed.end()), drop;
        std::vector<OVal> add;
        for (int ei : placed) {
            const QString ln = laneOf(k, list[ei].v);
            const double ea = list[ei].v.numOr("start", 0), eb = list[ei].v.numOr("end", 0);
            if (isLocked(ln)) continue;
            for (int oi = 0; oi < (int)list.size(); ++oi) {
                OVal& o = list[oi].v;
                if (newOnes.count(oi) || drop.count(oi) || !fin(o) || laneOf(k, o) != ln) continue;
                const double oa = o.numOr("start", 0), ob = o.numOr("end", 0);
                if (ob <= ea + 1e-6 || oa >= eb - 1e-6) continue;
                const QString of = o.strOr("file", "");
                const bool media = k == "sfx" || (k == "overlays" && !of.isEmpty() && !imgRe.match(of).hasMatch());
                auto tail = [&]() {
                    OVal t = o;
                    t.set("start", OVal::num(tf3(eb)));
                    if (media) t.set("in", OVal::num(tf3(o.numOr("in", 0) + inDelta(o, oa, eb))));
                    return t;
                };
                if (oa >= ea - 1e-6 && ob <= eb + 1e-6) drop.insert(oi);
                else if (oa < ea && ob > eb) { add.push_back(tail()); o.set("end", OVal::num(tf3(ea))); }
                else if (oa < ea) o.set("end", OVal::num(tf3(ea)));
                else o = tail();
                changed = true;
            }
        }
        if (changed) {
            std::vector<E> nl;
            for (int i = 0; i < (int)list.size(); ++i) if (!drop.count(i)) nl.push_back(list[i]);
            for (auto& a : add) nl.push_back({a, -1});
            std::stable_sort(nl.begin(), nl.end(), [](const E& x, const E& y) { return x.v.numOr("start", 0) < y.v.numOr("start", 0); });
            OVal arr = OVal::array();
            std::vector<int> map(list.size(), -1);
            for (int i = 0; i < (int)nl.size(); ++i) { arr.push(nl[i].v); if (nl[i].orig >= 0) map[nl[i].orig] = i; }
            p_.set(k, arr);
            remap[k.toStdString().c_str()] = map;
        }
    }
    if (!changed) return;
    QSet<QString> ns;
    for (const QString& s : selSet) {
        const int c = s.indexOf(':');
        const QString kind = s.left(c);
        const int j = s.mid(c + 1).toInt();
        auto it = remap.find(kind);
        if (it == remap.end()) { ns.insert(s); continue; }
        if (j >= 0 && j < (int)it->second.size() && it->second[j] >= 0) ns.insert(kind + ":" + QString::number(it->second[j]));
    }
    selSet = ns;
    if (!selKind.isEmpty()) {
        auto it = remap.find(selKind);
        if (it != remap.end()) {
            if (selI >= 0 && selI < (int)it->second.size() && it->second[selI] >= 0) selI = it->second[selI];
            else { selKind.clear(); selI = -1; }
        }
    }
    emit toast(QStringLiteral("重なった素材を詰めました(同じトラックには重ねません)"));
}

void EditModel::commit(bool keepOverlap) {
    if (!keepOverlap) resolveOverlaps();
    invalidate();
    dirty_ = true;
    setSaveState(QStringLiteral("未保存"));
    if (autosaveOn_) saveTimer_.start(500);
    emit changed();
}

void EditModel::mutate(const std::function<void()>& fn) {
    snap();
    fn();
    commit();
}

void EditModel::doUndo(std::vector<QString>& from, std::vector<QString>& to) {
    if (from.empty()) return;
    to.push_back(p_.dump(false));
    OVal v;
    if (from.back().constData() == prevStr_.constData() && from.back().size() == prevStr_.size()) v = prevVal_;   // 直前の控えなら読み直さない
    else if (!OVal::parse(from.back().toUtf8(), &v)) return;
    from.pop_back();
    p_ = v;
    selKind.clear(); selI = -1; selSet.clear(); segSel.clear(); gapSel.clear(); bgmSel = false; trkSel.clear();
    emit selectionChanged();
    commit(true);
}
bool EditModel::undo() { if (undoS_.empty()) return false; doUndo(undoS_, redoS_); return true; }
bool EditModel::redo() { if (redoS_.empty()) return false; doUndo(redoS_, undoS_); return true; }

// ---------------- 編集操作 ----------------
static bool isImagePath(const QString& f) {
    static const QRegularExpression re(R"(\.(png|jpe?g|webp|bmp|gif)$)", QRegularExpression::CaseInsensitiveOption);
    return re.match(f).hasMatch();
}

bool EditModel::splitAll(double t) {
    t = tf3(clampd(t, 0, dur_));
    bool splitHit = false;
    for (const OVal& s : p_.get("splits")->items()) if (std::fabs(s.asNum(NaN) - t) < 0.02) splitHit = true;
    const bool av = !(isLocked("v1") && isLocked("a1")) && !cutAt(t) && t > 0.02 && t < dur_ - 0.02 && !splitHit;
    std::vector<std::pair<QString, int>> hits;
    for (const char* kk : {"overlays", "topics", "telops", "sfx"}) {
        const OVal& a = *p_.get(kk);
        for (int i = 0; i < a.size(); ++i) {
            const OVal& it = a.at(i);
            if (isGapIt(it)) continue;
            if (!isLocked(laneOf(kk, it)) && it.numOr("start", 0) < t - 0.02 && it.numOr("end", 0) > t + 0.02) hits.push_back({kk, i});
        }
    }
    if (!av && hits.empty()) {
        bool all = true;
        for (auto& l : lanes()) if (!(isLocked(l.id) || l.id == "bgm")) all = false;
        if (all) emit toast(QStringLiteral("すべてロック中です"));
        return false;
    }
    mutate([&] {
        if (av) {
            OVal& sp = *p_.get("splits");
            sp.push(OVal::num(t));
            std::stable_sort(sp.items().begin(), sp.items().end(), [](const OVal& x, const OVal& y) { return x.asNum(0) < y.asNum(0); });
        }
        for (auto& h : hits) {
            OVal& arr = *p_.get(h.first);
            OVal b = arr.at(h.second);
            OVal& it = arr.at(h.second);
            b.set("start", OVal::num(t));
            it.set("end", OVal::num(t));
            const bool media = h.first == "sfx" || (h.first == "overlays" && !it.strOr("file", "").isEmpty() && !isImagePath(it.strOr("file", "")));
            if (media) b.set("in", OVal::num(tf3(it.numOr("in", 0) + inDelta(it, it.numOr("start", 0), t))));
            if (h.first == "overlays") { b.remove("in_anim"); it.remove("out_anim"); }
            arr.push(b);
        }
        for (const char* k : {"overlays", "topics", "telops", "sfx"}) sortList(k);
    });
    clearSel();
    return true;
}

void EditModel::cutRange(double a, double b) {
    if (isLocked("v1") && isLocked("a1")) { emit toast(QStringLiteral("V1/A1 はロック中です")); return; }
    if (b - a > 0.02) mutate([&] { p_.get("cuts")->push(numArr({tf3(a), tf3(b)})); });
}

bool EditModel::doMarkX() {
    if (markIn >= 0 && markOut >= 0) {
        const double a = std::min(markIn, markOut), b = std::max(markIn, markOut);
        markIn = markOut = -1;
        cutRange(a, b);
        return true;
    }
    emit toast(QStringLiteral("イン(I)とアウト(O)を打ってください"));
    return false;
}

void EditModel::normGaps() {
    invalidate();
    OVal ng = OVal::array();
    for (auto& g : gapsN()) if (g.first < dur_ - 1e-3) ng.push(numArr({tf3(g.first), tf3(g.second)}));
    p_.set("gaps", ng);
    invalidate();
}

bool EditModel::editDelete(bool ripple) {
    const std::vector<double> gaps = gapSel;
    QStringList keys;
    if (!selSet.isEmpty()) {
        // JS の Set は挿入順。ここでは添字の降順に消すので順序は結果に影響しない
        keys = QStringList(selSet.begin(), selSet.end());
    } else if (!selKind.isEmpty()) keys << selKind + ":" + QString::number(selI);
    const bool dropBgm = bgmSel && !p_.get("bgm")->isNull();
    const QStringList tk = trkSel;
    std::vector<std::pair<double, double>> both, vOnly, aOnly;
    auto nearD = [](double x, double y) { return std::fabs(x - y) < 1e-6; };
    for (const SegSel& q : segSel) {
        bool dup = false;
        for (auto* v : {&both, &vOnly, &aOnly}) for (auto& x : *v) if (nearD(x.first, q.a) && nearD(x.second, q.b)) dup = true;
        if (dup) continue;
        bool v = false, a = false;
        for (const SegSel& x : segSel) if (nearD(x.a, q.a) && nearD(x.b, q.b)) { if (x.trk.isNull() || x.trk == 'v') v = true; if (x.trk.isNull() || x.trk == 'a') a = true; }
        (v && a ? both : v ? vOnly : aOnly).push_back({q.a, q.b});
    }
    const auto& segs = both;
    if (segs.empty() && vOnly.empty() && aOnly.empty() && gaps.empty() && keys.isEmpty() && !dropBgm && tk.isEmpty()) return false;
    if (ripple && (!vOnly.empty() || !aOnly.empty())) emit toast(QStringLiteral("片方だけの削除は詰められないので、黒/無音にしました"));
    if ((!segs.empty() || !gaps.empty()) && isLocked("v1") && isLocked("a1")) { emit toast(QStringLiteral("V1/A1 はロック中です")); return true; }
    segSel.clear(); gapSel.clear(); bgmSel = false; trkSel.clear(); selKind.clear(); selI = -1; selSet.clear();
    emit selectionChanged();
    mutate([&] {
        for (const char* k : {"vmute", "amute", "aslip"}) if (!p_.get(k) || !p_.get(k)->isArr()) p_.set(k, OVal::array());
        struct KJ { QString k; int j; };
        auto parse = [](const QStringList& l) {
            std::vector<KJ> v;
            for (auto& s : l) v.push_back({s.left(s.indexOf(':')), s.mid(s.indexOf(':') + 1).toInt()});
            std::stable_sort(v.begin(), v.end(), [](const KJ& x, const KJ& y) { return x.j > y.j; });
            return v;
        };
        const auto tkn = parse(tk);
        for (auto& e : tkn) {
            if (e.k != "aslip") continue;
            const OVal& it = p_.get(e.k)->at(e.j);
            p_.get("amute")->push(numArr({it.numOr("a", 0), it.numOr("b", 0)}));
        }
        for (auto& e : tkn) p_.get(e.k)->erase(e.j);
        for (auto& r : vOnly) p_.get("vmute")->push(numArr({tf3(r.first), tf3(r.second)}));
        for (auto& r : aOnly) {
            p_.get("amute")->push(numArr({tf3(r.first), tf3(r.second)}));
            OVal ns = OVal::array();
            for (const OVal& x : p_.get("aslip")->items()) if (!(nearD(x.numOr("a", NaN), r.first) && nearD(x.numOr("b", NaN), r.second))) ns.push(x);
            p_.set("aslip", ns);
        }
        if (dropBgm) p_.set("bgm", OVal::null());
        for (auto& e : parse(keys)) p_.get(e.k)->erase(e.j);
        invalidate();
        OVal ng = OVal::array();
        for (auto& g : gapsN()) {
            bool sel = false;
            for (double x : gaps) if (std::fabs(x - g.first) < 1e-6) sel = true;
            if (!sel) ng.push(numArr({g.first, g.second}));
        }
        p_.set("gaps", ng);
        for (auto& s : segs) {
            p_.get("cuts")->push(numArr({tf3(s.first), tf3(s.second)}));
            if (!ripple && s.second < dur_ - 1e-3) p_.get("gaps")->push(numArr({tf3(s.second), tf3(s.second - s.first)}));
        }
        normGaps();
    });
    return true;
}

std::vector<double> EditModel::edgePoints() {
    std::vector<double> pts = {0, dur_};
    for (const OVal& s : p_.get("splits")->items()) pts.push_back(s.asNum(NaN));
    for (auto& c : mergedCuts()) { pts.push_back(c.first); pts.push_back(c.second); }
    for (const char* k : {"overlays", "topics", "telops", "sfx"})
        for (const OVal& it : p_.get(k)->items()) if (!isGapIt(it)) { pts.push_back(it.numOr("start", NaN)); pts.push_back(it.numOr("end", NaN)); }
    std::vector<double> out;
    for (double t : pts) if (t >= 0 && t <= dur_) out.push_back(t);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<double> EditModel::trimPoints() {
    std::vector<double> pts = {0, dur_};
    for (const OVal& s : p_.get("splits")->items()) pts.push_back(s.asNum(NaN));
    for (auto& c : mergedCuts()) { pts.push_back(c.first); pts.push_back(c.second); }
    for (auto& g : gapsN()) pts.push_back(g.first);
    for (const char* k : {"overlays", "topics", "telops", "sfx"})
        for (const OVal& it : p_.get(k)->items())
            if (!isGapIt(it) && !isLocked(laneOf(k, it))) { pts.push_back(it.numOr("start", NaN)); pts.push_back(it.numOr("end", NaN)); }
    std::vector<double> out;
    for (double t : pts) if (t >= 0 && t <= dur_) out.push_back(t);
    return out;
}

void EditModel::trimTo(int dir) {
    const double t = tcur_, o = outTime(t);
    std::vector<std::pair<double, double>> pts;
    for (double x : trimPoints()) pts.push_back({x, outTime(x)});
    if (dir < 0) {
        const std::pair<double, double>* a = nullptr;
        for (auto& p : pts) if (p.second < o - 0.02 && (!a || p.second > a->second || (p.second == a->second && p.first > a->first))) a = &p;
        if (a) { const double ax = a->first; cutRange(ax, t); seek(ax); }
    } else {
        const std::pair<double, double>* b = nullptr;
        for (auto& p : pts) if (p.second > o + 0.02 && (!b || p.second < b->second || (p.second == b->second && p.first < b->first))) b = &p;
        if (b) cutRange(t, b->first);
    }
}

void EditModel::nudge(int frames) {
    const double d = frames / fps_;
    std::vector<std::pair<QString, int>> keys;
    for (const QString& s : selSet) { const QString k = s.left(s.indexOf(':')); if (k != "cuts") keys.push_back({k, s.mid(s.indexOf(':') + 1).toInt()}); }
    if (keys.empty()) return;
    mutate([&] {
        for (auto& kj : keys) {
            OVal& it = p_.get(kj.first)->at(kj.second);
            double gd = 0;
            const bool g = isGapIt(it);
            const bool ok = g ? gapOf(it, nullptr, &gd, nullptr) : true;
            if (g && !ok) continue;
            const double s = it.numOr("start", 0), e = it.numOr("end", 0);
            const double dd = clampd(d, -s, (g ? gd : dur_) - e);
            it.set("start", OVal::num(tf3(s + dd)));
            it.set("end", OVal::num(tf3(e + dd)));
        }
    });
}

void EditModel::copySel() {
    clip_.clear();
    for (const QString& s : selSet) {
        const QString k = s.left(s.indexOf(':'));
        if (k == "cuts") continue;
        clip_.push_back({k, p_.get(k)->at(s.mid(s.indexOf(':') + 1).toInt())});
    }
    if (!clip_.empty()) emit toast(QString("%1 個をコピーしました").arg(clip_.size()));
}

void EditModel::pasteClip() {
    if (clip_.empty()) return;
    double t0 = 1e300;
    for (auto& c : clip_) t0 = std::min(t0, c.second.numOr("start", 0));
    std::vector<std::pair<QString, OVal>> added;
    mutate([&] {
        for (auto& c : clip_) {
            OVal it = c.second;
            const double len = it.numOr("end", 0) - it.numOr("start", 0);
            it.remove("gap_at");
            const double st = tf3(tcur_ + c.second.numOr("start", 0) - t0);
            it.set("start", OVal::num(st));
            it.set("end", OVal::num(tf3(std::min(dur_, st + len))));
            p_.get(c.first)->push(it);
            added.push_back({c.first, it});
        }
        for (const char* k : {"overlays", "topics", "telops", "sfx"}) sortList(k);
    });
    selSet.clear();
    for (auto& a : added) {
        const OVal& arr = *p_.get(a.first);
        for (int i = 0; i < arr.size(); ++i) if (arr.at(i) == a.second) { selSet.insert(a.first + ":" + QString::number(i)); selKind = a.first; selI = i; break; }
    }
    emit selectionChanged();
}

// ---------------- 保存 ----------------
void EditModel::setSaveState(const QString& s) {
    if (saveState_ == s) return;
    saveState_ = s;
    emit saveStateChanged(s);
}

OVal EditModel::projOut() const {
    bool any = false;
    const OVal* sfx = p_.get("sfx");
    if (sfx) for (const OVal& it : sfx->items()) if (sfxAuto(it)) any = true;
    if (!any) return p_;
    OVal c = p_;
    OVal* cs = c.get("sfx");
    for (int i = 0; i < cs->size(); ++i) if (sfxAuto(sfx->at(i))) cs->at(i).remove("end");
    return c;
}

QByteArray EditModel::serialize() const {
    QString s = projOut().dump(true);
    s.replace("\n", "\r\n");   // Windows の write_text(text モード)と同じ
    return s.toUtf8();
}

// project.py の save と同じく <名前>.tmp に書いて置き換える。書き切れなかった時は元のファイルに触らない
static bool writeFileSafe(const QString& path, const QByteArray& data, QString* err) {
    const QFileInfo fi(path);
    const QString tmp = fi.absolutePath() + "/" + fi.completeBaseName() + ".tmp";
#ifdef _WIN32
    // QFile::handle() の番号は CRT によって Win32 のハンドルに戻せないので、Win32 で直接書く
    const HANDLE h = CreateFileW((const wchar_t*)QDir::toNativeSeparators(tmp).utf16(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { *err = QStringLiteral("一時ファイルを作れません"); return false; }
    DWORD wrote = 0;
    bool ok = WriteFile(h, data.constData(), DWORD(data.size()), &wrote, nullptr) && wrote == DWORD(data.size())
              && FlushFileBuffers(h);   // 保存の直後に落ちても空のファイルが残らないように
    CloseHandle(h);
#else
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) { *err = f.errorString(); return false; }
    bool ok = f.write(data) == data.size() && f.flush();
    f.close();
    ok = ok && f.error() == QFileDevice::NoError;
#endif
    if (!ok) {
        *err = QStringLiteral("書き込めません(空き容量を確かめてください)");
        QFile::remove(tmp);
        return false;
    }
#ifdef _WIN32
    ok = MoveFileExW((const wchar_t*)QDir::toNativeSeparators(tmp).utf16(), (const wchar_t*)QDir::toNativeSeparators(path).utf16(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    QFile::remove(path);
    ok = QFile::rename(tmp, path);
#endif
    if (!ok) { *err = QStringLiteral("置き換えられません"); return false; }
    return true;
}

QString EditModel::writeBackup() const {
    const QFileInfo fi(path_);
    const QString bak = fi.absolutePath() + "/" + fi.completeBaseName() + ".local-bak.json";
    QString err;
    return writeFileSafe(bak, serialize(), &err) ? bak : QString();
}

// 戻れる対策: 一定の間隔で今の内容の写しを <名前>.backup/<名前>_<日時>.vedit.json に書く。新しい 30 個を残す
QString EditModel::writeTimedBackup() {
    if (path_.isEmpty()) return {};
    const QByteArray data = serialize();
    if (data == lastBackup_) return {};
    const QFileInfo fi(path_);
    const QString d = fi.absolutePath() + "/" + fi.completeBaseName() + ".backup";
    if (!QDir().mkpath(d)) return {};
    QString base = fi.completeBaseName();
    if (base.endsWith(QLatin1String(".vedit"))) base.chop(6);
    const QString p = d + "/" + base + "_" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".vedit.json";
    QString err;
    if (!writeFileSafe(p, data, &err)) return {};
    lastBackup_ = data;
    QStringList all = QDir(d).entryList({base + "_*.vedit.json"}, QDir::Files, QDir::Name);
    while (all.size() > 30) QFile::remove(d + "/" + all.takeFirst());
    return p;
}

EditModel::SaveResult EditModel::save(bool force) {
    saveTimer_.stop();
    if (!force && QFileInfo::exists(path_) && lastMtime_ != 0 && std::fabs(fileMtime(path_) - lastMtime_) > 1e-3) {
        emit conflict();
        return Conflict;
    }
    QString err;
    if (!writeFileSafe(path_, serialize(), &err)) { setSaveState("保存失敗: " + err); return Failed; }
    lastMtime_ = fileMtime(path_);
    dirty_ = false;
    setSaveState(QStringLiteral("保存済み"));
    return Saved;
}

void EditModel::startWatch(int ms) { watchTimer_.start(ms); }

bool EditModel::checkExternal() {
    if (dirty_) return false;   // 未保存の編集を上書きしない(保存する時に競合として聞く)
    if (!QFileInfo::exists(path_)) return false;
    if (fileMtime(path_) == lastMtime_) return false;
    QString err;
    if (!reloadFromDisk(&err)) return false;
    emit toast(QStringLiteral("外部の変更を取り込みました(元に戻す履歴はここから)"));
    emit externalLoaded();
    return true;
}

}  // namespace vui
