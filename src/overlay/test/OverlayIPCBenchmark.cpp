// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "lib/catch.hpp"
#include "overlay/IPC.h"
#include "overlay/OverlayIPC.h"
#include "util/Logging.h"
#include "util/TmpDir.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace stellar;

namespace
{

std::string
findOverlayBinary()
{
    std::vector<std::string> paths = {
        "target/release/stellar-overlay",
        "../target/release/stellar-overlay",
    };

    for (auto const& p : paths)
    {
        if (access(p.c_str(), X_OK) == 0)
        {
            return std::filesystem::absolute(p).string();
        }
    }
    return "";
}

struct BenchmarkResult
{
    size_t payloadSize;
    int iterations;
    double totalTimeMs;
    double avgLatencyMs;
    double throughputMBps;
    double minLatencyMs;
    double maxLatencyMs;
};

BenchmarkResult
benchmarkPayloadSize(OverlayIPC& ipc, size_t payloadSize, int iterations)
{
    BenchmarkResult result;
    result.payloadSize = payloadSize;
    result.iterations = iterations;

    std::vector<double> latencies;
    latencies.reserve(iterations);

    auto startTotal = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; i++)
    {
        auto start = std::chrono::high_resolution_clock::now();

        // Just call getTopTransactions to measure IPC latency
        // Payload size doesn't matter much here - mainly testing IPC overhead
        auto txs = ipc.getTopTransactions(payloadSize / 300, 1000);

        auto end = std::chrono::high_resolution_clock::now();
        double latencyMs =
            std::chrono::duration<double, std::milli>(end - start).count();
        latencies.push_back(latencyMs);
    }

    auto endTotal = std::chrono::high_resolution_clock::now();
    result.totalTimeMs =
        std::chrono::duration<double, std::milli>(endTotal - startTotal)
            .count();

    // Calculate stats
    double sum = 0;
    result.minLatencyMs = latencies[0];
    result.maxLatencyMs = latencies[0];

    for (double lat : latencies)
    {
        sum += lat;
        if (lat < result.minLatencyMs)
            result.minLatencyMs = lat;
        if (lat > result.maxLatencyMs)
            result.maxLatencyMs = lat;
    }

    result.avgLatencyMs = sum / iterations;

    // Calculate throughput (requests/sec)
    double totalSeconds = result.totalTimeMs / 1000.0;
    result.throughputMBps = iterations / totalSeconds;

    return result;
}

} // namespace

/**
 * Benchmark IPC performance with different payload sizes.
 *
 * This test measures the latency and throughput of the IPC channel
 * for various payload sizes to identify bottlenecks.
 *
 * Tagged with [.] and [benchmark] so it doesn't run by default.
 * Run with: stellar-core test '[ipc-benchmark]'
 */
TEST_CASE("IPC payload size benchmark", "[overlay-ipc-rust][.][benchmark]")
{
    std::string overlayBinary = findOverlayBinary();
    if (overlayBinary.empty())
    {
        FAIL("Skipping test - overlay binary not found");
        return;
    }

    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");
    CLOG_INFO(Overlay, "           IPC PAYLOAD SIZE BENCHMARK");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");
    CLOG_INFO(Overlay, "");

    TmpDir tmpDir("ipc-benchmark");
    std::string socketPath = tmpDir.getName() + "/overlay.sock";

    // Start the Rust overlay process
    OverlayIPC ipc(socketPath, overlayBinary, 11625);
    ipc.start();

    // Wait for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(ipc.isConnected());

    CLOG_INFO(Overlay, "IPC connected, starting benchmarks...");
    CLOG_INFO(Overlay, "");

    // Test different request counts
    struct TestCase
    {
        size_t size;
        int iterations;
        std::string label;
    };

    std::vector<TestCase> testCases = {
        {1, 1000, "1 TX request"},     {10, 500, "10 TX request"},
        {100, 100, "100 TX request"},  {1000, 50, "1000 TX request"},
        {5000, 20, "5000 TX request"}, {10000, 10, "10000 TX request"},
    };

    std::vector<BenchmarkResult> results;

    for (auto const& tc : testCases)
    {
        CLOG_INFO(Overlay, "Benchmarking {} ({} iterations)...", tc.label,
                  tc.iterations);

        auto result = benchmarkPayloadSize(ipc, tc.size, tc.iterations);
        results.push_back(result);

        CLOG_INFO(Overlay, "  Avg latency: {:.3f} ms", result.avgLatencyMs);
        CLOG_INFO(Overlay, "  Throughput: {:.2f} MB/s", result.throughputMBps);
        CLOG_INFO(Overlay, "  Min/Max: {:.3f} / {:.3f} ms", result.minLatencyMs,
                  result.maxLatencyMs);
        CLOG_INFO(Overlay, "");
    }

    // Print summary table
    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");
    CLOG_INFO(Overlay, "                       SUMMARY");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");
    CLOG_INFO(Overlay, "");

    CLOG_INFO(Overlay, "{:<12} {:>10} {:>12} {:>12} {:>12} {:>12}", "Request",
              "Iterations", "Avg (ms)", "Min (ms)", "Max (ms)", "Throughput");
    CLOG_INFO(Overlay, "{:<12} {:>10} {:>12} {:>12} {:>12} {:>12}", "Size", "",
              "", "", "", "(req/s)");
    CLOG_INFO(Overlay, "--------------------------------------------"
                       "----------------------------------------");

    for (size_t i = 0; i < results.size(); i++)
    {
        auto const& r = results[i];
        CLOG_INFO(Overlay,
                  "{:<12} {:>10} {:>12.3f} {:>12.3f} {:>12.3f} {:>12.0f}",
                  testCases[i].label, r.iterations, r.avgLatencyMs,
                  r.minLatencyMs, r.maxLatencyMs, r.throughputMBps);
    }

    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");

    // Analysis: Check for performance cliffs
    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "Performance Analysis:");
    for (size_t i = 1; i < results.size(); i++)
    {
        double sizeRatio = static_cast<double>(results[i].payloadSize) /
                           results[i - 1].payloadSize;
        double latencyRatio =
            results[i].avgLatencyMs / results[i - 1].avgLatencyMs;

        if (latencyRatio > sizeRatio * 2)
        {
            CLOG_WARNING(Overlay,
                         "  Performance cliff at {}: latency increased {}x "
                         "while size increased {}x",
                         testCases[i].label, latencyRatio, sizeRatio);
        }
        else if (latencyRatio < sizeRatio * 0.5)
        {
            CLOG_INFO(Overlay,
                      "  Good scaling at {}: latency increased {}x while size "
                      "increased {}x",
                      testCases[i].label, latencyRatio, sizeRatio);
        }
    }

    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "Benchmark complete!");

    ipc.shutdown();
}

/**
 * Benchmark concurrent IPC calls to measure contention.
 *
 * This test sends multiple requests in parallel to measure serialized IPC
 * throughput. IPC calls are serialized with a mutex since the channel is
 * not thread-safe (concurrent writes corrupt messages).
 */
TEST_CASE("IPC concurrent access benchmark", "[overlay-ipc-rust][.][benchmark]")
{
    std::string overlayBinary = findOverlayBinary();
    if (overlayBinary.empty())
    {
        FAIL("Skipping test - overlay binary not found");
        return;
    }

    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");
    CLOG_INFO(Overlay, "        IPC CONCURRENT ACCESS BENCHMARK");
    CLOG_INFO(Overlay, "============================================"
                       "========================================");

    TmpDir tmpDir("ipc-benchmark");
    std::string socketPath = tmpDir.getName() + "/overlay.sock";

    OverlayIPC ipc(socketPath, overlayBinary, 11625);
    ipc.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(ipc.isConnected());

    // Test concurrent getTopTransactions calls
    size_t const numThreads = 4;
    size_t const callsPerThread = 100;
    size_t const payloadSize = 1024; // 1KB TXs

    CLOG_INFO(Overlay, "Testing {} threads, {} calls each, {} byte payloads",
              numThreads, callsPerThread, payloadSize);

    auto startTime = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    std::vector<double> threadTimes(numThreads);
    std::mutex ipcMutex; // Serialize IPC access (channel not thread-safe)

    for (size_t t = 0; t < numThreads; t++)
    {
        threads.emplace_back([&, t]() {
            auto threadStart = std::chrono::high_resolution_clock::now();

            for (size_t i = 0; i < callsPerThread; i++)
            {
                std::lock_guard<std::mutex> lock(ipcMutex);
                auto txs = ipc.getTopTransactions(10, 5000);
                // Just query, don't validate results
            }

            auto threadEnd = std::chrono::high_resolution_clock::now();
            threadTimes[t] = std::chrono::duration<double, std::milli>(
                                 threadEnd - threadStart)
                                 .count();
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double totalTimeMs =
        std::chrono::duration<double, std::milli>(endTime - startTime).count();

    CLOG_INFO(Overlay, "");
    CLOG_INFO(Overlay, "Results:");
    CLOG_INFO(Overlay, "  Total wall time: {:.2f} ms", totalTimeMs);
    CLOG_INFO(Overlay, "  Total calls: {}", numThreads * callsPerThread);
    CLOG_INFO(Overlay, "  Calls/sec: {:.0f}",
              (numThreads * callsPerThread) / (totalTimeMs / 1000.0));

    for (size_t i = 0; i < numThreads; i++)
    {
        CLOG_INFO(Overlay, "  Thread {} time: {:.2f} ms", i, threadTimes[i]);
    }

    ipc.shutdown();
}
