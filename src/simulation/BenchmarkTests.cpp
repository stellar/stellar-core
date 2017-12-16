// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/Benchmark.h"
#include "lib/catch.hpp"
#include "test/test.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include <chrono>
#include <memory>

using namespace stellar;

TEST_CASE("stellar-core benchmark's initialization",
          "[benchmark][initialize][hide]")
{
    const size_t nAccounts = 1000;
    Config const& cfg = getTestConfig();
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app = Application::create(clock, cfg, false);
    app->applyCfgCommands();
    app->start();

    Benchmark::BenchmarkBuilder builder{app->getNetworkID()};
    builder.setNumberOfInitialAccounts(nAccounts).populateBenchmarkData();
    std::unique_ptr<TxSampler> sampler = builder.createSampler(*app);
    auto tx = sampler->createTransaction(nAccounts);
    REQUIRE(tx);
    REQUIRE(tx->execute(*app));
}

TEST_CASE("stellar-core's benchmark", "[benchmark][execute][hide]")
{
    const std::chrono::seconds testDuration(1);
    const size_t nAccounts = 1000;
    const uint32_t txRate = 100;
    const uint32_t txPerLedger = 1000;

    Config cfg = getTestConfig();
    cfg.DESIRED_MAX_TX_PER_LEDGER = txPerLedger;
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app = Application::create(clock, cfg, false);
    app->applyCfgCommands();
    app->start();

    Benchmark::BenchmarkBuilder builder{app->getNetworkID()};
    builder.setNumberOfInitialAccounts(nAccounts).populateBenchmarkData();
    bool done = false;
    BenchmarkExecutor executor;
    executor.setBenchmark(builder.createBenchmark(*app));
    executor.executeBenchmark(
        *app, testDuration, txRate, [&done, app](Benchmark::Metrics metrics) {
            done = true;
            reportBenchmark(metrics, app->getMetrics(), LOG(INFO));
        });
    while (!done)
    {
        clock.crank();
    }
    app->gracefulStop();
}
