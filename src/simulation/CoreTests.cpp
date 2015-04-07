// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

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
#include "herder/HerderImpl.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("core4 topology", "[simulation]")
{
    Simulation::pointer simulation = Topologies::core(4, 1.0, Simulation::OVER_LOOPBACK);
    simulation->startAllNodes();

    simulation->crankUntil(
        [&simulation]()
        {
            return simulation->haveAllExternalized(3);
        },
        std::chrono::seconds(2* EXP_LEDGER_TIMESPAN_SECONDS));

    REQUIRE(simulation->haveAllExternalized(3));
}

TEST_CASE("hierarchical topology at multiple scales", "[simulation][hide]")
{
    int const nLedgers = 3;
    std::vector<std::chrono::seconds> times;

    for (int nBranches = 0; nBranches < 30; nBranches++)
    {
        auto tBegin = std::chrono::system_clock::now();

        Simulation::pointer sim = Topologies::hierarchicalQuorum(nBranches);
        sim->startAllNodes();

        sim->crankUntil(
            [&sim, nLedgers]()
        {
            return sim->haveAllExternalized(nLedgers);
        },
            100 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS);

        REQUIRE(sim->haveAllExternalized(3));

        auto t = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now() - tBegin);
        times.push_back(t);

        LOG(INFO) << "Time spent closing " << nLedgers << " ledgers with " << sim->getNodes().size() << " nodes : "
            << t.count() << " seconds";

        LOG(INFO) << sim->metricsSummary("scp");
    }
}



TEST_CASE("core4 topology long, over tcp", "[simulation][long][hide]")
{
    Simulation::pointer simulation = Topologies::core(3, 1.0, Simulation::OVER_TCP);
    simulation->startAllNodes();

    simulation->crankUntil(
        [&simulation]()
    {
        return simulation->haveAllExternalized(30);
    },
        std::chrono::seconds(30 * EXP_LEDGER_TIMESPAN_SECONDS*2));

    REQUIRE(simulation->haveAllExternalized(3));
}


TEST_CASE("cycle4 topology", "[simulation]")
{
    Simulation::pointer simulation = Topologies::cycle4();
    simulation->startAllNodes();

    simulation->crankUntil(
        [&simulation]()
        {
            return simulation->haveAllExternalized(2);
        },
        std::chrono::seconds(20));

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
        std::chrono::seconds(2 * EXP_LEDGER_TIMESPAN_SECONDS));

    simulation->executeAll(simulation->accountCreationTransactions(3));

    try
    {
        simulation->crankUntil(
            [&]()
            {
                return simulation->haveAllExternalized(4) &&
                       simulation->accountsOutOfSyncWithDb().empty();
            },
            std::chrono::seconds(2 * EXP_LEDGER_TIMESPAN_SECONDS));

        auto crankingTime = simulation->executeStressTest(
            10, 10, [&simulation](size_t i)
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
