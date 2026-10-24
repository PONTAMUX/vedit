// 画面が止まったときの記録。メインスレッドが 0.4 秒以上イベントを回さないと、見張りのスレッドが
// メインスレッドを一瞬止めて呼び出しの段(アドレス)を写し、%APPDATA%/vedit/stall.log に書く。
// アドレスは「モジュール+オフセット」で書くので、vedit-native.exe の分は addr2line で関数名に戻せる
#include "stallwatch.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <atomic>
#include <thread>
#include "media/qtthread.h"
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <algorithm>
#include <vector>

namespace {
std::atomic<qint64> g_beat{0};
std::atomic<bool> g_stop{false};
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

// 止めた相手のスレッドの段を巻き戻す。止めている間はヒープも DLL の情報も触らない(相手が鍵を持っていると止まるので)
// 止めている間はレジスタとスタックの写しを取るだけにして、すぐ再開する。段の巻き戻しは写しの上でやる
// (RtlLookupFunctionEntry は DLL の表の鍵を取ることがあり、止めたまま待つと相手の止まりを長引かせる)
int capture(HANDLE th, DWORD64* out, int max, std::vector<DWORD64>* raw = nullptr) {
    static thread_local std::vector<BYTE> stk(512 * 1024);
    if (SuspendThread(th) == (DWORD)-1) return 0;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    SIZE_T got = 0;
    const bool ok = GetThreadContext(th, &c);
    if (ok) {
        MEMORY_BASIC_INFORMATION mi{};
        SIZE_T want = stk.size();
        if (VirtualQuery(reinterpret_cast<void*>(c.Rsp), &mi, sizeof(mi)))
            want = std::min<SIZE_T>(want, reinterpret_cast<BYTE*>(mi.BaseAddress) + mi.RegionSize - reinterpret_cast<BYTE*>(c.Rsp));
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(c.Rsp), stk.data(), want, &got);
    }
    ResumeThread(th);
    if (!ok || got < 8) return 0;
    if (raw) {   // スタックの先頭の値(待っている鍵のアドレスを探すため)。rcx 等のレジスタも足す
        raw->assign({c.Rcx, c.Rdx, c.R8, c.R9, c.Rbx, c.Rsi, c.Rdi, c.R12, c.R13, c.R14, c.R15});
        const auto* w = reinterpret_cast<const DWORD64*>(stk.data());
        for (SIZE_T i = 0; i < std::min<SIZE_T>(got / 8, 1024); ++i) raw->push_back(w[i]);
    }
    const DWORD64 lo = c.Rsp, hi = c.Rsp + got, delta = reinterpret_cast<DWORD64>(stk.data()) - lo;
    auto inStk = [&](DWORD64 a) { return a >= lo && a + 8 <= hi; };
    c.Rsp += delta;
    if (inStk(c.Rbp)) c.Rbp += delta;
    const DWORD64 clo = lo + delta, chi = hi + delta;
    int n = 0;
    while (n < max && c.Rip && c.Rsp >= clo && c.Rsp + 8 <= chi) {
        out[n++] = c.Rip;
        DWORD64 base = 0;
        auto* fe = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
        if (!fe) { c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp); c.Rsp += 8; continue; }   // 葉の関数
        void* hd = nullptr; DWORD64 ef = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, fe, &c, &hd, &ef, nullptr);
        if (inStk(c.Rbp)) c.Rbp += delta;   // 本物のスタックを指す値は写しへ付け替える
    }
    return n;
}

// モジュールの一覧は止まっていない時に作っておく(止まり中に GetModuleHandleEx を呼ぶと、DLL の鍵を握られている時に見張りまで止まる)
struct Mod { DWORD64 base, size; QString name; };
std::vector<Mod> g_mods;
void refreshMods() {
    HMODULE hs[1024]; DWORD need = 0;
    if (!EnumProcessModules(GetCurrentProcess(), hs, sizeof(hs), &need)) return;
    std::vector<Mod> v;
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
        MODULEINFO mi{}; wchar_t buf[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), hs[i], &mi, sizeof(mi))) continue;
        const DWORD len = GetModuleBaseNameW(GetCurrentProcess(), hs[i], buf, MAX_PATH);
        v.push_back({reinterpret_cast<DWORD64>(mi.lpBaseOfDll), mi.SizeOfImage, QString::fromWCharArray(buf, int(len))});
    }
    g_mods = std::move(v);
}
QString where(DWORD64 a) {
    for (const auto& m : g_mods) if (a >= m.base && a < m.base + m.size) return QString("%1+0x%2").arg(m.name).arg(a - m.base, 0, 16);
    return QString("0x%1").arg(a, 0, 16);
}
// DLL の鍵(PEB の LoaderLock)を今握っているスレッドの ID。0 なら誰も握っていない
DWORD loaderLockOwner() {
    auto* peb = reinterpret_cast<BYTE*>(NtCurrentTeb()->ProcessEnvironmentBlock);
    auto* cs = *reinterpret_cast<RTL_CRITICAL_SECTION**>(peb + 0x110);
    return cs ? DWORD(reinterpret_cast<ULONG_PTR>(cs->OwningThread)) : 0;
}
// スタックの中の値のうち、鍵(RTL_CRITICAL_SECTION)らしきものを探し、その持ち主のスレッド ID を返す
std::vector<std::pair<DWORD64, DWORD>> csOwners(const std::vector<DWORD64>& raw, DWORD self) {
    std::vector<std::pair<DWORD64, DWORD>> r;
    for (DWORD64 a : raw) {
        if (a < 0x10000 || (a & 7) || a > 0x7fffffffffffULL) continue;
        RTL_CRITICAL_SECTION cs{}; SIZE_T got = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &cs, sizeof(cs), &got) || got != sizeof(cs)) continue;
        const DWORD own = DWORD(reinterpret_cast<ULONG_PTR>(cs.OwningThread));
        if (!own || own == self || cs.RecursionCount < 1 || cs.RecursionCount > 64) continue;
        if (reinterpret_cast<ULONG_PTR>(cs.OwningThread) > 0xffffffffULL) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, own);
        if (!h) continue;
        const bool mine = GetProcessIdOfThread(h) == GetCurrentProcessId();
        CloseHandle(h);
        if (!mine) continue;
        bool dup = false;
        for (auto& x : r) dup |= x.first == a;
        if (!dup) r.push_back({a, own});
        if (r.size() >= 6) break;
    }
    return r;
}
qint64 threadCpuMs(HANDLE th) {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(th, &c, &e, &k, &u)) return -1;
    auto v = [](const FILETIME& f) { return (qint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(k) + v(u)) / 10000;
}
}

void startStallWatch() {
    HANDLE th = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &th, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (!th) return;
    g_beat = nowMs();
    auto* t = new QTimer(qApp);
    QObject::connect(t, &QTimer::timeout, [] { g_beat = nowMs(); });
    t->start(50);
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, [] { g_stop = true; });
    const QString path = qEnvironmentVariable("APPDATA", QDir::homePath()) + "/vedit/stall.log";
    QDir().mkpath(QFileInfo(path).absolutePath());
    const DWORD mainTid = GetCurrentThreadId();
    vn::Thread([th, path, mainTid] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        qint64 stallFrom = 0, lastShot = 0, lastMods = 0, cpu0 = 0, prevWrite = 0;
        int shots = 0;
        refreshMods();
        while (!g_stop) {
            Sleep(100);
            const qint64 now = nowMs(), beat = g_beat;
            if (now - beat < 400 && now - lastMods > 5000) { refreshMods(); lastMods = nowMs(); }
            if (now - beat < 400) {
                if (stallFrom) {   // 止まりが終わった
                    QFile f(path);
                    if (f.open(QIODevice::Append)) f.write(QString("-- 止まり %1 ms\n\n").arg(beat - stallFrom + 50).toUtf8());
                    stallFrom = 0;
                }
                continue;
            }
            if (!stallFrom) { stallFrom = beat; shots = 0; cpu0 = threadCpuMs(th); }
            if (shots >= 8 || now - lastShot < 300) continue;   // 1回の止まりで最大8枚・0.3秒おき
            DWORD64 a[48], b[48];
            const DWORD owner = loaderLockOwner();
            std::vector<DWORD64> rawM, rawO;
            const int n = capture(th, a, 48, &rawM);
            int nb = 0;
            if (owner && owner != mainTid && owner != GetCurrentThreadId()) {   // DLL の鍵を握っている相手の段も写す
                if (HANDLE oh = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, owner)) { nb = capture(oh, b, 48, &rawO); CloseHandle(oh); }
            }
            // main と DLL鍵の持ち主が待っている鍵を探し、その鍵の持ち主の段もたどる(3段まで)
            QString chain;
            {
                std::vector<std::pair<DWORD, std::vector<DWORD64>>> todo{{mainTid, rawM}};
                if (nb) todo.push_back({owner, rawO});
                std::vector<DWORD> seen{mainTid, owner, GetCurrentThreadId()};
                for (int depth = 0; depth < 3 && !todo.empty(); ++depth) {
                    std::vector<std::pair<DWORD, std::vector<DWORD64>>> next;
                    for (auto& [who, raw] : todo) {
                        for (auto [csAddr, own] : csOwners(raw, who)) {
                            chain += QString("  [%1 が待つ鍵 0x%2 (%3) の持ち主 %4]\n").arg(who).arg(csAddr, 0, 16).arg(where(csAddr)).arg(own);
                            if (std::find(seen.begin(), seen.end(), own) != seen.end()) continue;
                            seen.push_back(own);
                            HANDLE oh = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, own);
                            if (!oh) continue;
                            DWORD64 c[48]; std::vector<DWORD64> rw;
                            const int k = capture(oh, c, 48, &rw);
                            chain += QString("    (CPU %1 ms)\n").arg(threadCpuMs(oh));
                            CloseHandle(oh);
                            for (int i = 0; i < k; ++i) chain += "    " + where(c[i]) + "\n";
                            next.push_back({own, std::move(rw)});
                        }
                    }
                    todo = std::move(next);
                }
            }
            const qint64 t1 = nowMs();
            lastShot = now; ++shots;
            QString s = QString("%1 経過 %2 ms  main CPU %3 ms  DLL鍵 %4  (前の書き込み %5 ms)\n")
                            .arg(QDateTime::currentDateTime().toString("MM-dd hh:mm:ss.zzz")).arg(now - stallFrom).arg(threadCpuMs(th) - cpu0)
                            .arg(owner == 0 ? QString("なし") : owner == mainTid ? QString("main") : QString::number(owner)).arg(prevWrite);
            for (int i = 0; i < n; ++i) s += "  " + where(a[i]) + "\n";
            if (nb) { s += "  [DLL鍵の持ち主 " + QString::number(owner) + "]\n"; for (int i = 0; i < nb; ++i) s += "    " + where(b[i]) + "\n"; }
            s += chain;
            if (shots == 2) {   // 2枚目で全スレッドを写す(鍵の持ち主が何を待っているか、相手を探すため)
                if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
                    THREADENTRY32 te{}; te.dwSize = sizeof(te);
                    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId() || te.th32ThreadID == mainTid) continue;
                        HANDLE oh = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                        if (!oh) continue;
                        const int k = capture(oh, b, 24);
                        const qint64 cpu = threadCpuMs(oh);
                        CloseHandle(oh);
                        s += QString("  [スレッド %1 CPU %2 ms]\n").arg(te.th32ThreadID).arg(cpu);
                        for (int i = 0; i < k; ++i) s += "    " + where(b[i]) + "\n";
                    }
                    CloseHandle(snap);
                }
            }
            QFile f(path);
            if (f.open(QIODevice::Append)) f.write(s.toUtf8());
            prevWrite = nowMs() - t1;
        }
    }).detach();
}
