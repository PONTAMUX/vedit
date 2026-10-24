// 音声の再生: ミキサー(別スレッド)→ リング → 出力。音声が時計の親(映像は clock() を見て合わせる)。
// 出力は 2 種類: Null(デバイスを開かず実時間で PCM を捨てる。確認用)と Device(QAudioSink)。
// 一時停止・再開・シークは 10ms 程度のゲイン変化(シーク時は直前の音の尾を短くフェードアウト)でプチ音を避ける。
#pragma once
#include <QString>
#include <memory>
#include "core/project.h"
#include "core/timeline.h"

namespace vn {

class AudioPlayer {
public:
    enum class Sink { Null, Device };
    struct Impl;
    AudioPlayer();
    ~AudioPlayer();
    bool init(const Project& p, const Timeline& tl, QString* err);   // ミキサーを作る(重い)
    bool reinit(const Project& p, const Timeline& tl, QString* err);   // project が変わった時にミキサーを作り直す(出力は開いたまま)
    struct Levels { bool valid = false; double peak[2] = {0, 0}, rms[2] = {0, 0}; };   // 線形の振幅。出力へ渡した音そのもの(音量・フェード後)
    Levels takeLevels();        // 前に呼んでから今までに出力へ渡した音の L/R の peak / RMS(メーター用。呼ぶたびに区切る。新しい音が無ければ直前の値)
    bool open(Sink kind);                                            // 出力を始める(常に動かし続け、停止中は無音を流す)
    void close();
    void play(double t);        // t から再生(シークしてフェードイン)。停止中でも再生中でも可
    void pause();               // フェードアウトして止める
    void seek(double t);        // 再生中は尾をフェードアウトして飛ぶ。停止中は位置だけ動かす
    void setVolume(double gain, bool muted);   // 音量(2乗済みの係数)とミュート。10ms ほどかけて変わる(プチ音防止)
    void setRate(double r);     // 倍速 0.25..8(音程も変わる補間再生)。clock() は素材側の位置で進む
    double rate() const;
    void setProcessed(bool on); // A1 を処理後の音(audio.wav)/ 生の音(元素材)に切り替える(その位置から流し直す)
    bool processed() const;
    double peak() const;        // 直近に出力へ渡したブロックの最大振幅(確認用)
    double volume() const;      // 目標の音量係数(ミュート中は 0)
    bool playing() const;       // 再生を求めている(バッファ待ち・終端も含む)
    bool streaming() const;     // 実際に音が流れている(バッファ待ちでない)
    bool finished() const;      // 終端まで流した
    double clock() const;       // いま聞こえている位置(秒)。再生中は連続して進む
    int underruns() const;      // 流れ始めてから、ミキサーの生成が間に合わず無音を挟んだ回数
    double totalSeconds() const;
    QString info() const;

private:
    std::unique_ptr<Impl> d;
};

}  // namespace vn
