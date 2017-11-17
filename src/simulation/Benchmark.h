#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "medida/reporting/json_reporter.h"
#include "medida/timer.h"
#include "simulation/LoadGenerator.h"
#include "util/Timer.h"
#include <chrono>
#include <memory>
#include <vector>

namespace stellar
{

class TxSampler;

class Benchmark
{
  public:
    static size_t MAXIMAL_NUMBER_OF_TXS_PER_LEDGER;
    static const char* LOGGER_ID;

    class BenchmarkBuilder;

    struct Metrics
    {
        medida::Timer& benchmarkTimer;
        medida::Counter& txsCount;

      private:
        Metrics(medida::MetricsRegistry& registry);
        friend class Benchmark;
    };

    ~Benchmark();
    void startBenchmark(Application& app);
    Metrics stopBenchmark();
    Metrics getMetrics();

  protected:
    Benchmark(medida::MetricsRegistry& registry, uint32_t txRate,
              std::unique_ptr<TxSampler> sampler);

  private:
    bool generateLoadForBenchmark(Application& app, uint32_t txRate,
                                  Metrics& metrics);
    void scheduleLoad(Application& app, std::function<bool()> loadGenerator,
                      std::chrono::milliseconds stepTime);
    Benchmark::Metrics initializeMetrics(medida::MetricsRegistry& registry);
    VirtualTimer& getTimer(VirtualClock& clock);

    bool mIsRunning;
    uint32_t mTxRate;
    Benchmark::Metrics mMetrics;
    std::unique_ptr<medida::TimerContext> mBenchmarkTimeContext;
    std::unique_ptr<VirtualTimer> mLoadTimer;
    std::unique_ptr<TxSampler> mSampler;
};

class ShuffleLoadGenerator;

class Benchmark::BenchmarkBuilder
{
  public:
    BenchmarkBuilder(Hash const& networkID);
    BenchmarkBuilder& setNumberOfInitialAccounts(uint32_t accounts);
    BenchmarkBuilder& setTxRate(uint32_t txRate);
    BenchmarkBuilder& initializeBenchmark();
    BenchmarkBuilder& populateBenchmarkData();
    std::unique_ptr<Benchmark> createBenchmark(Application& app) const;

  private:
    void prepareBenchmark(Application& app,
                          ShuffleLoadGenerator& sampler) const;
    void populateAccounts(Application& app, size_t n,
                          ShuffleLoadGenerator& sampler) const;
    void setMaxTxSize(LedgerManager& ledger, uint32_t maxTxSetSize) const;
    LedgerCloseData createData(LedgerManager& ledger,
                               StellarValue& value) const;
    void createAccountsDirectly(
        Application& app,
        std::vector<LoadGenerator::AccountInfoPtr>& accounts) const;

    bool mInitialize;
    bool mPopulate;
    uint32_t mTxRate;
    uint32_t mAccounts;
    Hash mNetworkID;
};

class TxSampler
{
  public:
    class Tx
    {
      public:
        virtual ~Tx() = default;
        virtual bool execute(Application& app) = 0;
    };

    virtual ~TxSampler() = default;
    virtual std::unique_ptr<Tx> createTransaction() = 0;
};

class ShuffleLoadGenerator : public LoadGenerator, public TxSampler
{
  public:
    ShuffleLoadGenerator(Hash const& networkID);
    virtual ~ShuffleLoadGenerator() = default;
    virtual std::unique_ptr<Tx> createTransaction() override;
    std::vector<LoadGenerator::AccountInfoPtr> createAccounts(size_t batchSize);
    void initialize(Application& app, size_t numberOfAccounts);

  protected:
    virtual LoadGenerator::AccountInfoPtr
    pickRandomAccount(LoadGenerator::AccountInfoPtr tryToAvoid,
                      uint32_t ledgerNum) override;
    std::vector<LoadGenerator::AccountInfoPtr>::iterator
    shuffleAccounts(std::vector<LoadGenerator::AccountInfoPtr>& accounts);

    std::vector<LoadGenerator::AccountInfoPtr>::iterator mRandomIterator;
};

class BenchmarkExecutor
{
  public:
    void executeBenchmark(Application& app,
                          Benchmark::BenchmarkBuilder& benchmarkBuilder,
                          std::chrono::seconds testDuration,
                          std::function<void(Benchmark::Metrics)> stopCallback);

  private:
    VirtualTimer& getTimer(VirtualClock& clock);

    std::unique_ptr<VirtualTimer> mLoadTimer;
};

struct BenchmarkReporter
{
    template <typename Stream>
    void
    reportBenchmark(Benchmark::Metrics const& metrics,
                    medida::MetricsRegistry& metricsRegistry, Stream& str)
    {
        using namespace std;
        class ReportProcessor : public medida::MetricProcessor
        {
          public:
            virtual ~ReportProcessor() = default;
            virtual void
            Process(medida::Timer& timer)
            {
                count = timer.count();
            }

            std::uint64_t count;
        };
        auto externalizedTxs =
            metricsRegistry.GetAllMetrics()[{"ledger", "transaction", "apply"}];
        ReportProcessor processor;
        externalizedTxs->Process(processor);
        auto txsExternalized = processor.count;

        str << endl
            << "Benchmark metrics:" << endl
            << "  time spent: " << metrics.benchmarkTimer.sum()
            << " milliseconds" << endl
            << "  txs submitted: " << metrics.txsCount.count() << endl
            << "  txs externalized: " << txsExternalized << endl;

        medida::reporting::JsonReporter jr(metricsRegistry);
        str << jr.Report() << endl;
    }
};
}
