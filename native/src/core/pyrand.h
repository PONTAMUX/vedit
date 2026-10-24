// Python の random.Random(整数の seed) と同じ数列を出す MT19937(fx.py / expr.py の乱数を同じ結果にするため)
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace vn {

class PyRandom {
public:
    explicit PyRandom(long long seed) {
        unsigned long long n = seed < 0 ? (unsigned long long)(-seed) : (unsigned long long)seed;   // Python は abs(seed)
        std::vector<uint32_t> key;
        while (n) { key.push_back((uint32_t)(n & 0xFFFFFFFFu)); n >>= 32; }
        if (key.empty()) key.push_back(0);
        initByArray(key);
    }
    uint32_t u32() {
        if (idx_ >= N) twist();
        uint32_t y = mt_[idx_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }
    double random() {
        const uint32_t a = u32() >> 5, b = u32() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }
    double uniform(double a, double b) { return a + (b - a) * random(); }
    uint32_t getrandbits(int k) { return k <= 0 ? 0 : (u32() >> (32 - k)); }   // k <= 32
    uint32_t randbelow(uint32_t n) {
        if (n == 0) return 0;
        int k = 0;
        for (uint32_t t = n; t; t >>= 1) ++k;
        uint32_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }
    long long randint(long long a, long long b) { return a + (long long)randbelow((uint32_t)(b - a + 1)); }
    template <class T> void shuffle(std::vector<T>& v) {
        for (size_t i = v.size(); i-- > 1;) {
            const size_t j = randbelow((uint32_t)(i + 1));
            std::swap(v[i], v[j]);
        }
    }

private:
    static constexpr int N = 624, M = 397;
    uint32_t mt_[N];
    int idx_ = N;
    void initGenrand(uint32_t s) {
        mt_[0] = s;
        for (int i = 1; i < N; ++i) mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + (uint32_t)i;
        idx_ = N;
    }
    void initByArray(const std::vector<uint32_t>& key) {
        initGenrand(19650218u);
        int i = 1, j = 0;
        const int len = (int)key.size();
        for (int k = N > len ? N : len; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
            ++i; ++j;
            if (i >= N) { mt_[0] = mt_[N - 1]; i = 1; }
            if (j >= len) j = 0;
        }
        for (int k = N - 1; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
            ++i;
            if (i >= N) { mt_[0] = mt_[N - 1]; i = 1; }
        }
        mt_[0] = 0x80000000u;
        idx_ = N;
    }
    void twist() {
        for (int k = 0; k < N; ++k) {
            const uint32_t y = (mt_[k] & 0x80000000u) | (mt_[(k + 1) % N] & 0x7fffffffu);
            mt_[k] = mt_[(k + M) % N] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
        }
        idx_ = 0;
    }
};

}  // namespace vn
