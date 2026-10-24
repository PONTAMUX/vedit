// libav でのデコード(H.264/HEVC などは NVDEC=hwaccel cuda、だめなら CPU)と先読み。
// 別スレッドが「要求された時刻 + ahead 秒」までデコードしてキューに溜める。要求が離れたらシークして正確なコマまで読み直す。
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "core/timeline.h"
#include "media/qtthread.h"
#include "media/cudainterop.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace vn {

struct VFrame {
    enum Kind { NV12, P010, RGBA };
    Kind kind = NV12;
    int w = 0, h = 0;
    std::unique_ptr<uint8_t[]> y, uv;   // RGBA のときは y に w*h*4
    int cw = 0, ch = 0;                 // 色差の大きさ
    double pts = 0;                     // 秒
    int cs = 0;                         // 0=BT.709 1=BT.601 2=BT.2020
    bool full = false;                  // フルレンジ
    uint64_t id = 0;
    std::shared_ptr<cuda::DevBuf> dev;   // 非 null なら絵は VRAM の中(y・uv は空)
};
using FramePtr = std::shared_ptr<const VFrame>;

// 動画の情報(同じパスはキャッシュ)。画像は isImage=true で幅・高さだけ
MediaProps probeMedia(const QString& path);

class VideoStream {
public:
    VideoStream(const std::string& path, bool tryHw, double initialTime);
    ~VideoStream();
    VideoStream(const VideoStream&) = delete;

    // 非ブロック: この時刻を使う予定と伝える(先読みを始める。離れていればシーク)
    void retarget(double time, double ahead);
    // time 以前で最新のコマ。wait=true なら、そのコマが確定するまで(次のコマが来る/終端)待つ。無ければ null
    FramePtr frameAt(double time, bool wait, double ahead = 1.0);

    bool failed() const { return failed_; }
    // 再生が始まる前に NVDEC のデコーダを作っておく(paths の本数だけ同時に開いて最初のコマを出し、使い終えたものを再利用の置き場へ戻す)。
    // 再生中の NVDEC 作成は GPU 全体を数十〜百数十 ms 止め、描画が 1 コマ落ちる原因になるため。
    static void warmUp(const std::vector<std::string>& paths);
    bool usingHw() const { return usingHw_; }
    double endTime() const { return endTime_; }   // 最後のコマの終わり(分かれば)

private:
    void run();
    bool openAll(bool hw);
    void closeAll();
    FramePtr convert(AVFrame* f);
    int decodeOne(FramePtr* out);   // 1=コマあり 0=終端 -1=エラー
    void updateTargetLocked(double time, double ahead);
    bool needMoreLocked() const;
    void doSeek(double t);

    std::string path_;
    bool tryHw_, hw_ = false;
    double initial_;
    vn::Thread th_;
    std::mutex m_;
    std::condition_variable cv_work_, cv_done_;
    bool quit_ = false, seekReq_ = false, eof_ = false, opened_ = false;
    std::atomic<bool> failed_{false}, usingHw_{false};
    std::atomic<double> endTime_{0};
    uint64_t gen_ = 0;
    double seekT_ = 0, target_ = 0, ahead_ = 1.0;
    double eps_ = 0.004;
    std::deque<FramePtr> q_;
    bool gotFrame_ = false;

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    bool pooled_ = false, noPool_ = false;
    AVPacket* pkt_ = nullptr;
    AVFrame* fr_ = nullptr;
    SwsContext* sws_ = nullptr;
    int vidx_ = -1;
    double tb_ = 0, fps_ = 30, startTime_ = 0, lastPts_ = 0;
    bool draining_ = false;
    uint64_t nextId_ = 1;
};

}  // namespace vn
