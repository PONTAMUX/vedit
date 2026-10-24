// vaudio: 音声処理エンジン。vedit/audio.py の process() の移植。
#pragma once
#include <atomic>
#include <functional>
#include <string>

#include "dsp.h"
#include "json.h"

namespace vaudio {

struct ProcessOptions {
    bool force = false;
    std::string outDir;                 // 空ならプロジェクトのキャッシュ(<dir>/<stem>.cache)
    int workersOverride = 0;            // >0 なら並列数を固定(テスト用)
    std::function<void(double, const std::string&)> progress;  // 0..1, メッセージ
    std::atomic<bool>* cancel = nullptr;
    std::string exePath;                // 子プロセス(--worker)に使う実行ファイル。空なら自分自身
};

// プロジェクト(.vedit.json)の audio.chain を処理し、audio.wav / audio.json を書く。info(audio.json の中身)を返す。
// 失敗は std::runtime_error。キャンセルは std::runtime_error("中止しました")。
vj::Value processProject(const std::string& projectPath, const ProcessOptions& opt);

// 子プロセス(--worker <jobfile>)の本体。戻り値は終了コード。
int workerMain(const std::string& jobFile);

// 共通ユーティリティ
std::string defaultCacheDir(const std::string& projectPath);        // <dir>/<stem>.cache
std::string resolveInProject(const std::string& projectPath, const std::string& p);  // Python の Project.resolve + str(Path)
std::wstring widenPath(const std::string& u8);
std::string narrowPath(const std::wstring& w);
bool readFileText(const std::string& u8path, std::string& out);
bool writeFileText(const std::string& u8path, const std::string& text);

// 手順1つを実行するプラグインを作る(type = vst3 / plugin / gain)。GUI の試聴などでも使う。
std::unique_ptr<Plugin> buildStepPlugin(const std::string& projectDir, const vj::Value& step);

}  // namespace vaudio
