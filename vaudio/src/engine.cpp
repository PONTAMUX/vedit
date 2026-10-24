// vedit/audio.py の process() の移植。段の種類・キャッシュ鍵・区間分割・クロスフェード・正規化まで同じ順序で行う。
#include "engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "declick.h"
#include "media.h"
#include "vst3host.h"

namespace vaudio {

using vj::Value;

// ------------------------------------------------------------------ 文字列・ファイル
std::wstring widenPath(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], n);
    return w;
}
std::string narrowPath(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool readFileText(const std::string& p, std::string& out) {
    FILE* f = _wfopen(widenPath(p).c_str(), L"rb");
    if (!f) return false;
    out.clear();
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}
bool writeFileText(const std::string& p, const std::string& text) {
    FILE* f = _wfopen(widenPath(p).c_str(), L"wb");
    if (!f) return false;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}
static bool fileExists(const std::string& p) {
    DWORD a = GetFileAttributesW(widenPath(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static void removeFile(const std::string& p) { DeleteFileW(widenPath(p).c_str()); }
static bool moveReplace(const std::string& a, const std::string& b) {
    return MoveFileExW(widenPath(a).c_str(), widenPath(b).c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
static int64_t fileSize(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widenPath(p).c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((int64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}
// Python の os.stat().st_mtime と同じ double
static double fileMtime(const std::string& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(widenPath(p).c_str(), GetFileExInfoStandard, &d)) throw std::runtime_error("ファイルがありません: " + p);
    unsigned long long in = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    long long sec = (long long)(in / 10000000ULL) - 11644473600LL;
    int nsec = (int)(in % 10000000ULL) * 100;
    return (double)sec + (double)nsec * 1e-9;
}
static void makeDirs(const std::string& p) {
    std::wstring w = widenPath(p);
    for (size_t i = 1; i < w.size(); i++)
        if (w[i] == L'\\' || w[i] == L'/') CreateDirectoryW(w.substr(0, i).c_str(), nullptr);
    CreateDirectoryW(w.c_str(), nullptr);
}

static std::string winNorm(std::string s) {
    for (auto& c : s) if (c == '/') c = '\\';
    std::string out;
    bool unc = s.size() >= 2 && s[0] == '\\' && s[1] == '\\';
    size_t i = unc ? 2 : 0;
    if (unc) out = "\\\\";
    std::vector<std::string> parts;
    std::string cur;
    for (; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\\') {
            if (!cur.empty() && cur != ".") parts.push_back(cur);
            cur.clear();
        } else cur.push_back(s[i]);
    }
    for (size_t k = 0; k < parts.size(); k++) { if (k) out += "\\"; out += parts[k]; }
    if (parts.size() == 1 && parts[0].size() == 2 && parts[0][1] == ':') out += "\\";
    return out;
}
static bool isAbs(const std::string& s) {
    if (s.size() >= 3 && s[1] == ':' && (s[2] == '\\' || s[2] == '/')) return true;
    if (s.size() >= 2 && (s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/')) return true;
    return false;
}
static std::string fullPath(const std::string& p) {
    std::wstring w = widenPath(p);
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(w.c_str(), (DWORD)(sizeof buf / sizeof buf[0]), buf, nullptr);
    if (!n) return p;
    std::wstring full(buf, n);
    wchar_t lbuf[MAX_PATH * 4];
    DWORD m = GetLongPathNameW(full.c_str(), lbuf, (DWORD)(sizeof lbuf / sizeof lbuf[0]));
    if (m && m < sizeof lbuf / sizeof lbuf[0]) full.assign(lbuf, m);
    return narrowPath(full);
}
static std::string dirOf(const std::string& p) {
    size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? std::string(".") : p.substr(0, k);
}
static std::string stemOf(const std::string& p) {
    size_t k = p.find_last_of("\\/");
    std::string name = k == std::string::npos ? p : p.substr(k + 1);
    size_t d = name.find_last_of('.');
    return (d == std::string::npos || d == 0) ? name : name.substr(0, d);
}

std::string resolveInProject(const std::string& projectPath, const std::string& p) {
    std::string dir = dirOf(fullPath(projectPath));
    if (isAbs(p)) return winNorm(p);
    return winNorm(dir + "\\" + p);
}
std::string defaultCacheDir(const std::string& projectPath) {
    std::string full = fullPath(projectPath);
    return dirOf(full) + "\\" + stemOf(full) + ".cache";
}

// ------------------------------------------------------------------ 小物
static Audio slice(const Audio& a, size_t lo, size_t hi) {
    Audio o(a.size());
    for (size_t c = 0; c < a.size(); c++) o[c].assign(a[c].begin() + (long)lo, a[c].begin() + (long)hi);
    return o;
}
static double pyRound(double x, int nd) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.*f", nd, x);
    return atof(buf);
}
static Value fnum(double d) { return Value::number(d); }

static std::string typeOf(const Value& st) { return st.getStr("type"); }

std::unique_ptr<Plugin> buildStepPlugin(const std::string& projectDir, const Value& st) {
    std::string t = typeOf(st);
    if (t == "gain") return makeGain((float)st.getNum("db", 0));
    std::unique_ptr<Plugin> pl;
    const Value* params = st.find("params");
    if (t == "vst3") {
        std::string path = st.getStr("path");
        std::string abs = isAbs(path) ? winNorm(path) : winNorm(projectDir + "\\" + path);
        auto v = std::make_unique<Vst3Plugin>(abs);
        if (params && params->isObj())
            for (auto& kv : *params->o) v->setParam(kv.first, kv.second);
        return v;
    }
    if (t == "plugin") {
        Value empty = Value::object();
        return makeBuiltin(st.getStr("name"), params && params->isObj() ? *params : empty);
    }
    throw std::runtime_error("不明な audio ステップ: " + t);
}

// ------------------------------------------------------------------ 子プロセス
namespace {

struct Child {
    HANDLE proc = nullptr, thread = nullptr, hin = nullptr, hout = nullptr;
    std::string buf;
    bool start(const std::wstring& cmd, HANDLE job) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE inR, inW, outR, outW;
        if (!CreatePipe(&inR, &inW, &sa, 0)) return false;
        if (!CreatePipe(&outR, &outW, &sa, 0)) return false;
        SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, &sa, OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = inR;
        si.hStdOutput = outW;
        si.hStdError = nul;
        PROCESS_INFORMATION pi{};
        std::wstring c = cmd;
        BOOL ok = CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(inR);
        CloseHandle(outW);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) { CloseHandle(inW); CloseHandle(outR); return false; }
        if (job) AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        proc = pi.hProcess;
        thread = pi.hThread;
        hin = inW;
        hout = outR;
        return true;
    }
    bool writeLine(const std::string& s) {
        std::string t = s + "\n";
        DWORD n;
        return WriteFile(hin, t.data(), (DWORD)t.size(), &n, nullptr) && n == t.size();
    }
    bool readLine(std::string& line) {
        while (true) {
            size_t k = buf.find('\n');
            if (k != std::string::npos) {
                line = buf.substr(0, k);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, k + 1);
                return true;
            }
            char tmp[4096];
            DWORD n = 0;
            if (!ReadFile(hout, tmp, sizeof tmp, &n, nullptr) || n == 0) return false;
            buf.append(tmp, n);
        }
    }
    void closeAll() {
        if (hin) { CloseHandle(hin); hin = nullptr; }
        if (hout) { CloseHandle(hout); hout = nullptr; }
        if (proc) {
            if (WaitForSingleObject(proc, 3000) == WAIT_TIMEOUT) TerminateProcess(proc, 1);  // 自分で起動した子だけ
            CloseHandle(proc);
            proc = nullptr;
        }
        if (thread) { CloseHandle(thread); thread = nullptr; }
    }
    void kill() { if (proc) TerminateProcess(proc, 1); }
};

std::string selfExe() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return narrowPath(std::wstring(buf, n));
}

// 生の planar float を読み書き(チャンネル順に frames 個ずつ)
void writeRaw(const std::string& path, const Audio& a) {
    FILE* f = _wfopen(widenPath(path).c_str(), L"wb");
    if (!f) throw std::runtime_error("書き込めません: " + path);
    for (auto& ch : a) fwrite(ch.data(), sizeof(float), ch.size(), f);
    fclose(f);
}
Audio readRawSpan(const std::string& path, int nch, size_t frames, size_t lo, size_t hi) {
    FILE* f = _wfopen(widenPath(path).c_str(), L"rb");
    if (!f) throw std::runtime_error("読めません: " + path);
    Audio a((size_t)nch);
    for (int c = 0; c < nch; c++) {
        a[(size_t)c].resize(hi - lo);
        _fseeki64(f, (long long)(((size_t)c * frames + lo) * sizeof(float)), SEEK_SET);
        size_t got = fread(a[(size_t)c].data(), sizeof(float), hi - lo, f);
        if (got != hi - lo) { fclose(f); throw std::runtime_error("生データが短いです: " + path); }
    }
    fclose(f);
    return a;
}

}  // namespace

// ------------------------------------------------------------------ ワーカー
int workerMain(const std::string& jobFile) {
    try {
        std::string text;
        if (!readFileText(jobFile, text)) return 2;
        Value job = vj::parse(text);
        std::string projDir = job.getStr("projectDir");
        double pre = job.getNum("pre", 0);
        std::string raw = job.getStr("raw");
        int nch = (int)job.getNum("ch", 2), sr = (int)job.getNum("sr", 48000);
        size_t frames = (size_t)job.getNum("frames", 0);
        std::vector<std::unique_ptr<Plugin>> owned;
        if (pre != 0.0) owned.push_back(makeGain((float)pre));
        const Value* specs = job.find("specs");
        if (specs && specs->isArr())
            for (size_t i = 0; i < specs->size(); i++) owned.push_back(buildStepPlugin(projDir, specs->at(i)));
        std::vector<Plugin*> board;
        for (auto& p : owned) board.push_back(p.get());
        setvbuf(stdout, nullptr, _IONBF, 0);
        printf("ready\n");
        char line[4096];
        while (fgets(line, sizeof line, stdin)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (l == "quit") break;
            // "i lo hi outpath"
            size_t i, lo, hi;
            int off = 0;
            if (sscanf(l.c_str(), "%zu %zu %zu %n", &i, &lo, &hi, &off) < 3) continue;
            std::string out = l.substr((size_t)off);
            try {
                Audio in = readRawSpan(raw, nch, frames, lo, hi);
                Audio res = runBoard(in, sr, board);
                writeRaw(out, res);
                printf("ok %zu\n", i);
            } catch (const std::exception& e) {
                std::string m = e.what();
                for (auto& c : m) if (c == '\n' || c == '\r') c = ' ';
                printf("err %zu %s\n", i, m.c_str());
            }
        }
        return 0;
    } catch (const std::exception& e) {
        printf("fatal %s\n", e.what());
        return 3;
    }
}

// ------------------------------------------------------------------ メイン処理

Value processProject(const std::string& projectPath, const ProcessOptions& opt) {
    auto progress = [&](double f, const std::string& m) { if (opt.progress) opt.progress(f, m); };
    auto checkCancel = [&]() { if (opt.cancel && opt.cancel->load()) throw std::runtime_error("中止しました"); };

    std::string text;
    if (!readFileText(projectPath, text)) throw std::runtime_error("プロジェクトを開けません: " + projectPath);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    Value data = vj::parse(text);
    std::string projDir = dirOf(fullPath(projectPath));

    // audio のひな形(project.py の DEFAULT_AUDIO)
    Value audio = data.get("audio");
    if (!audio.isObj()) {
        audio = vj::parse("{\"chain\":[{\"type\":\"declick\",\"params\":{\"threshold\":8}},{\"type\":\"gain\",\"db\":18},{\"type\":\"normalize\",\"peak_db\":-3}]}");
    }
    Value chainV = audio.get("chain");
    if (!chainV.isArr()) throw std::runtime_error("audio.chain がありません");
    std::string srcRel = data.getStr("source");
    std::string src = resolveInProject(projectPath, srcRel);
    if (!fileExists(src)) throw std::runtime_error("元素材がありません: " + src);
    std::string cache = opt.outDir.empty() ? defaultCacheDir(projectPath) : winNorm(fullPath(opt.outDir));
    makeDirs(cache);
    std::string outWav = cache + "\\audio.wav", meta = cache + "\\audio.json";

    double mtime = fileMtime(src);
    Value keyArr = Value::array();
    keyArr.push(Value::str(src));
    keyArr.push(fnum(mtime));
    keyArr.push(chainV);
    std::string key = vj::dump(keyArr, vj::DumpOpt{true, true, -1});

    if (!opt.force && fileExists(outWav) && fileExists(meta)) {
        std::string mt;
        if (readFileText(meta, mt)) {
            try {
                Value old = vj::parse(mt);
                if (old.isObj() && old.getStr("key") == key) { progress(1.0, "キャッシュを使用"); return old; }
            } catch (...) {}
        }
    }

    // ---- 段の分類
    std::vector<Value> chain;
    for (size_t i = 0; i < chainV.size(); i++) chain.push_back(chainV.at(i));
    const Value* norm = nullptr;
    std::vector<Value> clickers, levels, plugins;
    for (auto& s : chain) {
        std::string t = typeOf(s);
        if (t == "normalize") { if (!norm) norm = &s; }
        else if (t == "declick") clickers.push_back(s);
        else if (t == "level") levels.push_back(s);
        else {
            if (t != "gain" && t != "vst3" && t != "plugin") throw std::runtime_error("不明な audio ステップ: " + t);
            plugins.push_back(s);
        }
    }
    int last = -1;
    for (size_t i = 0; i < plugins.size(); i++) if (typeOf(plugins[i]) == "vst3") last = (int)i;
    std::vector<Value> heavy(plugins.begin(), plugins.begin() + (last + 1)), light(plugins.begin() + (last + 1), plugins.end());
    std::string hfile = cache + "\\audio_heavy.f32", hmeta = cache + "\\audio_heavy.json";

    Value hkeyArr = Value::array();
    hkeyArr.push(Value::str(src));
    hkeyArr.push(fnum(mtime));
    {
        Value dl = Value::array();
        for (auto& s : chain) if (typeOf(s) == "declick" || typeOf(s) == "level") dl.push(s);
        hkeyArr.push(dl);
        Value hv = Value::array();
        for (auto& s : heavy) hv.push(s);
        hkeyArr.push(hv);
    }
    std::string hkey = vj::dump(hkeyArr, vj::DumpOpt{true, true, -1});

    bool hasNormLufs = norm && norm->find("lufs") && !norm->find("lufs")->isNull();
    auto preOf = [&](bool haveLr, double lr) -> double {
        if (haveLr && hasNormLufs && !plugins.empty()) return std::max(-30.0, std::min(40.0, norm->getNum("lufs", 0) - lr));
        return 0.0;
    };

    // ---- 重い段のキャッシュ
    bool hit = false;
    bool haveLufsRaw = false;
    double lufsRaw = 0, pre = 0;
    int clicks = 0, sr = 48000, nch = 0;
    size_t frames = 0;
    if (!heavy.empty() && !opt.force && fileExists(hfile) && fileExists(hmeta)) {
        try {
            std::string mt;
            readFileText(hmeta, mt);
            Value h = vj::parse(mt);
            const Value* lr = h.find("lufs_raw");
            bool hl = lr && lr->isNum();
            double lrv = hl ? lr->num() : 0;
            if (h.getStr("key") == hkey && h.getNum("pre", 1e300) == preOf(hl, lrv) && fileSize(hfile) == (int64_t)h.getNum("bytes", -1)) {
                hit = true;
                clicks = (int)h.getNum("clicks", 0);
                haveLufsRaw = hl;
                lufsRaw = lrv;
                pre = h.getNum("pre", 0);
                sr = (int)h.getNum("sr", 48000);
                nch = (int)h.getNum("ch", 2);
                frames = (size_t)h.getNum("frames", 0);
            }
        } catch (...) { hit = false; }
    }

    Audio rawAudio;
    if (!hit) {
        removeFile(hmeta);
        removeFile(hfile);
        progress(0.01, "元音声を読み込み中");
        Decoded dec = decodeAudio(src, 48000);
        rawAudio = std::move(dec.ch);
        sr = dec.sampleRate;
        nch = (int)rawAudio.size();
        frames = nch ? rawAudio[0].size() : 0;
        checkCancel();
        if (!clickers.empty()) {
            progress(0.03, "De-click");
            size_t step = (size_t)sr * 20, pad = 2048;
            Audio clean((size_t)nch);
            for (size_t a = 0; a < frames; a += step) {
                size_t lo = a > pad ? a - pad : 0, hi = std::min(frames, a + step + pad);
                Audio seg = slice(rawAudio, lo, hi);
                for (auto& st : clickers) {
                    const Value* p = st.find("params");
                    seg = declick(seg, sr, declickParamsFrom(p ? *p : Value()), clicks);
                }
                size_t n = std::min(step, frames - a);
                for (int c = 0; c < nch; c++) clean[(size_t)c].insert(clean[(size_t)c].end(), seg[(size_t)c].begin() + (long)(a - lo), seg[(size_t)c].begin() + (long)(a - lo + n));
                checkCancel();
            }
            rawAudio = std::move(clean);
        }
        for (auto& st : levels) {
            progress(0.05, "声の大きさをならし中");
            // q = {"f":500,"g":15,"p":0.7,"m":6,"t":0.003, **params}
            std::vector<std::pair<std::string, Value>> q = {
                {"f", Value::integer(500)}, {"g", Value::integer(15)}, {"p", Value::number(0.7)}, {"m", Value::integer(6)}, {"t", Value::number(0.003)}};
            const Value* p = st.find("params");
            if (p && p->isObj())
                for (auto& kv : *p->o) {
                    bool found = false;
                    for (auto& e : q) if (e.first == kv.first) { e.second = kv.second; found = true; }
                    if (!found) q.push_back(kv);
                }
            std::string args;
            for (auto& e : q) {
                if (!args.empty()) args += ":";
                std::string v = e.second.type == Value::Int ? std::to_string(e.second.i)
                              : e.second.type == Value::Float ? vj::pyFloat(e.second.d)
                              : e.second.type == Value::Bool ? (e.second.b ? "True" : "False") : e.second.s;
                args += e.first + "=" + v;
            }
            rawAudio = dynaudnorm(rawAudio, sr, args);
            checkCancel();
        }
        if (hasNormLufs && !plugins.empty()) {
            progress(0.07, "ラウドネス測定");
            lufsRaw = loudness(rawAudio, sr);
            haveLufsRaw = true;
        }
        pre = preOf(haveLufsRaw, lufsRaw);
        frames = nch ? rawAudio[0].size() : 0;
    }

    // ---- 区間処理
    size_t step = (size_t)sr * 30, padS = (size_t)sr * 2, xf = (size_t)sr / 20;
    std::vector<float> ramp(xf);
    {
        double st = xf > 1 ? 1.0 / (double)(xf - 1) : 0.0;
        for (size_t k = 0; k < xf; k++) ramp[k] = (float)((double)k * st);
        if (xf > 1) ramp[xf - 1] = 1.0f;
    }
    std::vector<size_t> starts;
    for (size_t a = 0; a < frames; a += step) starts.push_back(a);
    struct Span { size_t lo, hi; };
    std::vector<Span> spans;
    for (size_t a : starts) spans.push_back({a > padS ? a - padS : 0, std::min(frames, a + step + xf + padS)});

    std::vector<Plugin*> dummy;
    std::vector<std::unique_ptr<Plugin>> ownedFirst, ownedLight;
    std::vector<Plugin*> boardFirst, boardLight;

    const bool useWorkers = !hit && std::any_of(chain.begin(), chain.end(), [](const Value& s) { return typeOf(s) == "vst3"; });
    int nw = 1;
    if (useWorkers) {
        int n = (int)std::thread::hardware_concurrency();
        if (n <= 0) n = 2;
        double pct = 50;
        const Value* o = data.find("output");
        if (o && o->isObj()) { const Value* cp = o->find("cpu_percent"); if (cp && cp->isNum() && cp->num() != 0) pct = cp->num(); }
        int cores = std::max(2, std::min(n, (int)std::nearbyint(n * pct / 100.0)));
        nw = std::max(1, std::min({(int)starts.size(), 8, cores - 1}));
        if (opt.workersOverride > 0) nw = std::min((int)starts.size(), opt.workersOverride);
    }
    const std::vector<Value>& first = !heavy.empty() ? heavy : plugins;

    // 結果の供給元
    std::mutex mu;
    std::condition_variable cv;
    std::vector<int> state(spans.size(), 0);  // 0 待ち 1 完了 2 失敗
    std::vector<std::string> errs(spans.size());
    std::vector<std::thread> threads;
    std::vector<Child> kids((size_t)nw);
    HANDLE job = nullptr;
    std::atomic<size_t> nextSpan{0};
    std::atomic<bool> abortAll{false};
    std::string rawPath = cache + "\\audio_raw.f32", jobPath = cache + "\\audio_job.json";
    auto spanPath = [&](size_t i) { return cache + "\\audio_span_" + std::to_string(i) + ".f32"; };

    std::vector<size_t> offs{0};
    for (auto& s : spans) offs.push_back(offs.back() + (size_t)nch * (s.hi - s.lo));

    std::unique_ptr<FILE, int (*)(FILE*)> hout(nullptr, fclose);
    auto cleanupWorkers = [&]() {
        abortAll = true;
        for (auto& k : kids) if (k.proc) k.kill();
        for (auto& t : threads) if (t.joinable()) t.join();
        for (auto& k : kids) k.closeAll();
        threads.clear();
        if (job) { CloseHandle(job); job = nullptr; }
        removeFile(rawPath);
        removeFile(jobPath);
        for (size_t i = 0; i < spans.size(); i++) removeFile(spanPath(i));
    };

    try {
        if (!hit) {
            if (useWorkers && nw > 1) {
                writeRaw(rawPath, rawAudio);
                Value jv = Value::object();
                jv.set("projectDir", Value::str(projDir));
                jv.set("pre", fnum(pre));
                jv.set("raw", Value::str(rawPath));
                jv.set("ch", Value::integer(nch));
                jv.set("sr", Value::integer(sr));
                jv.set("frames", Value::integer((int64_t)frames));
                Value sp = Value::array();
                for (auto& s : first) sp.push(s);
                jv.set("specs", sp);
                writeFileText(jobPath, vj::dump(jv, vj::DumpOpt{false, true, -1}));
                job = CreateJobObjectW(nullptr, nullptr);
                if (job) {
                    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
                    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
                }
                std::string exe = opt.exePath.empty() ? selfExe() : opt.exePath;
                for (int w = 0; w < nw; w++) {
                    std::wstring cmd = L"\"" + widenPath(exe) + L"\" --worker \"" + widenPath(jobPath) + L"\"";
                    if (!kids[(size_t)w].start(cmd, job)) throw std::runtime_error("子プロセスを起動できません");
                }
                for (int w = 0; w < nw; w++) {
                    threads.emplace_back([&, w]() {
                        Child& k = kids[(size_t)w];
                        std::string line;
                        if (!k.readLine(line) || line != "ready") {
                            std::lock_guard<std::mutex> lk(mu);
                            // 起動に失敗: 取れる区間は他のワーカーに任せ、全員失敗なら止まる
                            errs[0] = "ワーカーが起動しませんでした: " + line;
                            abortAll = true;
                            for (auto& s : state) if (s == 0) s = 2;
                            cv.notify_all();
                            return;
                        }
                        while (!abortAll) {
                            size_t i = nextSpan.fetch_add(1);
                            if (i >= spans.size()) break;
                            k.writeLine(std::to_string(i) + " " + std::to_string(spans[i].lo) + " " + std::to_string(spans[i].hi) + " " + spanPath(i));
                            std::string r;
                            bool got = k.readLine(r);
                            std::lock_guard<std::mutex> lk(mu);
                            if (got && r.compare(0, 2, "ok") == 0) state[i] = 1;
                            else { state[i] = 2; errs[i] = got ? r : "ワーカーが異常終了しました"; abortAll = true; }
                            cv.notify_all();
                            if (state[i] == 2) break;
                        }
                        k.writeLine("quit");
                    });
                }
            } else {
                // 同じプロセスで順に
                if (pre != 0.0) ownedFirst.push_back(makeGain((float)pre));
                for (auto& s : first) ownedFirst.push_back(buildStepPlugin(projDir, s));
                for (auto& p : ownedFirst) boardFirst.push_back(p.get());
            }
            if (!heavy.empty()) {
                hout.reset(_wfopen(widenPath(cache + "\\audio_heavy.f32.tmp").c_str(), L"wb"));
                if (!hout) throw std::runtime_error("書き込めません: audio_heavy.f32.tmp");
            }
        }
        if (!heavy.empty() && !light.empty()) {
            for (auto& s : light) ownedLight.push_back(buildStepPlugin(projDir, s));
            for (auto& p : ownedLight) boardLight.push_back(p.get());
        }

        // 結果を順に取り出す関数
        auto getResult = [&](size_t i) -> Audio {
            checkCancel();
            if (hit) {
                FILE* f = _wfopen(widenPath(hfile).c_str(), L"rb");
                if (!f) throw std::runtime_error("読めません: " + hfile);
                size_t n = spans[i].hi - spans[i].lo;
                Audio a((size_t)nch);
                _fseeki64(f, (long long)(offs[i] * 4), SEEK_SET);
                for (int c = 0; c < nch; c++) {
                    a[(size_t)c].resize(n);
                    if (fread(a[(size_t)c].data(), 4, n, f) != n) { fclose(f); throw std::runtime_error("audio_heavy.f32 が短いです"); }
                }
                fclose(f);
                return a;
            }
            if (!threads.empty()) {
                std::unique_lock<std::mutex> lk(mu);
                while (state[i] == 0) {
                    cv.wait_for(lk, std::chrono::milliseconds(200));
                    if (opt.cancel && opt.cancel->load()) { abortAll = true; throw std::runtime_error("中止しました"); }
                }
                if (state[i] != 1) throw std::runtime_error(errs[i].empty() ? errs[0] : errs[i]);
                lk.unlock();
                size_t n = spans[i].hi - spans[i].lo;
                Audio a = readRawSpan(spanPath(i), nch, n, 0, n);
                removeFile(spanPath(i));
                return a;
            }
            Audio in = slice(rawAudio, spans[i].lo, spans[i].hi);
            return runBoard(in, sr, boardFirst);
        };

        Audio stage((size_t)nch);
        for (auto& c : stage) c.reserve(frames);
        std::vector<std::vector<float>> tail((size_t)nch);
        bool haveTail = false;
        float peak = 0.0f;
        for (size_t si = 0; si < starts.size(); si++) {
            size_t a = starts[si], lo = spans[si].lo;
            progress(0.1 + 0.8 * (double)si / (double)std::max<size_t>(1, starts.size()), "処理中 " + std::to_string(si + 1) + "/" + std::to_string(starts.size()));
            Audio res = getResult(si);
            if (hout) for (auto& ch : res) fwrite(ch.data(), 4, ch.size(), hout.get());
            if (!boardLight.empty()) res = runBoard(res, sr, boardLight);
            size_t n0 = a - lo, len = std::min(step + xf, frames - a);
            for (int c = 0; c < nch; c++) {
                std::vector<float> r(res[(size_t)c].begin() + (long)n0, res[(size_t)c].begin() + (long)(n0 + len));
                if (haveTail) {
                    size_t n = std::min({xf, r.size(), tail[(size_t)c].size()});
                    for (size_t k = 0; k < n; k++) {
                        float t = tail[(size_t)c][k] * (1.0f - ramp[k]);
                        float u = r[k] * ramp[k];
                        r[k] = t + u;
                    }
                }
                size_t cut = std::min(step, r.size());
                tail[(size_t)c].assign(r.begin() + (long)cut, r.end());
                for (size_t k = 0; k < cut; k++) peak = std::max(peak, std::fabs(r[k]));
                stage[(size_t)c].insert(stage[(size_t)c].end(), r.begin(), r.begin() + (long)cut);
            }
            haveTail = true;
        }
        if (!threads.empty()) {
            for (auto& t : threads) if (t.joinable()) t.join();
            threads.clear();
            for (auto& k : kids) k.closeAll();
            if (job) { CloseHandle(job); job = nullptr; }
            removeFile(rawPath);
            removeFile(jobPath);
        }
        if (hout) {
            hout.reset();
            moveReplace(cache + "\\audio_heavy.f32.tmp", hfile);
            Value hm = Value::object();
            hm.set("key", Value::str(hkey));
            hm.set("lufs_raw", haveLufsRaw ? fnum(lufsRaw) : Value::null());
            hm.set("pre", fnum(pre));
            hm.set("clicks", Value::integer(clicks));
            hm.set("sr", Value::integer(sr));
            hm.set("ch", Value::integer(nch));
            hm.set("frames", Value::integer((int64_t)frames));
            hm.set("bytes", Value::integer(fileSize(hfile)));
            writeFileText(hmeta, vj::dump(hm, vj::DumpOpt{false, true, -1}));
        }

        // ---- 正規化
        progress(0.92, "正規化");
        double peakDb = peak > 0 ? 20 * std::log10((double)peak) : -120.0;
        double gain = 0.0, lufsBefore = 0;
        bool haveLb = false;
        if (hasNormLufs) {
            lufsBefore = loudness(stage, sr);
            haveLb = true;
            gain = std::min(norm->getNum("lufs", 0) - lufsBefore, norm->getNum("peak_db", -1) - peakDb);
        } else if (norm) {
            if (!norm->find("peak_db")) throw std::runtime_error("normalize に peak_db がありません");
            gain = norm->getNum("peak_db", 0) - peakDb;
        }
        progress(0.96, "書き出し");
        writeWav24(outWav, stage, sr, gain);

        Value info = Value::object();
        info.set("key", Value::str(key));
        info.set("peak_before_norm_db", fnum(pyRound(peakDb, 2)));
        info.set("norm_gain_db", fnum(pyRound(gain + pre, 2)));
        info.set("pre_gain_db", fnum(pyRound(pre, 2)));
        // lufs_before: lufs_raw があればその丸め、無ければ lufs_before and round(lufs_before,1)
        if (haveLufsRaw) info.set("lufs_before", fnum(pyRound(lufsRaw, 1)));
        else if (!haveLb) info.set("lufs_before", Value::null());
        else if (lufsBefore == 0.0) info.set("lufs_before", fnum(lufsBefore));
        else info.set("lufs_before", fnum(pyRound(lufsBefore, 1)));
        if (!haveLb) info.set("lufs_after", Value::null());
        else if (lufsBefore == 0.0) info.set("lufs_after", fnum(lufsBefore));
        else info.set("lufs_after", fnum(pyRound(lufsBefore + gain, 1)));
        info.set("clicks_repaired", Value::integer(clicks));
        info.set("file", Value::str(outWav));
        writeFileText(meta, vj::dump(info, vj::DumpOpt{false, false, -1}));
        progress(1.0, "完了");
        return info;
    } catch (...) {
        hout.reset();
        cleanupWorkers();
        removeFile(cache + "\\audio_heavy.f32.tmp");
        throw;
    }
}

}  // namespace vaudio
