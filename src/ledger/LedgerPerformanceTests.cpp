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
#include "ledger/LedgerMaster.h"
#include "transactions/TxTests.h"
#include "database/Database.h"
#include "main/Config.h"
#include "simulation/Simulation.h"
#include <soci.h>

using namespace stellar;
using namespace std;
using namespace soci;

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

    void ensureNAccounts(size_t n)
    {
        auto creationTransactions = createAccounts(n);
        bool loading = true;
        bool needToCrank = false;

        loadAccount(*mAccounts.front());
        for(auto &tx : creationTransactions)
        {
            if (loading && !loadAccount(*tx.mTo))
            {
                // Could not load this account since it hasn't been created yet. 
                // Start creating them.
                loading = false;
            }
            if (!loading)
            {
                execute(tx);
                needToCrank = true;
            }
        }
        if (needToCrank)
        {
            crankUntilSync(chrono::seconds(10));
        }
    }
    void closeLedgerWithRandomTransactions(size_t n)
    {
        auto baseFee = mApp->getConfig().DESIRED_BASE_FEE;
        auto txs = createRandomTransactions(n, 0.5);
        TxSetFramePtr txSet = make_shared<TxSetFrame>(mApp->getLedgerMaster().getLastClosedLedgerHeader().hash);
        for(auto& tx: txs)
        {
            txSet->add(tx.createPaymentTx());
            tx.recordExecution(baseFee);
        }

        LedgerCloseData ledgerData(mApp->getLedgerMaster().getLedgerNum(), 
            txSet, 
            VirtualClock::to_time_t(mApp->getClock().now()), 
            baseFee);

        mApp->getLedgerMaster().closeLedger(ledgerData);
    }
};

}


TEST_CASE("ledger performance test", "[ledger][performance][hide]")
{
    LedgerPerformanceTests sim;

    SIMULATION_CREATE_NODE(10);

    SCPQuorumSet qSet0;
    qSet0.threshold = 1;
    qSet0.validators.push_back(v10NodeID);

    auto cfg = getTestConfig(1);
    cfg.DATABASE = "sqlite3://performance-test.db";
    cfg.REBUILD_DB = !ifstream("performance-test.db"); //  rebuild if the file doesn't exists
    
    auto n0 = sim.addNode(v10VSeed, qSet0, sim.getClock(), make_shared<Config>(cfg));
    sim.mApp = sim.getNodes().front();

    sim.startAllNodes();
    sim.ensureNAccounts(100);
    sim.closeLedgerWithRandomTransactions(1000);

    LOG(INFO) << "Done.";
}
