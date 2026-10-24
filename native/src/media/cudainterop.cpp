#include "media/cudainterop.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include "media/videostream.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace vn::cuda {

namespace {

using CUresult = int;
using CUcontext = void*;
using CUdeviceptr = unsigned long long;
using CUarray = void*;
using CUgraphicsResource = void*;
using CUstream = void*;

struct CUDA_MEMCPY2D {
    size_t srcXInBytes, srcY;
    int srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    int dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
};
constexpr int kMemHost = 1, kMemDevice = 2, kMemArray = 3;
constexpr unsigned kGL_TEXTURE_2D = 0x0DE1;
constexpr unsigned kRegisterWriteDiscard = 2;

struct Api {
    CUresult (*cuInit)(unsigned) = nullptr;
    CUresult (*cuCtxSetCurrent)(CUcontext) = nullptr;
    CUresult (*cuMemAlloc)(CUdeviceptr*, size_t) = nullptr;
    CUresult (*cuMemFree)(CUdeviceptr) = nullptr;
    CUresult (*cuMemcpy2D)(const CUDA_MEMCPY2D*) = nullptr;
    CUresult (*cuMemcpy2DAsync)(const CUDA_MEMCPY2D*, CUstream) = nullptr;
    CUresult (*cuStreamCreate)(CUstream*, unsigned) = nullptr;
    CUresult (*cuStreamSynchronize)(CUstream) = nullptr;
    CUresult (*cuGraphicsGLRegisterImage)(CUgraphicsResource*, unsigned, unsigned, unsigned) = nullptr;
    CUresult (*cuGraphicsUnregisterResource)(CUgraphicsResource) = nullptr;
    CUresult (*cuGraphicsMapResources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
    CUresult (*cuGraphicsUnmapResources)(unsigned, CUgraphicsResource*, CUstream) = nullptr;
    CUresult (*cuGraphicsSubResourceGetMappedArray)(CUarray*, CUgraphicsResource, unsigned, unsigned) = nullptr;
    bool ok = false;
};

Api g_api;
std::atomic<bool> g_disabled{false};
std::atomic<void*> g_ctx{nullptr};

bool loadApi() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* e = std::getenv("VN_NO_INTEROP"); e && *e && *e != '0') return;
#ifdef _WIN32
        HMODULE h = LoadLibraryA("nvcuda.dll");
        if (!h) return;
        auto get = [&](const char* n) { return (void*)GetProcAddress(h, n); };
#define L(f, name) g_api.f = (decltype(g_api.f))get(name); if (!g_api.f) return;
        L(cuInit, "cuInit")
        L(cuCtxSetCurrent, "cuCtxSetCurrent")
        L(cuMemAlloc, "cuMemAlloc_v2")
        L(cuMemFree, "cuMemFree_v2")
        L(cuMemcpy2D, "cuMemcpy2D_v2")
        L(cuMemcpy2DAsync, "cuMemcpy2DAsync_v2")
        L(cuStreamCreate, "cuStreamCreate")
        L(cuStreamSynchronize, "cuStreamSynchronize")
        L(cuGraphicsGLRegisterImage, "cuGraphicsGLRegisterImage")
        L(cuGraphicsUnregisterResource, "cuGraphicsUnregisterResource")
        L(cuGraphicsMapResources, "cuGraphicsMapResources")
        L(cuGraphicsUnmapResources, "cuGraphicsUnmapResources")
        L(cuGraphicsSubResourceGetMappedArray, "cuGraphicsSubResourceGetMappedArray")
#undef L
        g_api.ok = g_api.cuInit(0) == 0;
#endif
    });
    return g_api.ok;
}

// VRAM の置き場(同じ大きさを使い回す。cuMemAlloc は同期が入って重いので)
std::mutex g_poolMu;
std::vector<DevBuf> g_pool;
constexpr size_t kPoolKeep = 96;

DevBuf* allocBuf(size_t size) {
    {
        std::lock_guard<std::mutex> l(g_poolMu);
        for (size_t i = 0; i < g_pool.size(); ++i)
            if (g_pool[i].size == size) {
                DevBuf* b = new DevBuf(g_pool[i]);
                g_pool.erase(g_pool.begin() + i);
                return b;
            }
    }
    CUdeviceptr p = 0;
    if (g_api.cuMemAlloc(&p, size) != 0) return nullptr;
    DevBuf* b = new DevBuf;
    b->ptr = p;
    b->size = size;
    return b;
}

void releaseBuf(DevBuf* b) {
    DevBuf keep = *b;
    delete b;
    std::lock_guard<std::mutex> l(g_poolMu);
    if (g_pool.size() < kPoolKeep) { g_pool.push_back(keep); return; }
    if (g_api.cuMemFree) g_api.cuMemFree(keep.ptr);   // 呼んだスレッドの context で解放(同じデバイスの共有 context)
}

}  // namespace

bool enabled() { return !g_disabled && loadApi() && g_ctx.load() != nullptr; }
void disable() { g_disabled = true; }
void setContext(void* ctx) { g_ctx = ctx; }

void bindThread() {
    static thread_local void* bound = nullptr;
    void* c = g_ctx.load();
    if (c && c != bound && loadApi()) {
        g_api.cuCtxSetCurrent(c);
        bound = c;
    }
}

std::shared_ptr<DevBuf> copyFromDecoder(unsigned long long y, int yPitch, unsigned long long uv, int uvPitch, int w, int h, int bps) {
    if (!enabled()) return nullptr;
    bindThread();
    const size_t pitch = (size_t)w * bps;
    const size_t cw = (size_t)((w + 1) / 2) * 2 * bps, ch = (size_t)(h + 1) / 2;
    const size_t uvOff = pitch * h;
    DevBuf* raw = allocBuf(uvOff + cw * ch);
    if (!raw) return nullptr;
    raw->pitch = pitch;
    raw->uvOffset = uvOff;
    std::shared_ptr<DevBuf> buf(raw, releaseBuf);
    CUDA_MEMCPY2D m;
    std::memset(&m, 0, sizeof m);
    m.srcMemoryType = kMemDevice;
    m.dstMemoryType = kMemDevice;
    m.srcDevice = y;
    m.srcPitch = (size_t)yPitch;
    m.dstDevice = raw->ptr;
    m.dstPitch = pitch;
    m.WidthInBytes = pitch;
    m.Height = (size_t)h;
    if (g_api.cuMemcpy2D(&m) != 0) return nullptr;
    m.srcDevice = uv;
    m.srcPitch = (size_t)uvPitch;
    m.dstDevice = raw->ptr + uvOff;
    m.dstPitch = cw;
    m.WidthInBytes = cw;
    m.Height = ch;
    if (g_api.cuMemcpy2D(&m) != 0) return nullptr;
    return buf;
}

bool registerTextures(GlTarget& t, unsigned texY, unsigned texUV) {
    if (!enabled()) return false;
    bindThread();
    unregisterTextures(t);
    if (g_api.cuGraphicsGLRegisterImage(&t.resY, texY, kGL_TEXTURE_2D, kRegisterWriteDiscard) != 0) { t.resY = nullptr; return false; }
    if (g_api.cuGraphicsGLRegisterImage(&t.resUV, texUV, kGL_TEXTURE_2D, kRegisterWriteDiscard) != 0) {
        g_api.cuGraphicsUnregisterResource(t.resY);
        t.resY = t.resUV = nullptr;
        return false;
    }
    return true;
}

void unregisterTextures(GlTarget& t) {
    if (!g_api.ok) { t.resY = t.resUV = nullptr; return; }
    bindThread();
    if (t.resY) g_api.cuGraphicsUnregisterResource(t.resY);
    if (t.resUV) g_api.cuGraphicsUnregisterResource(t.resUV);
    t.resY = t.resUV = nullptr;
}

bool copyToTextures(const GlTarget& t, const DevBuf& b, int w, int h, int bps) {
    if (!enabled() || !t.resY || !t.resUV) return false;
    bindThread();
    // 既定ストリームだと他スレッドの暗黙同期に巻き込まれて数十ms詰まるので、非ブロッキング専用ストリームを使う
    static thread_local CUstream st = nullptr;
    static thread_local bool stTried = false;
    if (!stTried) { stTried = true; if (g_api.cuStreamCreate(&st, 1) != 0) st = nullptr; }
    CUgraphicsResource res[2] = {t.resY, t.resUV};
    if (g_api.cuGraphicsMapResources(2, res, st) != 0) return false;
    bool ok = true;
    CUarray arr[2] = {nullptr, nullptr};
    if (g_api.cuGraphicsSubResourceGetMappedArray(&arr[0], res[0], 0, 0) != 0 ||
        g_api.cuGraphicsSubResourceGetMappedArray(&arr[1], res[1], 0, 0) != 0)
        ok = false;
    if (ok) {
        const size_t cw = (size_t)((w + 1) / 2) * 2 * bps, ch = (size_t)(h + 1) / 2;
        CUDA_MEMCPY2D m;
        std::memset(&m, 0, sizeof m);
        m.srcMemoryType = kMemDevice;
        m.dstMemoryType = kMemArray;
        m.srcDevice = b.ptr;
        m.srcPitch = b.pitch;
        m.dstArray = arr[0];
        m.WidthInBytes = (size_t)w * bps;
        m.Height = (size_t)h;
        ok = (st ? g_api.cuMemcpy2DAsync(&m, st) : g_api.cuMemcpy2D(&m)) == 0;
        if (ok) {
            m.srcDevice = b.ptr + b.uvOffset;
            m.srcPitch = cw;
            m.dstArray = arr[1];
            m.WidthInBytes = cw;
            m.Height = ch;
            ok = (st ? g_api.cuMemcpy2DAsync(&m, st) : g_api.cuMemcpy2D(&m)) == 0;
        }
    }
    if (st) g_api.cuStreamSynchronize(st);
    g_api.cuGraphicsUnmapResources(2, res, st);
    return ok;
}

std::shared_ptr<const VFrame> toHost(const VFrame& f) {
    if (!f.dev || !g_api.ok) return nullptr;
    bindThread();
    auto v = std::make_shared<VFrame>();
    v->kind = f.kind; v->w = f.w; v->h = f.h; v->cw = f.cw; v->ch = f.ch;
    v->pts = f.pts; v->cs = f.cs; v->full = f.full; v->id = f.id;
    const int bps = f.kind == VFrame::P010 ? 2 : 1;
    v->y.reset(new uint8_t[(size_t)f.w * f.h * bps]);
    v->uv.reset(new uint8_t[(size_t)f.cw * 2 * bps * f.ch]);
    CUDA_MEMCPY2D m;
    std::memset(&m, 0, sizeof m);
    m.srcMemoryType = kMemDevice;
    m.dstMemoryType = kMemHost;
    m.srcDevice = f.dev->ptr;
    m.srcPitch = f.dev->pitch;
    m.dstHost = v->y.get();
    m.dstPitch = (size_t)f.w * bps;
    m.WidthInBytes = m.dstPitch;
    m.Height = (size_t)f.h;
    if (g_api.cuMemcpy2D(&m) != 0) return nullptr;
    m.srcDevice = f.dev->ptr + f.dev->uvOffset;
    m.srcPitch = (size_t)f.cw * 2 * bps;
    m.dstHost = v->uv.get();
    m.dstPitch = m.srcPitch;
    m.WidthInBytes = m.srcPitch;
    m.Height = (size_t)f.ch;
    if (g_api.cuMemcpy2D(&m) != 0) return nullptr;
    return v;
}

}  // namespace vn::cuda
