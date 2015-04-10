// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerHeaderFrame.h"
#include "transactions/TxTests.h"
#include "database/Database.h"
#include "main/Config.h"
#include "main/PersistentState.h"
#include "simulation/Simulation.h"
#include <soci.h>
#include "crypto/Base58.h"
#include "bucket/BucketManager.h"
#include "util/optional.h"
#include "util/Math.h"

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

    LedgerPerformanceTests()
        : Simulation(Simulation::OVER_LOOPBACK) {}

    void resizeAccounts(size_t n)
    {
        loadAccount(*mAccounts.front());
        mAccounts.resize(n);
    }
    optional<TxInfo> ensureAccountIsLoadedCreated(size_t i)
    {
        if (!mAccounts[i])
        {
            auto newAccount = createAccount(i);
            mAccounts[i] = newAccount;
            if (!loadAccount(*newAccount))
            {
                newAccount->mSeq = LedgerHeaderFrame(mApp->getLedgerManager()
                    .getCurrentLedgerHeader())
                    .getStartingSequenceNumber();
                return make_optional<TxInfo>(newAccount->creationTransaction());
            }
        }
        return nullopt<TxInfo>();
    }

    vector<TxInfo>
    createRandomTransaction_uniformLoadingCreating()
    {
        vector<optional<TxInfo>> txs;
        size_t iFrom, iTo;
        do
        {
            iFrom = static_cast<int>(rand_fraction() * mAccounts.size());
            iTo = static_cast<int>(rand_fraction() * mAccounts.size());
        } while (iFrom == iTo);

        txs.push_back(ensureAccountIsLoadedCreated(iFrom));
        txs.push_back(ensureAccountIsLoadedCreated(iTo));

        uint64_t amount = static_cast<uint64_t>(
            rand_fraction() *
            min(static_cast<uint64_t>(1000),
            (mAccounts[iFrom]->mBalance - getMinBalance()) / 3));
        txs.push_back(make_optional<TxInfo>(createTransferTransaction(iFrom, iTo, amount)));

        vector<TxInfo> result;
        for(auto tx : txs)
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

    static
    pair<vector<TxInfo>, vector<TxInfo>>
    partitionCreationTransaction(vector<TxInfo> txs)
    {
        vector<Simulation::TxInfo> creationTxs;
        vector<Simulation::TxInfo> otherTxs;
        for (auto tx : txs)
        {
            if (tx.mFrom->mId == 0)
                creationTxs.push_back(tx);
            else
                otherTxs.push_back(tx);
        }
        return pair<vector<TxInfo>, vector<TxInfo>>(creationTxs, otherTxs);
    }


    void closeLedger(vector<Simulation::TxInfo> txs)
    {
        auto baseFee = mApp->getConfig().DESIRED_BASE_FEE;
        TxSetFramePtr txSet = make_shared<TxSetFrame>(mApp->getLedgerManager().getLastClosedLedgerHeader().hash);
        for (auto& tx : txs)
        {
            txSet->add(tx.createPaymentTx());
            tx.recordExecution(baseFee);
        }

        LedgerCloseData ledgerData(mApp->getLedgerManager().getLedgerNum(),
            txSet,
            VirtualClock::to_time_t(mApp->getClock().now()),
            baseFee);

        mApp->getLedgerManager().closeLedger(ledgerData);
    }

};

}

TEST_CASE("ledger performance test", "[ledger][performance][hide]")
{
    int nAccounts = 10000000;
    int nLedgers = 9 /* weeks */ * 7 * 24 * 60 * 60
                 / 5 /* seconds between ledgers */;
    int nTransactionsPerLedger = 3;


    LedgerPerformanceTests sim;

    SIMULATION_CREATE_NODE(10);

    SCPQuorumSet qSet0;
    qSet0.threshold = 1;
    qSet0.validators.push_back(v10NodeID);

    auto cfg = getTestConfig(1);
    cfg.REBUILD_DB = false;
    cfg.DATABASE = "postgresql://host=localhost dbname=performance_test user=test password=test";
    cfg.BUCKET_DIR_PATH = "performance-test.db.buckets";
    cfg.MANUAL_CLOSE = true;
    sim.addNode(v10SecretKey, qSet0, sim.getClock(), make_shared<Config>(cfg));
    sim.mApp = sim.getNodes().front();
    if (sim.mApp->getPersistentState().getState(PersistentState::kDatabaseInitialized) != "true")
    {
        sim.mApp->getDatabase().initialize();
    }

    sim.startAllNodes();


        
    Timer& ledgerTimer = sim.mApp->getMetrics().NewTimer({ "performance-test", "ledger", "close" });
    Timer& mergeTimer = sim.mApp->getBucketManager().getMergeTimer();
    for (int iAccounts = 1000000; iAccounts <= nAccounts; iAccounts *= 10)
    {
        ledgerTimer.Clear();
        mergeTimer.Clear();

        LOG(INFO) << "Performance test with " << iAccounts << " accounts, starting";
        sim.resizeAccounts(iAccounts);

        for (int iLedgers = 0; iLedgers < nLedgers; iLedgers++)
        {
            auto txs = sim.createRandomTransactions_uniformLoadingCreating(nTransactionsPerLedger);

            auto scope = ledgerTimer.TimeScope();

            auto createTxs_otherTxs = LedgerPerformanceTests::partitionCreationTransaction(txs);
            if (!createTxs_otherTxs.first.empty())
            {
                sim.closeLedger(createTxs_otherTxs.first);
            }
            sim.closeLedger(createTxs_otherTxs.second);

            while (sim.crankAllNodes() > 0);

            cout << ".";
            cout.flush();

            if (iLedgers % 1000 == 0 && iLedgers != 0)
            {
                LOG(INFO) << endl << "Performance test with " << iAccounts << " accounts after " << iLedgers << " ledgers";
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
