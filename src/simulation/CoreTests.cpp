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
#include "herder/Herder.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;


// Simulation tests. Some of the tests in this suite are long.
// They are marked with [long][hide]. Run the day-to-day tests with 
//
//     --test 
// or
//     --test [simulation]~[long]
//


void printStats(int& nLedgers, std::chrono::system_clock::time_point tBegin, Simulation::pointer sim)
{
    auto t = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - tBegin);

    LOG(INFO) << "Time spent closing " << nLedgers << " ledgers with " << sim->getNodes().size() << " nodes : "
        << t.count() << " seconds";

    LOG(INFO) << sim->metricsSummary("scp");
}



TEST_CASE("core topology: 4 ledgers at scales 2..4", "[simulation]")
{
    Simulation::Mode mode = Simulation::OVER_LOOPBACK;
    SECTION("Over loopback")
    {
        mode = Simulation::OVER_LOOPBACK;
    }
    SECTION("Over tcp")
    {
        mode = Simulation::OVER_TCP;
    }

    for (int size = 2; size <= 4; size++)
    {
        auto tBegin = std::chrono::system_clock::now();

        Simulation::pointer sim = Topologies::core(size, 1.0, mode);
        sim->startAllNodes();

        int nLedgers = 4;
        sim->crankUntil(
            [&sim, nLedgers]()
        {
            return sim->haveAllExternalized(nLedgers+1);
        },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS);

        REQUIRE(sim->haveAllExternalized(nLedgers+1));

        printStats(nLedgers, tBegin, sim);
    }
}

void
hierarchicalTopo(int nLedgers, int nBranches, Simulation::Mode mode)
{
    auto tBegin = std::chrono::system_clock::now();

    Simulation::pointer sim = Topologies::hierarchicalQuorum(nBranches, mode);
    sim->startAllNodes();

    sim->crankUntil(
        [&sim, nLedgers]()
    {
        return sim->haveAllExternalized(nLedgers+1);
    },
        20 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS);

    REQUIRE(sim->haveAllExternalized(nLedgers+1));

    printStats(nLedgers, tBegin, sim);
}

TEST_CASE("hierarchical topology scales 1..3", "[simulation]")
{
    Simulation::Mode mode = Simulation::OVER_LOOPBACK;
    SECTION("Over loopback")
    {
        LOG(DEBUG) << "OVER_LOOPBACK";
        mode = Simulation::OVER_LOOPBACK;
    }
    SECTION("Over tcp")
    {
        LOG(DEBUG) << "OVER_TCP";
        mode = Simulation::OVER_TCP;
    }

    int const nLedgers = 4;
    for (auto nBranches = 1; nBranches <= 3; nBranches+=2)
    {
        hierarchicalTopo(nLedgers, nBranches, mode);
    }
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
    REQUIRE(simulation->haveAllExternalized(2));
}

TEST_CASE(
    "Stress test on 2 nodes 3 accounts 10 random transactions 10tx/sec",
    "[stress100][simulation][stress][long][hide]")
{
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK);

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]()
        {
            return simulation->haveAllExternalized(3);
        },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS);

    simulation->executeAll(simulation->accountCreationTransactions(3));

    try
    {
        simulation->crankUntil(
            [&]()
            {
                return simulation->haveAllExternalized(4) &&
                       simulation->accountsOutOfSyncWithDb().empty();
            },
            2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS);

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
