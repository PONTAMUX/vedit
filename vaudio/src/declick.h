// vaudio: 自前 De-click。vedit/declick.py(numpy)の移植。計算は double、入出力は float。
#pragma once
#include "dsp.h"

namespace vaudio {

struct DeclickParams {
    double threshold = 8.0;
    double widen_ms = 0.6;
    double max_ms = 8.0;
    double window_ms = 60.0;
};

// params は {"threshold":..,"widen_ms":..,"max_ms":..,"window_ms":..} の一部(省略は既定値)。
DeclickParams declickParamsFrom(const vj::Value& params);

// audio: チャンネルごとの配列。結果を返し、修復数を clicks に加算する。
Audio declick(const Audio& audio, int sr, const DeclickParams& p, int& clicks);

}  // namespace vaudio
