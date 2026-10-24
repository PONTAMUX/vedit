#pragma once
// 使えるメモリ(設定の「使えるメモリ」)。絵のキャッシュ(差し込み画像のテクスチャ・字幕の絵)はこの範囲に収める
#include <algorithm>
#include <atomic>
namespace vn {
inline std::atomic<long long> g_memBudget{2048LL << 20};
inline long long memBudget() { return g_memBudget.load(std::memory_order_relaxed); }
inline void setMemBudgetMB(int mb) { g_memBudget = (long long)std::max(256, mb) << 20; }
}
