// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/ThreadScalingBench.h"
#include "main/Config.h"
#include "rust/RustBridge.h"
#include "util/Logging.h"
#include <chrono>
#include <cstdint>
#include <openssl/evp.h>
#include <thread>
#include <vector>

namespace stellar
{

namespace
{

constexpr uint64_t kIterationsPerThread = 5'000'000;
constexpr size_t kBufSize = 256;

inline uint64_t
nextRand(uint64_t& s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s;
}

// Cached SHA-256 EVP_MD handle, fetched once. Avoids the per-call implicit
// algorithm fetch (and its global lock) that the legacy one-shot SHA256() does.
EVP_MD*
sha256Md()
{
    static EVP_MD* md = EVP_MD_fetch(nullptr, "SHA256", nullptr);
    return md;
}

// Work unit using OpenSSL cached EVP_MD with a reusable thread-local context.
uint64_t
doWorkEvp(std::vector<uint8_t>& buf, uint64_t& rng)
{
    for (size_t i = 0; i < buf.size(); ++i)
    {
        buf[i] = static_cast<uint8_t>(nextRand(rng) >> 33);
    }
    thread_local EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    unsigned char h[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_DigestInit_ex2(ctx, sha256Md(), nullptr);
    EVP_DigestUpdate(ctx, buf.data(), buf.size());
    EVP_DigestFinal_ex(ctx, h, &n);
    return static_cast<uint64_t>(h[0]) | (static_cast<uint64_t>(h[31]) << 8);
}

// Work unit using the Rust sha2 bridge (lock-free + SHA-NI), through cxx FFI.
uint64_t
doWorkRust(std::vector<uint8_t>& buf, uint64_t& rng)
{
    for (size_t i = 0; i < buf.size(); ++i)
    {
        buf[i] = static_cast<uint8_t>(nextRand(rng) >> 33);
    }
    unsigned char h[32];
    rust_bridge::sha256_rust(buf.data(), buf.size(), h);
    return static_cast<uint64_t>(h[0]) | (static_cast<uint64_t>(h[31]) << 8);
}

template <uint64_t (*WORK)(std::vector<uint8_t>&, uint64_t&)>
double
runThreads(unsigned numThreads)
{
    std::vector<uint64_t> sink(numThreads, 0);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    auto t0 = std::chrono::steady_clock::now();
    for (unsigned t = 0; t < numThreads; ++t)
    {
        threads.emplace_back([&sink, t]() {
            uint64_t rng = 0x9e3779b97f4a7c15ULL + t * 0x100000001b3ULL;
            uint64_t acc = 0;
            std::vector<uint8_t> buf(kBufSize);
            for (uint64_t j = 0; j < kIterationsPerThread; ++j)
            {
                acc ^= WORK(buf, rng);
            }
            sink[t] = acc;
        });
    }
    for (auto& th : threads)
    {
        th.join();
    }
    double wall =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    uint64_t s = 0;
    for (auto v : sink)
    {
        s ^= v;
    }
    volatile uint64_t keep = s;
    (void)keep;
    return wall;
}

template <uint64_t (*WORK)(std::vector<uint8_t>&, uint64_t&)>
void
sweep(char const* name, std::vector<unsigned> const& counts)
{
    CLOG_WARNING(Perf, "== {} ==", name);
    CLOG_WARNING(Perf, "{:>8s} {:>10s} {:>9s} {:>7s}", "threads", "wall_ms",
                 "speedup", "eff%");
    double base = 0.0;
    for (unsigned n : counts)
    {
        double w = runThreads<WORK>(n);
        if (n == counts.front())
        {
            base = w;
        }
        double spd = w > 0 ? base / w * n : 0.0;
        CLOG_WARNING(Perf, "{:>8d} {:>10.1f} {:>9.2f} {:>7.1f}", n, w * 1000.0,
                     spd, w > 0 ? base / w / n * 100.0 : 0.0);
    }
}

} // namespace

void
runThreadScalingBench(Config const& cfg)
{
    unsigned hc = std::thread::hardware_concurrency();
    if (hc == 0)
    {
        hc = 16;
    }
    std::vector<unsigned> counts;
    for (unsigned n = 1; n <= hc; n *= 2)
    {
        counts.push_back(n);
    }
    if (counts.empty() || counts.back() != hc)
    {
        counts.push_back(hc);
    }

    CLOG_WARNING(Perf, "===== SHA256 thread-scaling: OpenSSL cached EVP_MD vs "
                       "Rust sha2 bridge ({} iters/thread, {}B) =====",
                 kIterationsPerThread, kBufSize);
    sweep<doWorkEvp>("OpenSSL cached EVP_MD", counts);
    sweep<doWorkRust>("Rust sha2 bridge (via cxx FFI)", counts);
    CLOG_WARNING(Perf, "===== end SHA256 thread-scaling =====");
}

}
