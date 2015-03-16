// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "simulation/Topologies.h"
#include "lib/catch.hpp"
#include "generated/StellarXDR.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "crypto/SHA.h"
#include "main/test.h"
#include "util/Logging.h"
#include "util/types.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("core4 topology", "[simulation]")
{
    Simulation::pointer simulation = Topologies::core4();
    simulation->startAllNodes();

    simulation->crankForAtMost(std::chrono::seconds(2));

    REQUIRE(simulation->haveAllExternalized(3));
}

TEST_CASE("cycle4 topology", "[simulation]")
{
    Simulation::pointer simulation = Topologies::cycle4();
    simulation->startAllNodes();

    simulation->crankForAtMost(std::chrono::seconds(20));

    // Still transiently does not work (quorum retrieval)
    CHECK(simulation->haveAllExternalized(2));
}

TEST_CASE(
    "Stress test on 2 nodes, 3 accounts, 10 random transactions, 10tx/sec",
    "[stress100][simulation][stress][hide]")
{
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK);

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]()
        {
            return simulation->haveAllExternalized(3);
        },
        std::chrono::seconds(60000));

    simulation->executeAll(simulation->createAccounts(3));

    try
    {
        simulation->crankUntil(
            [&]()
            {
                return simulation->haveAllExternalized(4) &&
                       simulation->accountsOutOfSyncWithDb().empty();
            },
            std::chrono::seconds(60));

        auto crankingTime =
            simulation->executeStressTest(10, 10, [&simulation](size_t i)
                                          {
            return simulation->createRandomTransaction(0.5);
        });

        simulation->crankUntil(
            [&]()
            {
                return simulation->accountsOutOfSyncWithDb().empty();
            },
            std::chrono::seconds(60));
    }
    catch (...)
    {
        auto problems = simulation->accountsOutOfSyncWithDb();
        REQUIRE(problems.empty());
    }

    LOG(INFO) << simulation->metricsSummary("database");
}
