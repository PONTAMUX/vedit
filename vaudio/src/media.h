// vaudio: 音声の読み込み(FFmpeg ライブラリ)、WAV 書き出し、ラウドネス測定、dynaudnorm。
// Python 版は ffmpeg コマンドを呼ぶ所を、同じ ffmpeg のライブラリを直接使って同じ結果を得る。
#pragma once
#include <functional>
#include <string>

#include "dsp.h"

namespace vaudio {

struct Decoded {
    Audio ch;          // チャンネルごとの float
    int sampleRate = 0;
};

// ffmpeg -i src -vn -c:a pcm_f32le -ar sr 相当。sr<=0 なら元のサンプルレート。
// 失敗は std::runtime_error。
Decoded decodeAudio(const std::string& utf8Path, int sr);

// ffmpeg -af ebur128 の統合ラウドネス(LUFS。ログに出る小数1桁の値)
double loudness(const Audio& a, int sr);

// ffmpeg -af dynaudnorm=<args> 相当
Audio dynaudnorm(const Audio& a, int sr, const std::string& args);

// ffmpeg -af volume=<db>dB -c:a pcm_s24le 相当で WAV を書く
void writeWav24(const std::string& utf8Path, const Audio& a, int sr, double gainDb);

// 16/24/32bit PCM・float の WAV を float で読む(GUI の波形表示・テスト用。decodeAudio でも読める)
Decoded readAnyAudio(const std::string& utf8Path);

}  // namespace vaudio
