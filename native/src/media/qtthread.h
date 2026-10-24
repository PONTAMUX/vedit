#pragma once
// std::thread の代わり。std::thread で作ったスレッドが Qt に触れてから終わると、MinGW 版 Qt の
// スレッド終了の後始末(Qt6Core の thread_local)で落ち、Windows がそのたびにダンプを書いて全スレッドを
// 10 秒ほど止める(応答なしの原因だった)。QThread で作ったスレッドでは起きない
#include <QCoreApplication>
#include <QThread>
#include <memory>
#include <utility>

namespace vn {

class Thread {
public:
    Thread() = default;
    template <class F> explicit Thread(F&& f) : t_(QThread::create(std::forward<F>(f))) { t_->start(); }
    Thread(Thread&&) noexcept = default;
    Thread& operator=(Thread&& o) noexcept { if (this != &o) { join(); t_ = std::move(o.t_); } return *this; }
    ~Thread() { join(); }
    bool joinable() const { return (bool)t_; }
    void join() { if (t_) { t_->wait(); t_.reset(); } }
    void detach() {   // 終わったら自分で消える
        if (!t_) return;
        QThread* t = t_.release();
        if (auto* app = QCoreApplication::instance()) t->moveToThread(app->thread());   // 消すのは主スレッドのイベントループ
        QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
        if (t->isFinished()) t->deleteLater();
    }
private:
    std::unique_ptr<QThread> t_;
};

// 急がない裏仕事のスレッド(古い動画の片付け・先読みの下見)を省電力の扱いにする。Windows の EcoQoS
// (SetThreadInformation の ThreadPowerThrottling)とメモリの優先度「低」。再生・デコード・画面には使わない
// (Microsoft: EcoQoS は性能が要る所や前面の仕事には使わない)
void lowPowerThread();

}
