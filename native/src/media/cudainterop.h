// NVDEC の絵を CPU に戻さず GL テクスチャへ渡す(CUDA ドライバ API を nvcuda.dll から実行時に読む)。
// 無い・失敗した時は呼び出し側が従来の CPU 経路に戻る。環境変数 VN_NO_INTEROP=1 で強制的に無効。
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace vn {

struct VFrame;

namespace cuda {

// VRAM 上の NV12/P010(Y 面のあとに UV 面。行の幅=w*bps バイト)。破棄すると再利用の置き場へ戻る
struct DevBuf {
    unsigned long long ptr = 0;
    size_t size = 0;
    size_t pitch = 0;      // Y・UV 共通の行バイト数
    size_t uvOffset = 0;   // UV 面の先頭までのバイト数
};

bool enabled();                       // nvcuda.dll が読めて、無効化されていない
void disable();                       // 失敗したら以後ずっと CPU 経路
void setContext(void* ctx);           // ffmpeg の CUDA デバイスの CUcontext
void bindThread();                    // 呼んだスレッドにその CUcontext を current にする(何度呼んでもよい)

// デコーダのスレッドから: CUDA フレーム(data[0]/[1] は CUdeviceptr)を自前の VRAM へ写す。失敗なら null
std::shared_ptr<DevBuf> copyFromDecoder(unsigned long long y, int yPitch, unsigned long long uv, int uvPitch, int w, int h, int bps);

// 描画スレッド(GL current)から: 2枚の GL テクスチャ(Y と UV)へ写す。res は初回に作って持ち回す
struct GlTarget {
    void* resY = nullptr;
    void* resUV = nullptr;
};
bool registerTextures(GlTarget& t, unsigned texY, unsigned texUV);
void unregisterTextures(GlTarget& t);
bool copyToTextures(const GlTarget& t, const DevBuf& b, int w, int h, int bps);

// VRAM の絵を CPU のメモリへ読んで、普通の VFrame にする(interop が使えなくなった時の救済)
std::shared_ptr<const VFrame> toHost(const VFrame& f);

}  // namespace cuda
}  // namespace vn
