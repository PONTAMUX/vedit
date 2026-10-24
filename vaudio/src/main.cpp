// vaudio: コマンドライン処理(--process / --worker)と GUI 起動。
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "engine.h"
#include "json.h"

#ifdef VAUDIO_GUI
#include "gui.h"
#endif

using namespace vaudio;

static void ensureConsole() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) return;  // リダイレクト済み
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

static std::string toU8(const std::wstring& w) { return narrowPath(w); }

int main(int, char**) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(toU8(wargv[i]));
    LocalFree(wargv);

    auto has = [&](const char* k) { for (auto& a : args) if (a == k) return true; return false; };
    auto val = [&](const char* k) -> std::string {
        for (size_t i = 0; i + 1 < args.size(); i++) if (args[i] == k) return args[i + 1];
        return std::string();
    };

    if (has("--worker")) return workerMain(val("--worker"));

    if (has("--process")) {
        ensureConsole();
        setvbuf(stdout, nullptr, _IONBF, 0);
        ProcessOptions opt;
        opt.force = has("--force");
        opt.outDir = val("--out");
        std::string w = val("--workers");
        if (!w.empty()) opt.workersOverride = atoi(w.c_str());
        bool quiet = has("--quiet");
        if (!quiet)
            opt.progress = [](double f, const std::string& m) { fprintf(stderr, "[%3d%%] %s\n", (int)(f * 100), m.c_str()); };
        try {
            vj::Value info = processProject(val("--process"), opt);
            printf("%s\n", vj::dump(info, vj::DumpOpt{false, false, -1}).c_str());
            return 0;
        } catch (const std::exception& e) {
            fprintf(stderr, "エラー: %s\n", e.what());
            return 1;
        }
    }

#ifdef VAUDIO_GUI
    std::string file;
    for (size_t i = 1; i < args.size(); i++) if (args[i].compare(0, 2, "--") != 0) { file = args[i]; break; }
    return runGui(file);
#else
    ensureConsole();
    fprintf(stderr, "usage: vaudio --process <proj.vedit.json> [--force] [--out <dir>]\n");
    return 2;
#endif
}
