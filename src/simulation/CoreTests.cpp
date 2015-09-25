// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/Topologies.h"
#include "lib/catch.hpp"
#include "overlay/StellarXDR.h"
#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "crypto/SHA.h"
#include "main/test.h"
#include "util/Logging.h"
#include "util/types.h"
#include "herder/Herder.h"
#include "transactions/TransactionFrame.h"

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

// Simulation tests. Some of the tests in this suite are long.
// They are marked with [long][hide]. Run the day-to-day tests with
//
//     --test
// or
//     --test [simulation]~[long]
//

void
printStats(int& nLedgers, std::chrono::system_clock::time_point tBegin,
           Simulation::pointer sim)
{
    auto t = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now() - tBegin);

    LOG(INFO) << "Time spent closing " << nLedgers << " ledgers with "
              << sim->getNodes().size() << " nodes : " << t.count()
              << " seconds";

    LOG(INFO) << sim->metricsSummary("scp");
}

#include "lib/util/lrucache.hpp"

TEST_CASE("3 nodes. 2 running. threshold 2", "[simulation][core3]")
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

    {
        Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        Simulation::pointer simulation =
            std::make_shared<Simulation>(mode, networkID);

        std::vector<SecretKey> keys;
        for (int i = 0; i < 3; i++)
        {
            keys.push_back(
                SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i))));
        }

        SCPQuorumSet qSet;
        qSet.threshold = 2;
        for (auto& k : keys)
        {
            qSet.validators.push_back(k.getPublicKey());
        }

        simulation->addNode(keys[0], qSet, simulation->getClock());
        simulation->addNode(keys[1], qSet, simulation->getClock());
        simulation->addPendingConnection(keys[0].getPublicKey(),
                                         keys[1].getPublicKey());

        auto tBegin = std::chrono::system_clock::now();

        LOG(INFO) << "#######################################################";

        simulation->startAllNodes();

        int nLedgers = 10;
        simulation->crankUntil(
            [&simulation, nLedgers]()
            {
                return simulation->haveAllExternalized(nLedgers + 1);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        // printStats(nLedgers, tBegin, simulation);

        REQUIRE(simulation->haveAllExternalized(nLedgers + 1));
    }
    LOG(DEBUG) << "done with core3 test";
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

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);

    for (int size = 2; size <= 4; size++)
    {
        auto tBegin = std::chrono::system_clock::now();

        Simulation::pointer sim = Topologies::core(size, 1.0, mode, networkID);
        sim->startAllNodes();

        int nLedgers = 4;
        sim->crankUntil(
            [&sim, nLedgers]()
            {
                return sim->haveAllExternalized(nLedgers + 1);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        REQUIRE(sim->haveAllExternalized(nLedgers + 1));

        // printStats(nLedgers, tBegin, sim);
    }
}

void
hierarchicalTopo(int nLedgers, int nBranches, Simulation::Mode mode,
                 Hash const& networkID)
{
    auto tBegin = std::chrono::system_clock::now();

    Simulation::pointer sim =
        Topologies::hierarchicalQuorum(nBranches, mode, networkID);
    sim->startAllNodes();

    sim->crankUntil(
        [&sim, nLedgers]()
        {
            return sim->haveAllExternalized(nLedgers + 1);
        },
        20 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    REQUIRE(sim->haveAllExternalized(nLedgers + 1));

    // printStats(nLedgers, tBegin, sim);
}

/* this test is still busted for some reason:
core4 doesn't close, connections get dropped
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
    for (int nBranches = 1; nBranches <= 3; nBranches += 2)
    {
        hierarchicalTopo(nLedgers, nBranches, mode);
    }
}
*/

TEST_CASE("cycle4 topology", "[simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::cycle4(networkID);
    simulation->startAllNodes();

    simulation->crankUntil(
        [&simulation]()
        {
            return simulation->haveAllExternalized(2);
        },
        std::chrono::seconds(20), true);

    // Still transiently does not work (quorum retrieval)
    REQUIRE(simulation->haveAllExternalized(2));
}

TEST_CASE("Stress test on 2 nodes 3 accounts 10 random transactions 10tx/sec",
          "[stress100][simulation][stress][long][hide]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID);

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]()
        {
            return simulation->haveAllExternalized(3);
        },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    simulation->executeAll(simulation->accountCreationTransactions(3));

    try
    {
        simulation->crankUntil(
            [&]()
            {
                // we need to wait 2 rounds in case the tx don't propagate
                // to the second node in time and the second node gets the
                // nomination
                return simulation->haveAllExternalized(5) &&
                       simulation->accountsOutOfSyncWithDb().empty();
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

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
            std::chrono::seconds(60), true);
    }
    catch (...)
    {
        auto problems = simulation->accountsOutOfSyncWithDb();
        REQUIRE(problems.empty());
    }

    LOG(INFO) << simulation->metricsSummary("database");
}

TEST_CASE("Auto-calibrated single node load test", "[autoload][hide]")
{
    Config cfg =
#ifdef USE_POSTGRES
        !force_sqlite ? getTestConfig(0, Config::TESTDB_POSTGRESQL) :
#endif
                      getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE);
    cfg.RUN_STANDALONE = false;
    cfg.PARANOID_MODE = false;
    cfg.DESIRED_MAX_TX_PER_LEDGER = 10000;
    VirtualClock clock(VirtualClock::REAL_TIME);
    Application::pointer appPtr = Application::create(clock, cfg);
    appPtr->start();
    // force maxTxSetSize to avoid throwing txSets on the floor during the first
    // ledger close
    appPtr->getLedgerManager().getCurrentLedgerHeader().maxTxSetSize =
        cfg.DESIRED_MAX_TX_PER_LEDGER;

    appPtr->generateLoad(100000, 100000, 10, true);
    auto& io = clock.getIOService();
    asio::io_service::work mainWork(io);
    auto& complete =
        appPtr->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    while (!io.stopped() && complete.count() == 0)
    {
        clock.crank();
    }
}
