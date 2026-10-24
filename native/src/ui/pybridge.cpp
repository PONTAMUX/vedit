#include "pybridge.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace vui::bridge {

QString Cmd::line() const {
    QStringList l{prog};
    for (const QString& a : args) l << (a.contains(' ') ? "\"" + a + "\"" : a);
    return l.join(' ');
}

QString pythonExe() { return qEnvironmentVariableIsSet("VEDIT_PYTHON") ? qEnvironmentVariable("VEDIT_PYTHON") : QStringLiteral("python"); }
QString veditRoot() { return qEnvironmentVariableIsSet("VEDIT_ROOT") ? qEnvironmentVariable("VEDIT_ROOT") : QStringLiteral("C:/vedit"); }
QString vaudioExe() { return qEnvironmentVariableIsSet("VEDIT_VAUDIO") ? qEnvironmentVariable("VEDIT_VAUDIO") : veditRoot() + "/vaudio/dist/vaudio.exe"; }
QString vimgExe() { return qEnvironmentVariableIsSet("VEDIT_VIMG") ? qEnvironmentVariable("VEDIT_VIMG") : veditRoot() + "/vimg-native/dist/vimg.exe"; }

QString photoshopExe() {
    QString best;
    QDateTime bt;
    for (const char* env : {"ProgramFiles", "ProgramFiles(x86)"}) {
        const QString base = qEnvironmentVariable(env);
        if (base.isEmpty()) continue;
        const QDir ad(base + "/Adobe");
        for (const QFileInfo& d : ad.entryInfoList({"Adobe Photoshop*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QFileInfo exe(d.absoluteFilePath() + "/Photoshop.exe");
            if (exe.exists() && (best.isEmpty() || exe.lastModified() > bt)) { best = exe.absoluteFilePath(); bt = exe.lastModified(); }
        }
    }
    return best;
}

// python 側。server.py のジョブ(job_render / job_frame / job_silence)と修正点・画像編集の API を同じ関数で呼ぶ
QString jobScript() {
    return QStringLiteral(R"PY(
import sys, json, os
from pathlib import Path
sys.path.insert(0, sys.argv[4])
def out(tag, obj):
    sys.stdout.write(tag + json.dumps(obj, ensure_ascii=False, default=str) + "\n"); sys.stdout.flush()
try:
    kind, projp, a = sys.argv[1], sys.argv[2], json.loads(sys.argv[3])
    from vedit import audio, ff, render as rd, review, vimg
    from vedit.project import Project
    from vedit.timeutil import parse_time
    p = Project(projp)
    def rel(path):
        try: return Path(path).resolve().relative_to(p.dir.resolve()).as_posix()
        except ValueError: return str(path)
    if kind == "render":
        errs = p.validate()
        if errs: raise RuntimeError("; ".join(errs))
        rng = (parse_time(a["range"][0]), parse_time(a["range"][1])) if a.get("range") else None
        r = rd.render(p, draft=bool(a.get("draft")), rng=rng, on_progress=lambda x: out("P", x))
    elif kind == "frame":
        r = rd.render(p, frame=float(a["t"]))
    elif kind == "silence":
        audio.process(p)
        found = audio.detect_silence(p, float(a.get("min", 0.7)), float(a.get("noise", -40)))
        kb, ka = float(a.get("keep_before", 0.2)), float(a.get("keep_after", 0.35))
        cuts = [[round(s + kb, 3), round(e - ka, 3)] for s, e in found]
        r = {"cuts": [c for c in cuts if c[1] - c[0] > 0.05]}
    elif kind == "review":
        op, rr = a.get("op", "state"), a.get("r")
        if op == "start": review.start(p, video=a.get("video"), force=bool(a.get("force"))); rr = None
        elif op == "next": review.next_round(p, video=a.get("video")); rr = None
        elif op == "add": review.add(p, a["text"], r=rr)
        elif op == "update": review.update(p, a["i"], r=rr, done=a.get("done"), text=a.get("text"), note=a.get("note"), t=a.get("t"))
        elif op == "delete": review.delete(p, a["i"], r=rr)
        elif op == "video": review.set_video(p, a.get("video"), r=rr)
        elif op == "proxy":   # 修正前の動画の軽い版(server.preview_video の裏処理と同じ proxy.make_video(audio=True))
            from vedit import proxy
            st = review.state(p, rr)
            if st.get("video"): proxy.make_video(p, p.resolve(st["video"]), True)
        elif op == "export": r = {"text": review.export_text(p, undone=bool(a.get("undone")), r=rr)}
        elif op != "state": raise ValueError("不明な操作: " + op)
        if op != "export":
            r = review.state(p, rr)
            if r.get("video"):
                from vedit import proxy
                r["video_abs"] = str(p.resolve(r["video"]))
                if r.get("video_pv"): r["pv_path"] = str(proxy.video_proxy_path(p, p.resolve(r["video"]), True))
    elif kind == "proxy":   # プレビュー用の軽い映像(できていれば作らずに返す)
        from vedit import proxy
        src = p.resolve(p.data["source"])
        r = {"src": str(src), "path": str(proxy.make(p, prog=lambda x: out("P", x)))}
    elif kind == "wave":
        from vedit import server as sv
        sv.waveform(p)
        r = {"ok": True}
    elif kind == "saveas":
        data = json.loads(Path(a["data"]).read_text("utf-8"))
        r = {"path": str(p.save_as(a["dst"], data).path)}
    elif kind == "vimg":
        act = a.get("act", "new")
        if act == "new":
            k = a.get("kind", "image")
            src = p.resolve(p.data["source"])
            info = ff.probe(src) if src.is_file() else {}
            W, H = info.get("width") or 1920, info.get("height") or 1080
            folder = p.dir / "img"
            path = vimg.unique(folder, "telop" if k == "telop" else "image")
            layers = []
            if k == "telop":
                layers.append(vimg.telop_layer(p.style("telop"), W, H))
            elif k == "frame":
                t = float(a["t"])
                bg = folder / f"frame_{int(t // 60):02d}m{t % 60:05.2f}s.png"
                rd.render(p, out=str(bg), frame=t, full=True)
                layers.append({"type": "image", "name": "下敷き(書き出さない)", "src": bg.name, "x": W / 2, "y": H / 2, "w": W, "h": H, "rot": 0, "locked": True, "export": False})
            png = vimg.create(path, vimg.new_doc(W, H, layers))
            r = {"path": rel(path), "png": rel(png)}
        else:
            f = Path(a["path"]); f = f if f.is_absolute() else p.dir / f
            created = False
            if not vimg.is_doc(f):
                if f.suffix.lower() not in vimg.RASTER: raise ValueError("画像ではありません")
                side = f.with_name(f.stem + vimg.EXT)
                created = not side.exists()
                f = side if side.exists() else vimg.from_raster(f)
            r = {"path": rel(f), "png": rel(vimg.png_of(f)), "created": created}
    else:
        raise ValueError("不明なジョブ: " + kind)
    out("R", r)
except BaseException as e:
    import traceback
    out("E", (str(e) or traceback.format_exc()))
    sys.exit(1)
)PY");
}

Cmd jobCmd(const QString& kind, const QString& proj, const QJsonObject& args) {
    Cmd c;
    c.prog = pythonExe();
    c.args = {QStringLiteral("-I"), QStringLiteral("-X"), QStringLiteral("utf8"), QStringLiteral("-c"), jobScript(), kind, proj,
              QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)), veditRoot()};
    c.workDir = QFileInfo(proj).absolutePath();
    return c;
}

Cmd audioCmd(const QString& proj, bool force) {
    Cmd c;
    c.prog = vaudioExe();
    c.args = {QStringLiteral("--process"), proj};
    if (force) c.args << QStringLiteral("--force");
    return c;
}

Cmd vimgOpenCmd(const QString& file) {
    Cmd c;
    c.prog = vimgExe();
    if (!file.isEmpty()) c.args << file;
    return c;
}

Cmd photoshopCmd(const QString& file) {
    Cmd c;
    c.prog = photoshopExe();
    c.args << file;
    return c;
}

Cmd explorerCmd(const QString& path) {
    Cmd c;
    c.prog = QStringLiteral("explorer.exe");
    const QString p = QDir::toNativeSeparators(path);
    if (QFileInfo(path).isDir()) c.args << p; else c.args << QStringLiteral("/select,") + p;
    return c;
}

static QString errText(const QByteArray& body) {
    const QString t = QJsonDocument::fromJson("[" + body + "]").array().at(0).toString();
    return t.isEmpty() ? QString::fromUtf8(body) : t;
}

static void noWindow(QProcess* p) {
#ifdef Q_OS_WIN
    p->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW; });
#else
    Q_UNUSED(p);
#endif
}

static void setupEnv(QProcess* p, const Cmd& c) {
    QProcessEnvironment e = QProcessEnvironment::systemEnvironment();
    e.insert("PYTHONIOENCODING", "utf-8");
    e.insert("PYTHONUTF8", "1");
    p->setProcessEnvironment(e);
    if (!c.workDir.isEmpty()) p->setWorkingDirectory(c.workDir);
}

std::function<bool(const Cmd&)> launcher;

bool launch(const Cmd& c) {
    if (launcher) return launcher(c);
    if (c.prog.isEmpty()) return false;
    QProcess p;
    p.setProgram(c.prog);
    p.setArguments(c.args);
    if (!c.workDir.isEmpty()) p.setWorkingDirectory(c.workDir);
    // 切り離し起動。窓を出さない必要があるのは explorer 以外のコンソール系だが、GUI は自分で窓を持つので CREATE_NO_WINDOW は付けない
    return p.startDetached();
}

Result runSync(const Cmd& c, int timeoutMs) {
    Result r;
    QProcess p;
    noWindow(&p);
    setupEnv(&p, c);
    p.setProgram(c.prog);
    p.setArguments(c.args);
    p.start();
    if (!p.waitForStarted(10000)) { r.error = "起動できません: " + c.prog; return r; }
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(3000); r.error = "時間切れ"; return r; }
    r.code = p.exitCode();
    r.out = p.readAllStandardOutput();
    r.err = p.readAllStandardError();
    r.ok = p.exitStatus() == QProcess::NormalExit && r.code == 0;
    for (const QByteArray& ln : r.out.split('\n')) {
        const QByteArray l = ln.trimmed();
        if (l.size() < 2) continue;
        if (l[0] == 'R') { r.obj = QJsonDocument::fromJson(l.mid(1)).object(); }
        else if (l[0] == 'E') { r.error = errText(l.mid(1)); r.ok = false; }
    }
    if (!r.ok && r.error.isEmpty()) r.error = QString::fromUtf8(r.err).trimmed().right(800);
    return r;
}

// ---- Job ----
Job::Job(const Cmd& c, bool isAudio, QObject* parent) : QObject(parent), c_(c), audio_(isAudio) {}
Job::~Job() {
    if (p_ && p_->state() != QProcess::NotRunning) { p_->kill(); p_->waitForFinished(2000); }
}
bool Job::running() const { return p_ && p_->state() != QProcess::NotRunning; }
void Job::cancel() { cancelled_ = true; if (running()) p_->kill(); }

void Job::start() {
    p_ = new QProcess(this);
    noWindow(p_);
#ifdef Q_OS_WIN
    // 裏で走る仕事(波形・無音・書き出しなど)は優先度「通常以下」。空いた CPU は全部使い、操作中の画面にだけ譲る
    p_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) { a->flags |= CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS; });
#endif
    setupEnv(p_, c_);
    connect(p_, &QProcess::readyReadStandardOutput, this, &Job::readOut);
    connect(p_, &QProcess::readyReadStandardError, this, &Job::readErr);
    connect(p_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) { error_ = "起動できません: " + c_.prog; emit finished(false, QJsonObject(), error_); }
    });
    connect(p_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) { onFinished(code); });
    p_->setProgram(c_.prog);
    p_->setArguments(c_.args);
    p_->start();
}

void Job::readOut() {
    bufOut_ += p_->readAllStandardOutput();
    if (audio_) { all_ += bufOut_; bufOut_.clear(); return; }
    int i;
    while ((i = bufOut_.indexOf('\n')) >= 0) {
        const QByteArray l = bufOut_.left(i).trimmed();
        bufOut_.remove(0, i + 1);
        if (l.size() < 2) continue;
        if (l[0] == 'P') emit progress(l.mid(1).toDouble(), QString());
        else if (l[0] == 'R') { result_ = QJsonDocument::fromJson(l.mid(1)).object(); gotResult_ = true; }
        else if (l[0] == 'E') { error_ = errText(l.mid(1)); gotError_ = true; }
    }
}

void Job::readErr() {
    bufErr_ += p_->readAllStandardError();
    int i;
    while ((i = bufErr_.indexOf('\n')) >= 0) {
        const QString l = QString::fromUtf8(bufErr_.left(i)).trimmed();
        bufErr_.remove(0, i + 1);
        if (l.isEmpty()) continue;
        errTail_ = l;
        static const QRegularExpression re(R"(^\[\s*(\d+)%\]\s*(.*)$)");
        const auto m = re.match(l);
        if (m.hasMatch()) emit progress(m.captured(1).toInt() / 100.0, m.captured(2));
    }
}

void Job::onFinished(int code) {
    readOut();
    readErr();
    if (audio_) {
        const QJsonDocument d = QJsonDocument::fromJson(all_.trimmed());
        if (code == 0) { emit finished(true, d.object(), QString()); return; }
        emit finished(false, QJsonObject(), errTail_.isEmpty() ? QStringLiteral("音声処理に失敗しました (終了コード %1)").arg(code) : errTail_);
        return;
    }
    if (cancelled_) { emit finished(false, QJsonObject(), QStringLiteral("中止しました")); return; }
    if (code == 0 && gotResult_) { emit progress(1.0, QString()); emit finished(true, result_, QString()); return; }
    QString e = gotError_ ? error_ : errTail_;
    if (e.isEmpty()) e = QStringLiteral("失敗しました (終了コード %1)").arg(code);
    emit finished(false, QJsonObject(), e);
}

// ---- SysMon ----
SysMon::SysMon(QObject* parent) : QObject(parent) {
    qRegisterMetaType<SysStats>("vui::bridge::SysStats");
    smiPath_ = QStandardPaths::findExecutable("nvidia-smi");
    smiBad_ = smiPath_.isEmpty();
}

static unsigned long long ft(const FILETIME& f) { return ((unsigned long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }

SysStats SysMon::sample() {
    SysStats s = last_;
#ifdef Q_OS_WIN
    FILETIME i, k, u;
    if (GetSystemTimes(&i, &k, &u)) {
        const unsigned long long idle = ft(i), total = ft(k) + ft(u);
        if (total0_ && total > total0_) s.cpu = 100.0 * (1.0 - double(idle - idle0_) / double(total - total0_));
        else if (!total0_) s.cpu = 0;
        idle0_ = idle; total0_ = total;
        if (s.cpu < 0) s.cpu = 0;
        if (s.cpu > 100) s.cpu = 100;
    }
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        s.mem = ms.dwMemoryLoad;
        s.memTotal = ms.ullTotalPhys / 1073741824.0;
        s.memUsed = (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0;
    }
#endif
    last_ = s;
    return s;
}

void SysMon::pollGpu() {
    if (smiBad_ || (smi_ && smi_->state() != QProcess::NotRunning)) return;
    if (!smi_) {
        smi_ = new QProcess(this);
        noWindow(smi_);
        connect(smi_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
            const QStringList f = QString::fromUtf8(smi_->readAllStandardOutput()).split('\n').value(0).split(',');
            if (code != 0 || f.size() < 3) { smiBad_ = true; last_.gpu = last_.vramUsed = last_.vramTotal = -1; return; }
            last_.gpu = f[0].trimmed().toDouble();
            last_.vramUsed = f[1].trimmed().toDouble() / 1024;
            last_.vramTotal = f[2].trimmed().toDouble() / 1024;
        });
        connect(smi_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) { smiBad_ = true; });
    }
    smi_->start(smiPath_, {"--query-gpu=utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"});
}

void SysMon::tick() {
    pollGpu();
    emit updated(sample());
}

QString SysMon::format(const SysStats& s) {
    auto cls = [](double x) { return x >= 95 ? QStringLiteral("hot") : x >= 80 ? QStringLiteral("warm") : QString(); };
    QStringList parts;
    auto item = [&](const QString& k, double x, const QString& txt) {
        if (x < 0) return;
        parts << QStringLiteral("<span style='color:%1'><b style='color:#8d8d99;font-weight:normal'>%2</b> %3</span>")
                     .arg(cls(x) == "hot" ? "#ff6b6b" : cls(x) == "warm" ? "#ffc233" : "#e6e6ea", k, txt.isEmpty() ? QString::number(qRound(x)) + "%" : txt);
    };
    item("CPU", s.cpu, QString());
    item("GPU", s.gpu, QString());
    item("VRAM", s.vramTotal > 0 ? s.vramUsed / s.vramTotal * 100 : -1, s.vramTotal > 0 ? QStringLiteral("%1/%2GB").arg(s.vramUsed, 0, 'f', 1).arg(qRound(s.vramTotal)) : QString());
    item("メモリ", s.mem, s.mem >= 0 ? QStringLiteral("%1% (%2/%3GB)").arg(qRound(s.mem)).arg(s.memUsed, 0, 'f', 1).arg(qRound(s.memTotal)) : QString());
    return parts.join("&nbsp;&nbsp;");
}

}  // namespace vui::bridge
