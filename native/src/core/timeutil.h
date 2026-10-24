// 時刻の読み書きと、カット後のタイムライン換算(vedit/timeutil.py の移植)
#pragma once
#include <QJsonValue>
#include <utility>
#include <vector>

namespace vn {

// 秒(数値) / "12.5" / "1:02.5" / "01:02:03.5" → 秒。読めなければ false
bool parseTime(const QJsonValue& v, double* out);
double parseTimeOr(const QJsonValue& v, double dflt);

class TimeMap {
public:
    static constexpr double MIN_KEEP = 0.02;
    struct Piece {      // ("src", a, b) / ("gap", 長さ)
        bool gap;
        double a, b;    // gap のときは a=長さ
        double len() const { return gap ? a : b - a; }
    };
    using Ranges = std::vector<std::pair<double, double>>;

    TimeMap() = default;
    TimeMap(double duration, const Ranges& cuts, const Ranges& gaps);

    double duration = 0, total = 0;
    Ranges cuts, keeps, gaps;    // gaps: (元素材の時刻, 長さ) 時刻順

    std::vector<Piece> pieces() const;
    bool gapAt(double at, double* outStart, double* len) const;
    double out(double t) const;
    double outEnd(double t) const;
    bool outRange(double a, double b, double* oa, double* ob) const;
};

}  // namespace vn
