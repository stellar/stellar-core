// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "util/Timer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "ledger/LedgerManagerImpl.h"
#include "transactions/TxTests.h"
#include "database/Database.h"
#include "main/Config.h"
#include "simulation/Simulation.h"
#include <soci.h>
#include "crypto/Base58.h"
#include "clf/CLFManager.h"

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

    void ensureNAccounts(size_t n, size_t batchSize = 1000)
    {
        loadAccount(*mAccounts.front());

        auto newAccounts = createAccounts(max<size_t>(0, n - mAccounts.size() + 1));

        vector<TxInfo> txs;
        for (int i = 0; i < newAccounts.size(); i++)
        {
            if (loadAccount(*newAccounts[i]))
            {
                if (newAccounts[i]->mId % batchSize == 0)
                    LOG(INFO) << "... loaded up to account " << newAccounts[i]->mId;
            } else
            {
                txs.push_back(newAccounts[i]->creationTransaction());
                newAccounts[i]->mSeq = LedgerHeaderFrame(mApp->getLedgerManagerImpl()
                    .getCurrentLedgerHeader())
                    .getStartingSequenceNumber();
            }
            if (txs.size() == batchSize)
            {
                closeLedger(txs);
                LOG(INFO) << "...created up to account " << newAccounts[i]->mId;
                txs.clear();
            }
        }
        if (txs.size() > 0 )
        {
            closeLedger(txs);
        }
    }
    void closeLedgerWithRandomTransactions(size_t n)
    {
        auto txs = createRandomTransactions(n, 0.5);
        closeLedger(txs);
    }

    void closeLedger(vector<Simulation::TxInfo> txs)
    {
        auto baseFee = mApp->getConfig().DESIRED_BASE_FEE;
        TxSetFramePtr txSet = make_shared<TxSetFrame>(mApp->getLedgerManagerImpl().getLastClosedLedgerHeader().hash);
        for (auto& tx : txs)
        {
            txSet->add(tx.createPaymentTx());
            tx.recordExecution(baseFee);
        }

        LedgerCloseData ledgerData(mApp->getLedgerManagerImpl().getLedgerNum(),
            txSet,
            VirtualClock::to_time_t(mApp->getClock().now()),
            baseFee);

        mApp->getLedgerManagerImpl().closeLedger(ledgerData);
    }

};

}

TEST_CASE("ledger performance test", "[ledger][performance][hide]")
{
    int nAccounts = 10000000;
    int nLedgers = 1000;
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
    auto n0 = sim.addNode(v10VSeed, qSet0, sim.getClock(), make_shared<Config>(cfg));
    sim.mApp = sim.getNodes().front();
    if (sim.mApp->getPersistentState().getState(PersistentState::kDatabaseInitialized) != "true")
    {
        sim.mApp->getDatabase().initialize();
    }

    sim.startAllNodes();


        
    Timer& ledgerTimer = sim.mApp->getMetrics().NewTimer({ "performance-test", "ledger", "close" });
    Timer& mergeTimer = sim.mApp->getCLFManager().getMergeTimer();
    for (int iAccounts = 1000; iAccounts < nAccounts; iAccounts *= 10)
    {
        ledgerTimer.Clear();
        mergeTimer.Clear();

        LOG(INFO) << "Performance test with " << iAccounts << ", loading/creating accounts";
        sim.ensureNAccounts(iAccounts);
        LOG(INFO) << "Performance test with " << iAccounts << ", running";

        for (int iLedgers = 0; iLedgers < nLedgers; iLedgers++)
        {
            auto txs = sim.createRandomTransactions(nTransactionsPerLedger, 0.5);

            auto scope = ledgerTimer.TimeScope();
            sim.closeLedger(txs);
            while (sim.crankAllNodes() > 0);
        }
        LOG(INFO) << "Performance test with " << iAccounts << ", done";
        LOG(INFO) << endl << sim.metricsSummary("performance-test");
        LOG(INFO) << endl << sim.metricsSummary("bucket");
        LOG(INFO) << "done";
    }
}
