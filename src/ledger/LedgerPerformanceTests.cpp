// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "database/Database.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include "simulation/Simulation.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/SociNoWarnings.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "util/optional.h"

using namespace stellar;
using namespace std;
using namespace soci;
using namespace medida;

typedef std::unique_ptr<Application> appPtr;

namespace stellar
{
class LedgerPerformanceTests : public Simulation
{
  public:
    size_t nAccounts = 10;

    Application::pointer mApp;

    LedgerPerformanceTests(Hash const& networkID)
        : Simulation(Simulation::OVER_LOOPBACK, networkID)
    {
    }

    void
    resizeAccounts(size_t n)
    {
        loadAccount(*mAccounts.front());
        mAccounts.resize(n);
    }
    optional<TxInfo>
    ensureAccountIsLoadedCreated(size_t i)
    {
        if (!mAccounts[i])
        {
            auto newAccount = createAccount(i);
            mAccounts[i] = newAccount;
            if (!loadAccount(*newAccount))
            {
                newAccount->mSeq =
                    LedgerHeaderFrame(
                        mApp->getLedgerManager().getCurrentLedgerHeader())
                        .getStartingSequenceNumber();
                return make_optional<TxInfo>(newAccount->creationTransaction());
            }
        }
        return nullopt<TxInfo>();
    }

    vector<TxInfo>
    createRandomTransaction_uniformLoadingCreating()
    {
        auto from = pickRandomAccount(mAccounts.at(0), 0);
        auto to = pickRandomAccount(from, 0);
        vector<optional<TxInfo>> txs;

        txs.push_back(ensureAccountIsLoadedCreated(from->mId));
        txs.push_back(ensureAccountIsLoadedCreated(to->mId));

        int64_t amount = static_cast<int64_t>(
            rand_fraction() * min(static_cast<int64_t>(1000),
                                  (from->mBalance - mMinBalance) / 3));
        txs.push_back(make_optional<TxInfo>(
            createTransferNativeTransaction(from, to, amount)));

        vector<TxInfo> result;
        for (auto tx : txs)
        {
            if (tx)
                result.push_back(*tx);
        }
        return result;
    }

    vector<TxInfo>
    createRandomTransactions_uniformLoadingCreating(size_t n)
    {
        vector<TxInfo> result;
        for (size_t i = 0; i < n; i++)
        {
            auto newTxs = createRandomTransaction_uniformLoadingCreating();
            std::copy(newTxs.begin(), newTxs.end(), std::back_inserter(result));
        }
        return result;
    }

    static pair<vector<TxInfo>, vector<TxInfo>>
    partitionCreationTransaction(vector<TxInfo> txs)
    {
        vector<Simulation::TxInfo> creationTxs;
        vector<Simulation::TxInfo> otherTxs;
        for (auto& tx : txs)
        {
            if (tx.mFrom->mId == 0)
                creationTxs.push_back(tx);
            else
                otherTxs.push_back(tx);
        }
        return pair<vector<TxInfo>, vector<TxInfo>>(creationTxs, otherTxs);
    }

    void
    closeLedger(vector<Simulation::TxInfo> txs)
    {
        auto baseFee = mApp->getConfig().TESTING_UPGRADE_DESIRED_FEE;
        LoadGenerator::TxMetrics txm(mApp->getMetrics());
        TxSetFramePtr txSet = make_shared<TxSetFrame>(
            mApp->getLedgerManager().getLastClosedLedgerHeader().hash);
        for (auto& tx : txs)
        {
            std::vector<TransactionFramePtr> txfs;
            tx.toTransactionFrames(*mApp, txfs, txm);
            for (auto f : txfs)
                txSet->add(f);
            tx.recordExecution(baseFee);
        }

        StellarValue sv(txSet->getContentsHash(),
                        VirtualClock::to_time_t(mApp->getClock().now()),
                        emptyUpgradeSteps, 0);
        LedgerCloseData ledgerData(mApp->getLedgerManager().getLedgerNum(),
                                   txSet, sv);

        mApp->getLedgerManager().closeLedger(ledgerData);
    }
};
}

TEST_CASE("ledger performance test", "[performance][hide]")
{
    int nAccounts = 10000000;
    int nLedgers =
        9 /* weeks */ * 7 * 24 * 60 * 60 / 5 /* seconds between ledgers */;
    int nTransactionsPerLedger = 3;

    auto cfg = getTestConfig(1);

    Hash networkID = sha256(cfg.NETWORK_PASSPHRASE);
    LedgerPerformanceTests sim(networkID);

    SIMULATION_CREATE_NODE(10);

    SCPQuorumSet qSet0;
    qSet0.threshold = 1;
    qSet0.validators.push_back(v10NodeID);

    cfg.DATABASE =
        SecretValue{"postgresql://host=localhost dbname=performance_test "
                    "user=test password=test"};
    cfg.BUCKET_DIR_PATH = "performance-test.db.buckets";
    cfg.MANUAL_CLOSE = true;
    sim.addNode(v10SecretKey, qSet0, &cfg);
    sim.mApp = sim.getNodes().front();

    sim.startAllNodes();

    Timer& ledgerTimer = sim.mApp->getMetrics().NewTimer(
        {"performance-test", "ledger", "close"});
    Timer& mergeTimer = sim.mApp->getBucketManager().getMergeTimer();
    // use uint64_t for iAccounts to prevent overflows
    for (uint64_t iAccounts = 1000000; iAccounts <= nAccounts; iAccounts *= 10)
    {
        ledgerTimer.Clear();
        mergeTimer.Clear();

        LOG(INFO) << "Performance test with " << iAccounts
                  << " accounts, starting";
        sim.resizeAccounts(iAccounts);

        for (int iLedgers = 0; iLedgers < nLedgers; iLedgers++)
        {
            auto txs = sim.createRandomTransactions_uniformLoadingCreating(
                nTransactionsPerLedger);

            auto scope = ledgerTimer.TimeScope();

            auto createTxs_otherTxs =
                LedgerPerformanceTests::partitionCreationTransaction(txs);
            if (!createTxs_otherTxs.first.empty())
            {
                sim.closeLedger(createTxs_otherTxs.first);
            }
            sim.closeLedger(createTxs_otherTxs.second);

            while (sim.crankAllNodes() > 0)
                ;

            cout << ".";
            cout.flush();

            if (iLedgers % 1000 == 0 && iLedgers != 0)
            {
                LOG(INFO) << endl
                          << "Performance test with " << iAccounts
                          << " accounts after " << iLedgers << " ledgers";
                LOG(INFO) << endl << sim.metricsSummary("performance-test");
                LOG(INFO) << endl << sim.metricsSummary("bucket");
            }
        }
        LOG(INFO) << "Performance test with " << iAccounts << " accounts, done";
        LOG(INFO) << endl << sim.metricsSummary("performance-test");
        LOG(INFO) << endl << sim.metricsSummary("bucket");
        LOG(INFO) << "done";
    }
}
