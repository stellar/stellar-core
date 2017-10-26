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
#include <functional>
#include <memory>
#include <random>
#include <vector>

namespace stellar
{

const char* Benchmark::LOGGER_ID = "Benchmark";

size_t Benchmark::MAXIMAL_NUMBER_OF_TXS_PER_LEDGER = 1000;

Benchmark::Benchmark(Hash const& networkID)
    : Benchmark(networkID, 1000000, MAXIMAL_NUMBER_OF_TXS_PER_LEDGER)
{
}

Benchmark::Benchmark(Hash const& networkID, size_t numberOfInitialAccounts,
                     uint32_t txRate)
    : LoadGenerator(networkID)
    , mIsRunning(false)
    , mNumberOfInitialAccounts(numberOfInitialAccounts)
    , mTxRate(txRate)
{
}

std::shared_ptr<Benchmark::Metrics>
Benchmark::startBenchmark(Application& app)
{
    mIsRunning = true;
    using namespace std;
    shared_ptr<Benchmark::Metrics> metrics{initializeMetrics(app.getMetrics())};
    auto lastLedgerNum = app.getLedgerManager().getLedgerNum();
    lastLedgerNum = lastLedgerNum == 0 ? 0 : lastLedgerNum - 1;
    std::function<bool()> load = [this, &app, metrics,
                                  lastLedgerNum]() mutable {
        if (!this->mIsRunning)
        {
            return false;
        }
        if (lastLedgerNum < app.getLedgerManager().getLedgerNum())
        {
            generateLoadForBenchmark(app, this->mTxRate, *metrics);
            lastLedgerNum = app.getLedgerManager().getLedgerNum();
        }

        return true;
    };
    metrics->benchmarkTimeContext =
        make_unique<medida::TimerContext>(metrics->benchmarkTimer.TimeScope());
    load();
    scheduleLoad(app, load);

    return metrics;
}

std::unique_ptr<Benchmark::Metrics>
Benchmark::initializeMetrics(medida::MetricsRegistry& registry)
{
    return make_unique<Benchmark::Metrics>(Benchmark::Metrics(registry));
}

Benchmark::Metrics::Metrics(medida::MetricsRegistry& registry)
    : benchmarkTimer(registry.NewTimer({"benchmark", "overall", "time"}))
    , txsCount(registry.NewCounter({"benchmark", "txs", "count"}))
{
}

std::shared_ptr<Benchmark::Metrics>
Benchmark::stopBenchmark(std::shared_ptr<Benchmark::Metrics> metrics)
{
    mIsRunning = false;
    metrics->timeSpent = metrics->benchmarkTimeContext->Stop();
    return metrics;
}

bool
Benchmark::generateLoadForBenchmark(Application& app, uint32_t txRate,
                                    Metrics& metrics)
{
    updateMinBalance(app);

    if (txRate == 0)
    {
        txRate = 1;
    }

    CLOG(TRACE, LOGGER_ID) << "Generating " << txRate
                           << "transactions per step";

    uint32_t ledgerNum = app.getLedgerManager().getLedgerNum();
    std::vector<LoadGenerator::TxInfo> txs;
    for (uint32_t it = 0; it < txRate; ++it)
    {
        txs.push_back(createRandomTransaction(0.5, ledgerNum));
    }

    for (auto& tx : txs)
    {
        if (!tx.execute(app))
        {
            CLOG(ERROR, LOGGER_ID)
                << "Error while executing a transaction: transaction rejected";
            return false;
        }
    }

    metrics.txsCount.inc(txs.size());

    CLOG(TRACE, LOGGER_ID) << txRate
                           << " transactions generated in single step";

    return true;
}

void
Benchmark::createAccountsUsingLedgerManager(Application& app, size_t n)
{
    auto accountsLeft = mNumberOfInitialAccounts;
    TxMetrics txm(app.getMetrics());
    LedgerManager& ledger = app.getLedgerManager();
    auto ledgerNum = ledger.getLedgerNum();
    StellarValue value(ledger.getLastClosedLedgerHeader().hash, ledgerNum,
                       emptyUpgradeSteps, 0);

    while (accountsLeft > 0)
    {
        auto ledgerNum = ledger.getLedgerNum();
        TxSetFramePtr txSet = std::make_shared<TxSetFrame>(
            ledger.getLastClosedLedgerHeader().hash);

        std::vector<TransactionFramePtr> txFrames;
        auto batchSize =
            std::min(accountsLeft, MAXIMAL_NUMBER_OF_TXS_PER_LEDGER);

        std::vector<LoadGenerator::AccountInfoPtr> newAccounts =
            LoadGenerator::createAccounts(batchSize, ledgerNum);

        for (AccountInfoPtr& account : newAccounts)
        {
            TxInfo tx = account->creationTransaction();
            tx.toTransactionFrames(app, txFrames, txm);
            tx.recordExecution(app.getConfig().DESIRED_BASE_FEE);
        }

        for (TransactionFramePtr txFrame : txFrames)
        {
            txSet->add(txFrame);
        }

        StellarValue value(txSet->getContentsHash(), ledgerNum,
                           emptyUpgradeSteps, 0);
        auto closeData = LedgerCloseData{ledgerNum, txSet, value};
        app.getLedgerManager().valueExternalized(closeData);

        accountsLeft -= batchSize;
    }
}

void
Benchmark::createAccountsUsingTransactions(Application& app, size_t n)
{
    auto ledgerNum = app.getLedgerManager().getLedgerNum();
    std::vector<LoadGenerator::AccountInfoPtr> newAccounts =
        LoadGenerator::createAccounts(n, ledgerNum);
    for (LoadGenerator::AccountInfoPtr account : newAccounts)
    {
        LoadGenerator::TxInfo tx = account->creationTransaction();
        if (!tx.execute(app))
        {
            CLOG(ERROR, LOGGER_ID)
                << "Error while executing CREATE_ACCOUNT transaction";
        }
    }
}

void
Benchmark::createAccountsDirectly(Application& app, size_t n)
{
    soci::transaction sqlTx(app.getDatabase().getSession());

    auto ledger = app.getLedgerManager().getLedgerNum();
    std::vector<LoadGenerator::AccountInfoPtr> createdAccounts =
        LoadGenerator::createAccounts(mNumberOfInitialAccounts, ledger);

    int64_t balanceDiff = 0;
    std::vector<LedgerEntry> live;
    std::transform(
        createdAccounts.begin(), createdAccounts.end(),
        std::back_inserter(live),
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

    StellarValue sv(app.getLedgerManager().getLastClosedLedgerHeader().hash,
                    ledger, emptyUpgradeSteps, 0);
    LedgerCloseData ledgerData = createData(app.getLedgerManager(), sv);
    app.getLedgerManager().closeLedger(ledgerData);

    mAccounts = createdAccounts;
}

void
Benchmark::setMaxTxSize(LedgerManager& ledger, uint32_t maxTxSetSize)
{
    StellarValue sv(ledger.getLastClosedLedgerHeader().hash,
                    ledger.getLedgerNum(), emptyUpgradeSteps, 0);
    {
        LedgerUpgrade up(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        up.newMaxTxSetSize() = maxTxSetSize;
        Value v(xdr::xdr_to_opaque(up));
        sv.upgrades.emplace_back(v.begin(), v.end());
    }
    LedgerCloseData ledgerData = createData(ledger, sv);
    ledger.closeLedger(ledgerData);
}

LedgerCloseData
Benchmark::createData(LedgerManager& ledger, StellarValue& value)
{
    auto ledgerNum = ledger.getLedgerNum();
    TxSetFramePtr txSet =
        std::make_shared<TxSetFrame>(ledger.getLastClosedLedgerHeader().hash);
    value.txSetHash = txSet->getContentsHash();
    return LedgerCloseData{ledgerNum, txSet, value};
}

void
Benchmark::prepareBenchmark(Application& app)
{
    CLOG(INFO, LOGGER_ID) << "Preparing data for benchmark";

    initializeMetrics(app.getMetrics());

    app.newDB();

    setMaxTxSize(app.getLedgerManager(), MAXIMAL_NUMBER_OF_TXS_PER_LEDGER);

    auto ledger = app.getLedgerManager().getLedgerNum();

    createAccountsDirectly(app, mNumberOfInitialAccounts);
    // createAccounts(app, mNumberOfInitialAccounts);
    // createAccountsUsingTransactions(app, mNumberOfInitialAccounts);

    app.getHistoryManager().queueCurrentHistory();

    CLOG(INFO, LOGGER_ID) << "Data for benchmark prepared";
}

void
Benchmark::initializeBenchmark(Application& app, uint32_t ledgerNum)
{
    mAccounts =
        LoadGenerator::createAccounts(mNumberOfInitialAccounts, ledgerNum);
    mRandomIterator = shuffleAccounts(mAccounts);
}

std::vector<LoadGenerator::AccountInfoPtr>::iterator
Benchmark::shuffleAccounts(std::vector<LoadGenerator::AccountInfoPtr>& accounts)
{
    auto rng = std::default_random_engine{0};
    std::shuffle(mAccounts.begin(), mAccounts.end(), rng);
    return mAccounts.begin();
}

LoadGenerator::AccountInfoPtr
Benchmark::pickRandomAccount(AccountInfoPtr tryToAvoid, uint32_t ledgerNum)
{
    if (mRandomIterator == mAccounts.end())
    {
        mRandomIterator = mAccounts.begin();
    }
    auto result = *mRandomIterator;
    mRandomIterator++;
    return result;
}
}
