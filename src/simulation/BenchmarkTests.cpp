// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/Benchmark.h"
#include "history/HistoryArchive.h"
#include "lib/catch.hpp"
#include "medida/metric_processor.h"
#include "medida/reporting/json_reporter.h"
#include "simulation/BenchmarkExecutor.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include <memory>

using namespace stellar;

const char* LOGGER_ID = "Benchmark";

std::unique_ptr<Benchmark>
initializeBenchmark(Application& app)
{
    auto benchmark = make_unique<Benchmark>(app.getNetworkID());
    benchmark->initializeBenchmark(app,
                                   app.getLedgerManager().getLedgerNum() - 1);
    return benchmark;
}

void
prepareBenchmark(Application& app)
{
    auto benchmark = make_unique<Benchmark>(app.getNetworkID());
    benchmark->prepareBenchmark(app);
}

std::unique_ptr<Config>
initializeConfig()
{
    std::unique_ptr<Config> cfg = make_unique<Config>(getTestConfig());
    cfg->DATABASE = SecretValue{"postgresql://dbname=core user=stellar "
                                "password=__PGPASS__ host=localhost"};
    cfg->PUBLIC_HTTP_PORT = true;
    cfg->COMMANDS.push_back("ll?level=info");
    cfg->DESIRED_MAX_TX_PER_LEDGER =
        Benchmark::MAXIMAL_NUMBER_OF_TXS_PER_LEDGER;
    cfg->FORCE_SCP = true;
    cfg->RUN_STANDALONE = false;
    cfg->BUCKET_DIR_PATH = "buckets";

    using namespace std;
    const string historyName = "benchmark";
    const string historyGetCmd = "cp history/vs/{0} {1}";
    const string historyPutCmd = "cp {0} history/vs/{1}";
    const string historyMkdirCmd = "mkdir -p history/vs/{0}";
    cfg->HISTORY[historyName] = make_shared<HistoryArchive>(
        historyName, historyGetCmd, historyPutCmd, historyMkdirCmd);

    return cfg;
}

TEST_CASE("stellar-core benchmark's initialization", "[benchmark][initialize]")
{
    std::unique_ptr<Config> cfg = initializeConfig();
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer app = Application::create(clock, *cfg, false);
    app->applyCfgCommands();

    prepareBenchmark(*app);
}

TEST_CASE("stellar-core's benchmark", "[benchmark]")
{
    auto testDuration = std::chrono::seconds(60 * 10);

    VirtualClock clock(VirtualClock::REAL_TIME);
    std::unique_ptr<Config> cfg = initializeConfig();

    Application::pointer app = Application::create(clock, *cfg, false);
    app->applyCfgCommands();
    app->start();
    auto benchmark = initializeBenchmark(*app);
    bool done = false;

    VirtualTimer timer{clock};
    auto metrics = benchmark->startBenchmark(*app);
    timer.expires_from_now(testDuration);
    timer.async_wait(
        [&benchmark, &done, app, metrics](asio::error_code const& error) {
            auto stopMetrics = benchmark->stopBenchmark(metrics);
            BenchmarkExecutor().reportBenchmark(*metrics, *app);
            done = true;
        });

    while (!done)
    {
        clock.crank();
    }
    app->gracefulStop();

    CLOG(INFO, LOGGER_ID) << "Benchmark complete.";
    app->getMetrics().NewMeter({"benchmark", "run", "complete"}, "run").Mark();
}
