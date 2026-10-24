// vaudio: pedalboard の処理モデルと、内蔵エフェクト(JUCE 6.1 dsp の移植)。
//
// 移植元(私用。どちらも GPL/AGPL):
//   pedalboard 0.9.26  pedalboard/process.h, plugins/Chain.h, JucePlugin.h, plugins/*.h  (GPL-3.0, Spotify AB)
//   JUCE 6.1.4         modules/juce_dsp  (AGPL-3.0 / JUCE)
// JUCE 本体は使わない。DSP の計算だけを float のまま移植している(Python 版と ~60dB 以上一致させるため)。
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "json.h"

namespace vaudio {

struct Spec {
    double sampleRate = 0;
    uint32_t maximumBlockSize = 0;
    uint32_t numChannels = 0;
};

// pedalboard::Plugin 相当。ブロックは in-place(ProcessContextReplacing)で渡す。
class Plugin {
public:
    virtual ~Plugin() {}
    virtual void prepare(const Spec& spec) = 0;
    // 戻り値は「使える出力サンプル数」(プラグインの遅れがあると n より小さい)
    virtual int process(float** ch, int numCh, int n) = 0;
    virtual void reset() = 0;
    virtual int getLatencyHint() { return 0; }
};

// 複数チャンネルのバッファ(チャンネルごとの配列)
using Audio = std::vector<std::vector<float>>;

// pb.Pedalboard(plugins)(audio, sr, reset=True) 相当。遅れ補正込みで、入力と同じ長さの音を返す。
// plugins の所有権は呼び出し側。
Audio runBoard(const Audio& in, double sampleRate, const std::vector<Plugin*>& plugins, unsigned bufferSize = 8192);

// 内蔵エフェクト / ゲイン。未知の名前・パラメータは例外(std::runtime_error)。
std::unique_ptr<Plugin> makeGain(float gainDb);
std::unique_ptr<Plugin> makeBuiltin(const std::string& name, const vj::Value& params);

// 内蔵エフェクトの名前一覧(GUI 用)
const std::vector<std::string>& builtinNames();

}  // namespace vaudio
