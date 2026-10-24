// ASS を libavfilter の ass フィルタ(libass)で透明な RGBA に描く。専用スレッドで 1 コマずつ(キーごとに 1 回)描き、
// 結果(絵のある範囲だけに切り詰めた画像)を貯めておく。書き出し(ffmpeg の ass フィルタ)と同じ描画器なので見た目が一致する
#pragma once
#include <QHash>
#include <QImage>
#include <QString>
#include <condition_variable>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include "media/qtthread.h"

namespace vn {

class SubRender {
public:
    struct Img {
        QImage img;      // RGBA8888 premultiplied。null = 何も描かれない
        int x = 0, y = 0;
    };

    SubRender(int W, int H, const QString& assText);
    ~SubRender();

    bool peek(const QString& key, Img* out);                  // できていれば true
    void request(const QString& key, long long nowMs, bool urgent = false);        // 先回り(キューの後ろ)。できている/依頼済みなら何もしない
    bool get(const QString& key, long long nowMs, Img* out);  // できるまで待つ(キューの先頭)
    bool failed() const { return failed_; }

private:
    void run();
    bool initGraph();
    Img renderOne(long long nowMs);

    int W_, H_;
    QString assText_, assPath_;
    vn::Thread th_;
    std::mutex mu_;
    std::condition_variable cv_, done_;
    std::deque<std::pair<QString, long long>> q_;
    QHash<QString, Img> cache_;
    std::list<QString> order_;
    QHash<QString, int> pending_, must_;
    long long lastReq_ = 0;
    bool quit_ = false;
    bool failed_ = false;

    struct Graph;
    Graph* g_ = nullptr;
};

}  // namespace vn
