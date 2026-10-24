// vaudio: VST3 ホスト(Steinberg VST3 SDK の hosting クラスを使用)。pedalboard.load_plugin(...) 相当の振る舞い。
//   パラメータ名は pedalboard と同じ python 名(例 "sensitivity", "click_widening")で指定する。
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dsp.h"

namespace vaudio {

struct Vst3ParamInfo {
    std::string pyName;     // pedalboard の python 名
    std::string title;
    std::string units;
    std::string type;       // "float" | "str" | "bool"
    double minValue = 0, maxValue = 0;
    std::vector<std::string> validValues;  // type=="str" のとき
    std::string currentText;
};

class Vst3Editor {
public:
    virtual ~Vst3Editor() {}
    virtual void size(int& w, int& h) = 0;
    virtual void onSize(int w, int h) = 0;
    virtual void close() = 0;
};

class Vst3Plugin : public Plugin {
public:
    // 失敗は std::runtime_error
    explicit Vst3Plugin(const std::string& utf8Path);
    ~Vst3Plugin() override;

    // python 名でパラメータを設定。値は JSON(数値・文字列・真偽)。pedalboard の setattr と同じ解釈。
    void setParam(const std::string& pyName, const vj::Value& v);
    std::vector<Vst3ParamInfo> describeParams();
    // エディタで変えた後の値を python 名 → JSON 値で返す(setParam 後の状態と違うものだけ)
    vj::Value changedParams();

    std::string pluginName() const;

    // エディタ。親 HWND(void*)に埋め込む。onResize は プラグイン側からのサイズ変更要求。
    bool hasEditor();
    std::unique_ptr<Vst3Editor> openEditor(void* parentHwnd, std::function<void(int, int)> onResize);

    // Plugin
    void prepare(const Spec& spec) override;
    int process(float** ch, int numCh, int n) override;
    void reset() override;
    int getLatencyHint() override;

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};

}  // namespace vaudio
