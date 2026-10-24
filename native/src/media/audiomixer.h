// 音声ミキサー: 出力時刻からの PCM(48kHz stereo float)を作る。画面・出力デバイスとは独立。
// 組み立ては render.py の build_audio / _cut_wav / _sfx_graph と同じ(A1 のカット+継ぎ目フェード、amute/aslip、sfx、
// 差し込みの音、bgm)。volume の enable・afade・sidechaincompress・amix・alimiter は libavfilter をそのまま使う
// (FFmpeg と同じ式になる)。シーク時はグラフを作り直し、少し手前から流して状態(リミッター・圧縮の包絡)を温める。
#pragma once
#include <QString>
#include <cstdint>
#include <memory>
#include "core/project.h"
#include "core/timeline.h"

namespace vn {

constexpr int kSampleRate = 48000;

class AudioMixer {
public:
    AudioMixer();
    ~AudioMixer();
    bool init(const Project& p, const Timeline& tl, QString* err);
    int64_t totalSamples() const;
    double totalSeconds() const { return (double)totalSamples() / kSampleRate; }
    int64_t position() const;               // 次に read で出るサンプル位置
    void seek(int64_t sample);               // 0..totalSamples
    void read(float* l, float* r, int n);    // 次の n サンプル(終わりを過ぎたぶんは 0)。位置は進む
    bool setProcessed(bool on);              // A1 を処理後/生の音源に切り替え(あとで seek すること)
    QString info() const;                    // 読み込んだ要素の件数(確認用)

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace vn
