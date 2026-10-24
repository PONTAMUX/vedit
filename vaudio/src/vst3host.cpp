// vaudio: VST3 ホスト。処理の流れは pedalboard 0.9.26 の ExternalPlugin.h(GPL-3.0, Spotify AB)を
// VST3 SDK のホスティングクラスで置き換えて再現している。
//   - prepare(): 設定が変わったときだけ setupProcessing(kOffline)+setActive(true)+setProcessing(true)
//   - reset():   releaseResources 相当(setProcessing(false)/setActive(false))し、次の prepare でやり直す
//   - process(): samplesProvided を数え、max(0, provided - latency) を「使える出力」として返す
//   - パラメータ名・値の解釈は pedalboard/_pedalboard.py の AudioProcessorParameter と同じ
#include "vst3host.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/connectionproxy.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace vaudio {

namespace {

HostApplication& hostApp() {
    static HostApplication h;
    static bool init = false;
    if (!init) {
        PluginContextFactory::instance().setPluginContext(&h);
        init = true;
    }
    return h;
}

std::map<std::string, VST3::Hosting::Module::Ptr>& moduleCache() {
    static std::map<std::string, VST3::Hosting::Module::Ptr> m;
    return m;
}

// ---- IComponentHandler(エディタ操作はコントローラ側に既に反映されるので、通知を受けるだけ)
class HostHandler : public IComponentHandler {
public:
    tresult PLUGIN_API beginEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32) override { return kResultOk; }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IComponentHandler::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refs;
        if (r == 0) delete this;
        return r;
    }
private:
    std::atomic<uint32> refs{1};
};

class PlugFrame : public IPlugFrame {
public:
    std::function<void(int, int)> onResize;
    IPlugView* view = nullptr;
    tresult PLUGIN_API resizeView(IPlugView* v, ViewRect* r) override {
        if (!r) return kInvalidArgument;
        if (onResize) onResize(r->right - r->left, r->bottom - r->top);
        if (v) v->onSize(r);
        return kResultOk;
    }
    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) || FUnknownPrivate::iidEqual(iid, IPlugFrame::iid)) {
            *obj = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }
        *obj = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++refs; }
    uint32 PLUGIN_API release() override {
        uint32 r = --refs;
        if (r == 0) delete this;
        return r;
    }
private:
    std::atomic<uint32> refs{1};
};

// ---- pedalboard のパラメータ名・値の解釈
std::string trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string normalizePyName(const std::string& nameIn) {
    // python: c.isalpha() or c.isnumeric()、かつ ASCII のみ。それ以外は "_"
    std::string name = lower(trim(nameIn));
    auto rep = [&](const std::string& a, const std::string& b) {
        size_t p = 0;
        while ((p = name.find(a, p)) != std::string::npos) { name.replace(p, a.size(), b); p += b.size(); }
    };
    rep("#", "_sharp");
    rep("\xE2\x99\xAF", "_sharp");  // ♯
    rep("\xE2\x99\xAD", "_flat");   // ♭
    std::string chars;
    for (unsigned char c : name) chars.push_back((c < 128 && isalnum(c)) ? (char)c : '_');
    // UTF-8 の多バイト文字は文字ごとに "_" 1個(バイトごとに "_" が並ぶので後で連続を潰す)
    if (chars.empty()) return "";
    std::string out;
    for (size_t i = 0; i < chars.size(); i++) {
        if (i + 1 < chars.size() && chars[i] == '_' && chars[i + 1] == '_') continue;
        out.push_back(chars[i]);
    }
    size_t a = 0, b = out.size();
    while (a < b && out[a] == '_') a++;
    while (b > a && out[b - 1] == '_') b--;
    return out.substr(a, b - a);
}

std::string stripSuffixes(std::string s, bool siPrefix = true) {
    s = trim(s);
    std::string sl = lower(s);
    if (siPrefix && sl.size() >= 3 && sl.compare(sl.size() - 3, 3, "khz") == 0) {
        char* e = nullptr;
        std::string head = s.substr(0, s.size() - 3);
        double v = strtod(head.c_str(), &e);
        if (e && *e == 0 && !head.empty()) {
            char buf[64];
            snprintf(buf, sizeof buf, "%.17g", v * 1000);
            return buf;
        }
        return s;
    }
    static const char* sfx[] = {"seconds", "dbtp", "sec", "hz", "ms", "db", "x", "%", "*", ",", "."};
    bool again = true;
    while (again) {
        again = false;
        for (auto f : sfx) {
            size_t n = strlen(f);
            if (sl.size() >= n && sl.compare(sl.size() - n, n, f) == 0) {
                s = trim(s.substr(0, s.size() - n));
                sl = lower(s);
                again = true;
                break;
            }
        }
    }
    return s;
}

bool parseFloat(const std::string& textIn, double& out) {
    std::string t = stripSuffixes(textIn);
    if (t.empty()) return false;
    char* e = nullptr;
    out = strtod(t.c_str(), &e);
    return e && *e == 0;
}

std::string numToText(const vj::Value& v) {
    if (v.type == vj::Value::Int) return std::to_string(v.i);
    if (v.type == vj::Value::Float) return vj::pyFloat(v.d);
    if (v.type == vj::Value::Bool) return v.b ? "True" : "False";
    return v.s;
}

struct ParamRange {
    double lo, hi;
    std::string text;
};

struct ParamEntry {
    ParameterInfo info;
    std::string title, units, py;
    bool analysed = false;
    std::string type = "str";
    double minV = 0, maxV = 0;
    std::vector<ParamRange> ranges;       // 元の文字列
    std::vector<double> rangeFloat;       // type==float のときの数値
    std::vector<std::string> valid;
};

}  // namespace

struct Vst3Plugin::Impl {
    std::string path;
    VST3::Hosting::Module::Ptr module;
    IPtr<IComponent> comp;
    IPtr<IAudioProcessor> proc;
    IPtr<IEditController> ctl;
    IPtr<IConnectionPoint> cpComp, cpCtl;
    bool ctlSeparate = false;
    HostHandler* handler = nullptr;
    std::string name;
    std::vector<ParamEntry> params;
    std::map<ParamID, double> setValues;   // setParam で設定した値(再読込時に再設定、処理側へも送る)
    std::map<ParamID, double> pending;     // 次の process で処理側へ送る
    std::map<ParamID, double> initial;     // エディタ前の値
    int32 inBuses = 0, outBuses = 0;
    Spec lastSpec;
    bool active = false;
    long samplesProvided = 0;
    ParameterChanges inChanges, outChanges;
    bool resetFresh = false;

    ~Impl() { teardown(); }

    void teardown() {
        if (proc && active) {
            proc->setProcessing(false);
            comp->setActive(false);
            active = false;
        }
        if (cpComp && cpCtl) {
            cpComp->disconnect(cpCtl);
            cpCtl->disconnect(cpComp);
        }
        cpComp = nullptr;
        cpCtl = nullptr;
        if (ctl) {
            ctl->setComponentHandler(nullptr);
            if (ctlSeparate) ctl->terminate();
        }
        if (handler) { handler->release(); handler = nullptr; }
        ctl = nullptr;
        proc = nullptr;
        if (comp) comp->terminate();
        comp = nullptr;
    }

    void load() {
        auto& f = module->getFactory();
        std::unique_ptr<VST3::UID> cid;
        for (auto& ci : f.classInfos()) {
            if (ci.category() == kVstAudioEffectClass) {
                cid.reset(new VST3::UID(ci.ID()));
                name = ci.name();
                break;
            }
        }
        if (!cid) throw std::runtime_error("VST3 に Audio Module Class が見つかりません: " + path);
        comp = f.createInstance<IComponent>(*cid);
        if (!comp) throw std::runtime_error("VST3 のコンポーネントを作れません: " + path);
        if (comp->initialize(&hostApp()) != kResultOk) throw std::runtime_error("VST3 の初期化に失敗: " + path);
        proc = FUnknownPtr<IAudioProcessor>(comp);
        if (!proc) throw std::runtime_error("VST3 が IAudioProcessor を持ちません");
        // コントローラ
        ctlSeparate = false;
        FUnknownPtr<IEditController> sc(comp);
        if (sc) {
            ctl = sc;
        } else {
            TUID ctlId;
            if (comp->getControllerClassId(ctlId) == kResultTrue) {
                ctl = f.createInstance<IEditController>(VST3::UID::fromTUID(ctlId));
                if (ctl) {
                    ctlSeparate = true;
                    ctl->initialize(&hostApp());
                }
            }
        }
        if (!ctl) throw std::runtime_error("VST3 のコントローラを作れません");
        handler = new HostHandler();
        ctl->setComponentHandler(handler);
        if (ctlSeparate) {
            cpComp = FUnknownPtr<IConnectionPoint>(comp);
            cpCtl = FUnknownPtr<IConnectionPoint>(ctl);
            if (cpComp && cpCtl) {
                cpComp->connect(cpCtl);
                cpCtl->connect(cpComp);
            }
        }
        // 処理側の状態をコントローラへ
        {
            auto* ms = new MemoryStream();
            if (comp->getState(ms) == kResultTrue) {
                ms->seek(0, IBStream::kIBSeekSet, nullptr);
                ctl->setComponentState(ms);
            }
            ms->release();
        }
        inBuses = comp->getBusCount(kAudio, kInput);
        outBuses = comp->getBusCount(kAudio, kOutput);
        // パラメータ一覧
        params.clear();
        int32 n = ctl->getParameterCount();
        for (int32 i = 0; i < n; i++) {
            ParameterInfo pi;
            if (ctl->getParameterInfo(i, pi) != kResultOk) continue;
            if (pi.flags & ParameterInfo::kIsProgramChange) continue;
            ParamEntry e;
            e.info = pi;
            e.title = VST3::StringConvert::convert(pi.title);
            e.units = VST3::StringConvert::convert(pi.units);
            std::string nm = e.title;
            if (!e.units.empty() && e.units[0] != ':') nm += " " + e.units;
            e.py = normalizePyName(nm);
            params.push_back(std::move(e));
        }
    }

    std::string textFor(const ParamEntry& e, double norm) {
        String128 s;
        if (ctl->getParamStringByValue(e.info.id, norm, s) != kResultOk) return "";
        return VST3::StringConvert::convert(s);
    }

    void analyse(ParamEntry& e) {
        if (e.analysed) return;
        const int steps = 1000;
        std::string text;
        bool have = false;
        double start = 0;
        e.ranges.clear();
        for (int x = 0; x <= steps; x++) {
            double raw = (double)x / steps;
            std::string t = textFor(e, raw);
            if (!have) { text = t; have = true; }
            else if (t != text) {
                e.ranges.push_back({start, raw, text});
                text = t;
                start = raw;
            }
        }
        e.ranges.push_back({start, 1.0, text});
        bool allFloat = true;
        for (auto& r : e.ranges) { double v; if (!parseFloat(r.text, v)) { allFloat = false; break; } }
        e.valid.clear();
        if (allFloat) {
            e.type = "float";
            e.rangeFloat.clear();
            for (auto& r : e.ranges) { double v = 0; parseFloat(r.text, v); e.rangeFloat.push_back(v); }
            e.minV = *std::min_element(e.rangeFloat.begin(), e.rangeFloat.end());
            e.maxV = *std::max_element(e.rangeFloat.begin(), e.rangeFloat.end());
        } else {
            bool boolish = false;
            if (e.ranges.size() == 2) {
                static const std::set<std::string> tr = {"on", "yes", "true", "enabled"};
                for (auto& r : e.ranges) if (tr.count(lower(r.text))) boolish = true;
            }
            if (boolish) {
                e.type = "bool";
                e.minV = 0; e.maxV = 1;
            } else {
                e.type = "str";
            }
            for (auto& r : e.ranges) e.valid.push_back(r.text);
        }
        e.analysed = true;
    }

    double rawForText(ParamEntry& e, const std::string& text) {
        std::u16string u = VST3::StringConvert::convert(text);
        ParamValue v = 0;
        if (ctl->getParamValueByString(e.info.id, (TChar*)u.c_str(), v) != kResultOk) return -1;
        return v;
    }

    double rawFor(ParamEntry& e, const vj::Value& val) {
        analyse(e);
        if (e.type == "float") {
            double nv;
            if (val.isNum()) nv = val.num();
            else if (val.isStr()) {
                std::string t = val.s;
                if (!e.units.empty() && t.size() >= e.units.size() && t.compare(t.size() - e.units.size(), e.units.size(), e.units) == 0)
                    t = t.substr(0, t.size() - e.units.size());
                char* ep = nullptr;
                nv = strtod(t.c_str(), &ep);
                if (!ep || *ep) throw std::runtime_error("パラメータ '" + e.py + "' の値が数値ではありません");
            } else if (val.type == vj::Value::Bool) nv = val.b ? 1 : 0;
            else throw std::runtime_error("パラメータ '" + e.py + "' の値が数値ではありません");
            if (nv < e.minV || nv > e.maxV)
                throw std::runtime_error("パラメータ '" + e.py + "' の値 " + vj::pyFloat(nv) + " が範囲外です (" + vj::pyFloat(e.minV) + " - " + vj::pyFloat(e.maxV) + ")");
            double rep = rawForText(e, vj::pyFloat(nv));  // python: str(float(value))
            // いちばん近い範囲(pedalboard は dict の順で、同距離なら先のもの)
            double bestDiff = 0;
            int best = -1;
            for (size_t i = 0; i < e.rangeFloat.size(); i++) {
                double diff = nv - e.rangeFloat[i];
                if (best < 0 || std::fabs(diff) < std::fabs(bestDiff)) { best = (int)i; bestDiff = diff; }
            }
            if (best >= 0 && (rep < e.ranges[(size_t)best].lo || rep > e.ranges[(size_t)best].hi)) return e.ranges[(size_t)best].lo;
            return rep;
        }
        if (e.type == "str") {
            std::string t = val.isStr() ? val.s : numToText(val);
            int idx = -1;
            for (size_t i = 0; i < e.ranges.size(); i++) if (e.ranges[i].text == t) { idx = (int)i; break; }
            if (idx < 0) {
                std::string list;
                for (auto& r : e.ranges) list += (list.empty() ? "" : ", ") + r.text;
                throw std::runtime_error("パラメータ '" + e.py + "' の値 '" + t + "' は候補にありません: " + list);
            }
            double rep = rawForText(e, t);
            if (rep < e.ranges[(size_t)idx].lo || rep > e.ranges[(size_t)idx].hi) return e.ranges[(size_t)idx].lo;
            return rep;
        }
        // bool
        if (val.type != vj::Value::Bool) throw std::runtime_error("パラメータ '" + e.py + "' は真偽値で指定してください");
        return val.b ? 1.0 : 0.0;
    }

    ParamEntry* findParam(const std::string& py) {
        for (auto& e : params) if (e.py == py) return &e;
        return nullptr;
    }

    void applyValue(ParamID id, double raw) {
        ctl->setParamNormalized(id, raw);
        setValues[id] = raw;
        pending[id] = raw;
    }

    // ---- 処理側
    void deactivate() {
        if (active) {
            proc->setProcessing(false);
            comp->setActive(false);
            active = false;
        }
    }

    void activate(const Spec& spec) {
        deactivate();
        uint32_t nch = spec.numChannels;
        if (nch != 1 && nch != 2) throw std::runtime_error("VST3 は 1ch か 2ch のみ対応です");
        SpeakerArrangement want = nch == 1 ? SpeakerArr::kMono : SpeakerArr::kStereo;
        std::vector<SpeakerArrangement> ins((size_t)inBuses), outs((size_t)outBuses);
        for (int32 i = 0; i < inBuses; i++) proc->getBusArrangement(kInput, i, ins[(size_t)i]);
        for (int32 i = 0; i < outBuses; i++) proc->getBusArrangement(kOutput, i, outs[(size_t)i]);
        if (inBuses > 0) ins[0] = want;
        if (outBuses > 0) outs[0] = want;
        proc->setBusArrangements(ins.empty() ? nullptr : ins.data(), inBuses, outs.empty() ? nullptr : outs.data(), outBuses);
        SpeakerArrangement chk = 0;
        if (inBuses > 0 && (proc->getBusArrangement(kInput, 0, chk) != kResultOk || chk != want))
            throw std::runtime_error("VST3 が " + std::to_string(nch) + "ch の入力に対応していません");
        if (outBuses > 0 && (proc->getBusArrangement(kOutput, 0, chk) != kResultOk || chk != want))
            throw std::runtime_error("VST3 が " + std::to_string(nch) + "ch の出力に対応していません");
        for (int32 i = 0; i < inBuses; i++) comp->activateBus(kAudio, kInput, i, i == 0);
        for (int32 i = 0; i < outBuses; i++) comp->activateBus(kAudio, kOutput, i, i == 0);
        ProcessSetup setup{};
        setup.processMode = kOffline;
        setup.symbolicSampleSize = kSample32;
        setup.maxSamplesPerBlock = (int32)spec.maximumBlockSize;
        setup.sampleRate = spec.sampleRate;
        if (proc->setupProcessing(setup) != kResultOk) throw std::runtime_error("VST3 setupProcessing に失敗");
        if (comp->setActive(true) != kResultOk) throw std::runtime_error("VST3 setActive に失敗");
        proc->setProcessing(true);
        active = true;
        lastSpec = spec;
        // 設定済みの値を処理側へ
        for (auto& kv : setValues) pending[kv.first] = kv.second;
    }
};

Vst3Plugin::Vst3Plugin(const std::string& utf8Path) : d(new Impl) {
    d->path = utf8Path;
    const char* rf = getenv("VAUDIO_VST_RESET");
    d->resetFresh = rf && std::string(rf) == "fresh";
    auto& cache = moduleCache();
    auto it = cache.find(utf8Path);
    if (it == cache.end()) {
        std::string err;
        auto m = VST3::Hosting::Module::create(utf8Path, err);
        if (!m) throw std::runtime_error("VST3 を読み込めません: " + utf8Path + " (" + err + ")");
        it = cache.emplace(utf8Path, m).first;
    }
    d->module = it->second;
    d->inChanges.setMaxParameters(1024);
    d->outChanges.setMaxParameters(1024);
    d->load();
}

Vst3Plugin::~Vst3Plugin() {}

std::string Vst3Plugin::pluginName() const { return d->name; }

void Vst3Plugin::setParam(const std::string& py, const vj::Value& v) {
    ParamEntry* e = d->findParam(py);
    if (!e) {
        std::string list;
        for (auto& p : d->params) list += (list.empty() ? "" : ", ") + p.py;
        throw std::runtime_error("パラメータ '" + py + "' はこのプラグインにありません (" + list + ")");
    }
    double raw = d->rawFor(*e, v);
    // pedalboard は再読込で値を2回設定する。ここは1回で足りる(値の相互依存があるプラグイン向けに2回)
    d->applyValue(e->info.id, raw);
    d->applyValue(e->info.id, raw);
    d->initial[e->info.id] = d->ctl->getParamNormalized(e->info.id);
}

std::vector<Vst3ParamInfo> Vst3Plugin::describeParams() {
    std::vector<Vst3ParamInfo> out;
    for (auto& e : d->params) {
        d->analyse(e);
        Vst3ParamInfo p;
        p.pyName = e.py;
        p.title = e.title;
        p.units = e.units;
        p.type = e.type;
        p.minValue = e.minV;
        p.maxValue = e.maxV;
        p.validValues = e.valid;
        p.currentText = d->textFor(e, d->ctl->getParamNormalized(e.info.id));
        out.push_back(std::move(p));
    }
    return out;
}

vj::Value Vst3Plugin::changedParams() {
    vj::Value out = vj::Value::object();
    for (auto& e : d->params) {
        if (e.info.flags & ParameterInfo::kIsReadOnly) continue;
        double cur = d->ctl->getParamNormalized(e.info.id);
        auto it = d->initial.find(e.info.id);
        double base = it != d->initial.end() ? it->second : e.info.defaultNormalizedValue;
        if (std::fabs(cur - base) < 1e-9) continue;
        d->analyse(e);
        std::string text = d->textFor(e, cur);
        if (e.type == "float") {
            double v;
            if (!parseFloat(text, v)) continue;
            out.set(e.py, vj::Value::number(v));
        } else if (e.type == "bool") {
            out.set(e.py, vj::Value::boolean(cur >= 0.5));
        } else {
            out.set(e.py, vj::Value::str(text));
        }
    }
    return out;
}

void Vst3Plugin::prepare(const Spec& spec) {
    if (d->lastSpec.sampleRate != spec.sampleRate || d->lastSpec.maximumBlockSize < spec.maximumBlockSize ||
        d->lastSpec.numChannels != spec.numChannels || !d->active) {
        d->activate(spec);
    }
}

void Vst3Plugin::reset() {
    if (d->resetFresh) {
        // 作り直し(pedalboard の PersistsAudioOnReset 相当)
        Spec keep = d->lastSpec;
        auto vals = d->setValues;
        d->teardown();
        d->params.clear();
        d->load();
        d->setValues = vals;
        for (int i = 0; i < 2; i++) for (auto& kv : vals) d->ctl->setParamNormalized(kv.first, kv.second);
        d->pending.clear();
        d->active = false;
        d->lastSpec = Spec();
        (void)keep;
    } else {
        d->deactivate();
        d->lastSpec.maximumBlockSize = 0;
    }
    d->samplesProvided = 0;
}

int Vst3Plugin::getLatencyHint() { return (int)d->proc->getLatencySamples(); }

int Vst3Plugin::process(float** ch, int numCh, int n) {
    if (!d->active) throw std::runtime_error("VST3 が有効化されていません");
    d->inChanges.clearQueue();
    d->outChanges.clearQueue();
    for (auto& kv : d->pending) {
        int32 idx = 0;
        IParamValueQueue* q = d->inChanges.addParameterData(kv.first, idx);
        if (q) q->addPoint(0, kv.second, idx);
    }
    d->pending.clear();

    std::vector<AudioBusBuffers> ib((size_t)std::max(1, d->inBuses)), ob((size_t)std::max(1, d->outBuses));
    for (auto& b : ib) { b.numChannels = 0; b.silenceFlags = 0; b.channelBuffers32 = nullptr; }
    for (auto& b : ob) { b.numChannels = 0; b.silenceFlags = 0; b.channelBuffers32 = nullptr; }
    if (d->inBuses > 0) { ib[0].numChannels = numCh; ib[0].channelBuffers32 = ch; }
    if (d->outBuses > 0) { ob[0].numChannels = numCh; ob[0].channelBuffers32 = ch; }
    ProcessData pd{};
    pd.processMode = kOffline;
    pd.symbolicSampleSize = kSample32;
    pd.numSamples = n;
    pd.numInputs = d->inBuses;
    pd.numOutputs = d->outBuses;
    pd.inputs = d->inBuses ? ib.data() : nullptr;
    pd.outputs = d->outBuses ? ob.data() : nullptr;
    pd.inputParameterChanges = &d->inChanges;
    pd.outputParameterChanges = &d->outChanges;
    pd.inputEvents = nullptr;
    pd.outputEvents = nullptr;
    pd.processContext = nullptr;
    if (d->proc->process(pd) != kResultOk) throw std::runtime_error("VST3 の process が失敗しました");
    d->samplesProvided += n;
    long usable = std::max(0L, d->samplesProvided - (long)d->proc->getLatencySamples());
    return (int)std::min(usable, (long)n);
}

// ---- エディタ
namespace {
class EditorImpl : public Vst3Editor {
public:
    IPtr<IPlugView> view;
    PlugFrame* frame = nullptr;
    bool attached = false;
    void size(int& w, int& h) override {
        ViewRect r{};
        if (view->getSize(&r) == kResultOk) { w = r.right - r.left; h = r.bottom - r.top; }
    }
    void onSize(int w, int h) override {
        ViewRect r{0, 0, w, h};
        view->onSize(&r);
    }
    void close() override {
        if (attached) { view->removed(); attached = false; }
        view->setFrame(nullptr);
    }
    ~EditorImpl() override {
        close();
        if (frame) frame->release();
    }
};
}  // namespace

bool Vst3Plugin::hasEditor() {
    IPlugView* v = d->ctl->createView(ViewType::kEditor);
    if (!v) return false;
    v->release();
    return true;
}

std::unique_ptr<Vst3Editor> Vst3Plugin::openEditor(void* parentHwnd, std::function<void(int, int)> onResize) {
    IPlugView* v = d->ctl->createView(ViewType::kEditor);
    if (!v) throw std::runtime_error("このプラグインはエディタを持ちません");
    auto e = std::make_unique<EditorImpl>();
    e->view = owned(v);
    if (e->view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) throw std::runtime_error("HWND 非対応のエディタです");
    e->frame = new PlugFrame();
    e->frame->onResize = std::move(onResize);
    e->view->setFrame(e->frame);
    if (e->view->attached(parentHwnd, kPlatformTypeHWND) != kResultOk) throw std::runtime_error("エディタを埋め込めません");
    e->attached = true;
    return e;
}

}  // namespace vaudio
