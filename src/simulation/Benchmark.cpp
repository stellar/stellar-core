// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/Benchmark.h"

#include "bucket/BucketManager.h"
#include "database/Database.h"
#include "ledger/LedgerDelta.h"
#include "util/Logging.h"
#include "util/make_unique.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <vector>

namespace stellar
{

const uint32_t Benchmark::STEP_MSECS = 100;

Benchmark::Benchmark(medida::MetricsRegistry& registry, uint32_t txRate,
                     std::unique_ptr<TxSampler> sampler)
    : mIsRunning(false)
    , mMetrics(Benchmark::Metrics(registry))
    , mSampler(std::move(sampler))
{
    setTxRate(txRate);
}

Benchmark::~Benchmark()
{
    if (mIsRunning)
    {
        stopBenchmark();
    }
}

void
Benchmark::startBenchmark(Application& app)
{
    if (mIsRunning)
    {
        throw std::runtime_error{"Benchmark already started"};
    }
    mIsRunning = true;
    mBenchmarkTimeContext =
        make_unique<medida::TimerContext>(mMetrics.mBenchmarkTimer.TimeScope());
    scheduleLoad(app, std::chrono::milliseconds{STEP_MSECS});
}

Benchmark::Metrics::Metrics(medida::MetricsRegistry& registry)
    : mBenchmarkTimer(registry.NewTimer({"benchmark", "overall", "time"}))
    , mTxsCount(registry.NewCounter({"benchmark", "txs", "count"}))
{
}

Benchmark::Metrics
Benchmark::stopBenchmark()
{
    LOG(INFO) << "Stopping benchmark";
    if (!mIsRunning)
    {
        throw std::runtime_error{"Benchmark is already stopped"};
    }
    mBenchmarkTimeContext->Stop();
    mIsRunning = false;
    LOG(INFO) << "Benchmark stopping procedure finished";
    return mMetrics;
}

void
Benchmark::setTxRate(uint32_t txRate)
{
    mTxRate = txRate * STEP_MSECS / 1000;
}

bool
Benchmark::generateLoadForBenchmark(Application& app)
{
    LOG(TRACE) << "Generating " << mTxRate << " transaction(s)";

    mBenchmarkTimeContext->Stop();
    auto txs = mSampler->createTransaction(mTxRate);
    mBenchmarkTimeContext->Reset();
    if (!txs->execute(app))
    {
        LOG(ERROR) << "Error while executing a transaction: "
                      "transaction was rejected";
        return false;
    }
    mMetrics.mTxsCount.inc(mTxRate);

    LOG(TRACE) << mTxRate << " transaction(s) generated in a single step";

    return true;
}

void
Benchmark::scheduleLoad(Application& app, std::chrono::milliseconds stepTime)
{
    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(app.getClock());
    }
    mLoadTimer->expires_from_now(stepTime);
    mLoadTimer->async_wait(
        [this, &app, stepTime](asio::error_code const& error) {
            if (error)
            {
                return;
            }
            if (generateLoadForBenchmark(app))
            {
                this->scheduleLoad(app, stepTime);
            }
            else
            {
                stopBenchmark();
            }
        });
}

Benchmark::BenchmarkBuilder::BenchmarkBuilder(Hash const& networkID)
    : mPopulate(false)
    , mAlreadyPopulated(false)
    , mTxRate(0)
    , mNumberOfAccounts(0)
    , mNetworkID(networkID)
    , mLoadAccounts(false)
{
}

Benchmark::BenchmarkBuilder&
Benchmark::BenchmarkBuilder::setNumberOfInitialAccounts(uint32_t accounts)
{
    mNumberOfAccounts = accounts;
    return *this;
}

Benchmark::BenchmarkBuilder&
Benchmark::BenchmarkBuilder::setTxRate(uint32_t txRate)
{
    mTxRate = txRate;
    return *this;
}

Benchmark::BenchmarkBuilder&
Benchmark::BenchmarkBuilder::loadAccounts()
{
    mLoadAccounts = true;
    return *this;
}

Benchmark::BenchmarkBuilder&
Benchmark::BenchmarkBuilder::populateBenchmarkData()
{
    mPopulate = true;
    return *this;
}

void
createAccountsDirectly(
    Application& app,
    std::vector<LoadGenerator::AccountInfoPtr>::const_iterator createdStart,
    std::vector<LoadGenerator::AccountInfoPtr>::const_iterator createdEnd)
{
    soci::transaction sqlTx(app.getDatabase().getSession());

    auto ledger = app.getLedgerManager().getLedgerNum();
    int64_t balanceDiff = 0;
    std::vector<LedgerEntry> live;
    std::transform(
        createdStart, createdEnd, std::back_inserter(live),
        [&app, &balanceDiff](LoadGenerator::AccountInfoPtr const& account) {
            AccountFrame aFrame = account->createDirectly(app);
            balanceDiff += aFrame.getBalance();
            return aFrame.mEntry;
        });

    SecretKey skey = SecretKey::fromSeed(app.getNetworkID());
    AccountFrame::pointer masterAccount =
        AccountFrame::loadAccount(skey.getPublicKey(), app.getDatabase());
    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());
    masterAccount->addBalance(-balanceDiff);
    masterAccount->touch(ledger);
    masterAccount->storeChange(delta, app.getDatabase());

    sqlTx.commit();

    auto liveEntries = delta.getLiveEntries();
    live.insert(live.end(), liveEntries.begin(), liveEntries.end());
    app.getBucketManager().addBatch(app, ledger, live, {});
}

void
populateAccounts(
    Application& app,
    std::vector<LoadGenerator::AccountInfoPtr>::const_iterator createdStart,
    std::vector<LoadGenerator::AccountInfoPtr>::const_iterator createdEnd)
{
    std::vector<LoadGenerator::AccountInfoPtr>::const_iterator start =
        createdStart;
    std::vector<LoadGenerator::AccountInfoPtr>::const_iterator end = createdEnd;
    size_t accountsLeft = std::distance(createdStart, createdEnd);
    size_t batchSize = accountsLeft;
    for (; start != createdEnd; start = end, accountsLeft -= batchSize)
    {
        batchSize = std::min(accountsLeft, MAXIMAL_NUMBER_OF_ACCOUNTS_IN_BATCH);
        end = start + batchSize;

        auto ledgerNum = app.getLedgerManager().getLedgerNum();
        createAccountsDirectly(app, start, end);
    }
}

std::unique_ptr<Benchmark>
Benchmark::BenchmarkBuilder::createBenchmark(Application& app)
{
    std::unique_ptr<TxSampler> sampler = createSampler(app);
    return make_unique<Benchmark>(app.getMetrics(), mTxRate,
                                     std::move(sampler));
}

std::unique_ptr<TxSampler>
Benchmark::BenchmarkBuilder::createSampler(Application& app)
{
    auto sampler = make_unique<TxSampler>(mNetworkID);
    sampler->createAccounts(mNumberOfAccounts,
                            app.getLedgerManager().getLedgerNum());
    if (mPopulate && !mAlreadyPopulated)
    {
        // root account should be first on that list
        // omit the root account
        auto const& createdAccounts = sampler->getAccounts();
        populateAccounts(app, createdAccounts.cbegin() + 1,
                         createdAccounts.cend());
        mAlreadyPopulated = true;
    }
    if (mLoadAccounts)
    {
        sampler->loadAccounts(app);
    }
    sampler->initialize(app);

    return sampler;
}

TxSampler::TxSampler(Hash const& networkID) : LoadGenerator(networkID)
{
}

bool
TxSampler::Tx::execute(Application& app)
{
    for (auto& tx : mTxs)
    {
        if (!tx.execute(app))
        {
            return false;
        }
    }
    return true;
}

std::unique_ptr<TxSampler::Tx>
TxSampler::createTransaction(size_t size)
{
    auto result = make_unique<TxSampler::Tx>();
    for (size_t it = 0; it < size; ++it)
    {
        result->mTxs.push_back(LoadGenerator::createRandomTransaction(0.5));
    }
    return result;
}

std::vector<LoadGenerator::AccountInfoPtr>
TxSampler::createAccounts(size_t batchSize, uint32_t ledgerNum)
{
    return LoadGenerator::createAccounts(batchSize, ledgerNum);
}

std::vector<LoadGenerator::AccountInfoPtr> const&
TxSampler::getAccounts()
{
    return mAccounts;
}

void
TxSampler::initialize(Application& app)
{
    LOG(INFO) << "Initializing benchmark";

    mRandomIterator = shuffleAccounts(mAccounts);

    LOG(INFO) << "Benchmark initialized";
}

void
TxSampler::loadAccounts(Application& app)
{
    LoadGenerator::loadAccounts(app, mAccounts);
}

LoadGenerator::AccountInfoPtr
TxSampler::pickRandomAccount(AccountInfoPtr tryToAvoid, uint32_t ledgerNum)
{
    if (mRandomIterator == mAccounts.end())
    {
        mRandomIterator = mAccounts.begin();
    }
    auto result = *mRandomIterator;
    mRandomIterator++;
    return result;
}

std::vector<LoadGenerator::AccountInfoPtr>::iterator
TxSampler::shuffleAccounts(std::vector<LoadGenerator::AccountInfoPtr>& accounts)
{
    auto rng = std::default_random_engine{0};
    std::shuffle(mAccounts.begin(), mAccounts.end(), rng);
    return mAccounts.begin();
}

void
BenchmarkExecutor::executeBenchmark(
    Application& app, std::chrono::seconds testDuration, uint32_t txRate,
    std::function<void(Benchmark::Metrics)> stopCallback)
{
    if (!mBenchmark)
    {
        LOG(INFO)
            << "Benchmark was not initialized - benchmark's execution stopped";
        return;
    }
    mBenchmark->setTxRate(txRate);

    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(app.getClock());
    }
    mLoadTimer->expires_from_now(std::chrono::milliseconds{1});
    mLoadTimer->async_wait([this, &app, testDuration,
                            stopCallback](asio::error_code const& error) {

        mBenchmark->startBenchmark(app);

        auto stopProcedure = [this,
                              stopCallback](asio::error_code const& error) {

            auto metrics = mBenchmark->stopBenchmark();
            stopCallback(metrics);

            LOG(INFO) << "Benchmark complete";
        };

        mLoadTimer->expires_from_now(testDuration);
        mLoadTimer->async_wait(stopProcedure);
    });
}

void
BenchmarkExecutor::setBenchmark(std::unique_ptr<Benchmark> benchmark)
{
    mBenchmark = std::move(benchmark);
}
}
