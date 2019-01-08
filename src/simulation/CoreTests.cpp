// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketManagerImpl.h"
#include "bucket/LedgerCmp.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/format.h"
#include "main/Application.h"
#include "medida/stats/snapshot.h"
#include "overlay/StellarXDR.h"
#include "simulation/Topologies.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/format.h"
#include "util/types.h"
#include "xdrpp/autocheck.h"
#include <sstream>

using namespace stellar;

typedef std::unique_ptr<Application> appPtr;

// Simulation tests. Some of the tests in this suite are long.
// They are marked with [long][!hide]. Run the day-to-day tests with
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

TEST_CASE("3 nodes 2 running threshold 2", "[simulation][core3]")
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

        simulation->addNode(keys[0], qSet);
        simulation->addNode(keys[1], qSet);
        simulation->addPendingConnection(keys[0].getPublicKey(),
                                         keys[1].getPublicKey());

        auto tBegin = std::chrono::system_clock::now();

        LOG(INFO) << "#######################################################";

        simulation->startAllNodes();

        int nLedgers = 10;
        simulation->crankUntil(
            [&simulation, nLedgers]() {
                return simulation->haveAllExternalized(nLedgers + 1, 5);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        // printStats(nLedgers, tBegin, simulation);

        REQUIRE(simulation->haveAllExternalized(nLedgers + 1, 5));
    }
    LOG(DEBUG) << "done with core3 test";
}

TEST_CASE("core topology 4 ledgers at scales 2 to 4", "[simulation]")
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
            [&sim, nLedgers]() {
                return sim->haveAllExternalized(nLedgers + 1, nLedgers);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        REQUIRE(sim->haveAllExternalized(nLedgers + 1, 5));

        // printStats(nLedgers, tBegin, sim);
    }
}

static void
resilienceTest(Simulation::pointer sim)
{
    auto nodes = sim->getNodeIDs();
    auto nbNodes = nodes.size();

    for (size_t i = 0; i < nbNodes; i++)
    {
        // now restart a victim node i, will reconnect to
        // j to join the network
        auto j = (i + 1) % nbNodes;

        auto victimID = nodes[i];
        auto otherID = nodes[j];
        SECTION(fmt::format("restart victim {}", i))
        {
            // bring network to a good place
            sim->startAllNodes();

            uint32 targetLedger = 2;
            const uint32 nbLedgerStep = 3;

            auto crankForward = [&](uint32 step, uint32 maxGap) {
                targetLedger += step;
                sim->crankUntil(
                    [&]() {
                        return sim->haveAllExternalized(targetLedger, maxGap);
                    },
                    2 * nbLedgerStep * Herder::EXP_LEDGER_TIMESPAN_SECONDS,
                    false);

                REQUIRE(sim->haveAllExternalized(targetLedger, maxGap));
            };

            crankForward(nbLedgerStep, 1);

            auto victimConfig = sim->getNode(victimID)->getConfig();
            // kill instance
            sim->removeNode(victimID);
            // let the rest of the network move on
            crankForward(nbLedgerStep, 1);
            // start the instance
            sim->addNode(victimConfig.NODE_SEED, victimConfig.QUORUM_SET,
                         &victimConfig, false);
            auto refreshedApp = sim->getNode(victimID);
            refreshedApp->start();
            // connect to another node
            sim->addConnection(victimID, otherID);
            // this crank should allow the node to rejoin the network
            crankForward(1, INT32_MAX);
            // network should be fully in sync now
            crankForward(nbLedgerStep, 1);
        }
    }
}
TEST_CASE("resilience tests", "[resilience][simulation][long][!hide]")
{
    Simulation::Mode mode = Simulation::OVER_LOOPBACK;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);

    auto confGen = [](int configNum) -> Config {
        // we have to have persistent nodes as we want to simulate a restart
        auto c = getTestConfig(configNum, Config::TESTDB_ON_DISK_SQLITE);
        return c;
    };

    SECTION("custom-A")
    {
        resilienceTest(Topologies::customA(mode, networkID, confGen, 2));
    }
    SECTION("hierarchical")
    {
        resilienceTest(
            Topologies::hierarchicalQuorum(2, mode, networkID, confGen, 2));
    }
    SECTION("simplified hierarchical")
    {
        resilienceTest(Topologies::hierarchicalQuorumSimplified(
            4, 3, mode, networkID, confGen, 2));
    }
    SECTION("core4")
    {
        resilienceTest(Topologies::core(4, 0.75, mode, networkID, confGen));
    }
    SECTION("branched cycle")
    {
        resilienceTest(
            Topologies::branchedcycle(5, 0.6, mode, networkID, confGen));
    }
}

static void
hierarchicalTopoTest(int nLedgers, int nBranches, Simulation::Mode mode,
                     Hash const& networkID)
{
    LOG(DEBUG) << "starting topo test " << nLedgers << " : " << nBranches;
    auto tBegin = std::chrono::system_clock::now();

    Simulation::pointer sim =
        Topologies::hierarchicalQuorum(nBranches, mode, networkID);
    sim->startAllNodes();

    sim->crankUntil(
        [&sim, nLedgers]() {
            return sim->haveAllExternalized(nLedgers + 1, 5);
        },
        20 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    REQUIRE(sim->haveAllExternalized(nLedgers + 1, 5));

    // printStats(nLedgers, tBegin, sim);
}

TEST_CASE("hierarchical topology scales 1 to 3", "[simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::Mode mode = Simulation::OVER_LOOPBACK;
    auto test = [&]() {
        int const nLedgers = 4;
        for (int nBranches = 1; nBranches <= 3; nBranches += 2)
        {
            hierarchicalTopoTest(nLedgers, nBranches, mode, networkID);
        }
    };
    SECTION("Over loopback")
    {
        LOG(DEBUG) << "OVER_LOOPBACK";
        mode = Simulation::OVER_LOOPBACK;
        test();
    }
    SECTION("Over tcp")
    {
        LOG(DEBUG) << "OVER_TCP";
        mode = Simulation::OVER_TCP;
        test();
    }
}

static void
hierarchicalSimplifiedTest(int nLedgers, int nbCore, int nbOuterNodes,
                           Simulation::Mode mode, Hash const& networkID)
{
    LOG(DEBUG) << "starting simplified test " << nLedgers << " : " << nbCore;
    auto tBegin = std::chrono::system_clock::now();

    Simulation::pointer sim = Topologies::hierarchicalQuorumSimplified(
        nbCore, nbOuterNodes, mode, networkID);
    sim->startAllNodes();

    sim->crankUntil(
        [&sim, nLedgers]() {
            return sim->haveAllExternalized(nLedgers + 1, 3);
        },
        20 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    REQUIRE(sim->haveAllExternalized(nLedgers + 1, 3));

    // printStats(nLedgers, tBegin, sim);
}

TEST_CASE("core nodes with outer nodes", "[simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::Mode mode = Simulation::OVER_LOOPBACK;
    SECTION("Over loopback")
    {
        mode = Simulation::OVER_LOOPBACK;
        hierarchicalSimplifiedTest(4, 5, 10, mode, networkID);
    }
    SECTION("Over tcp")
    {
        mode = Simulation::OVER_TCP;
        hierarchicalSimplifiedTest(4, 5, 10, mode, networkID);
    }
}

TEST_CASE("cycle4 topology", "[simulation]")
{
    const int nLedgers = 10;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation = Topologies::cycle4(networkID);
    simulation->startAllNodes();

    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(nLedgers + 2, 4); },
        2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    // Still transiently does not work (quorum retrieval)
    REQUIRE(simulation->haveAllExternalized(nLedgers, 4));
}

TEST_CASE(
    "Stress test on 2 nodes 3 accounts 10 random transactions 10tx per sec",
    "[stress100][simulation][stress][long][!hide]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::pair(Simulation::OVER_LOOPBACK, networkID);

    simulation->startAllNodes();
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(3, 1); },
        2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    auto nodes = simulation->getNodes();
    auto& app = *nodes[0]; // pick a node to generate load

    app.getLoadGenerator().generateLoad(true, 3, 0, 0, 10, 100, false);
    try
    {
        simulation->crankUntil(
            [&]() {
                // we need to wait 2 rounds in case the tx don't propagate
                // to the second node in time and the second node gets the
                // nomination
                return simulation->haveAllExternalized(5, 2) &&
                       simulation->accountsOutOfSyncWithDb(app).empty();
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        app.getLoadGenerator().generateLoad(false, 3, 0, 10, 10, 100, false);
        simulation->crankUntil(
            [&]() {
                return simulation->haveAllExternalized(8, 2) &&
                       simulation->accountsOutOfSyncWithDb(app).empty();
            },
            2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);
    }
    catch (...)
    {
        auto problems = simulation->accountsOutOfSyncWithDb(app);
        REQUIRE(problems.empty());
    }

    LOG(INFO) << simulation->metricsSummary("database");
}

Application::pointer
newLoadTestApp(VirtualClock& clock)
{
    Config cfg =
#ifdef USE_POSTGRES
        !force_sqlite ? getTestConfig(0, Config::TESTDB_POSTGRESQL) :
#endif
                      getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE);
    cfg.RUN_STANDALONE = false;
    // force maxTxSetSize to avoid throwing txSets on the floor during the first
    // ledger close
    cfg.TESTING_UPGRADE_MAX_TX_PER_LEDGER = 10000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    Application::pointer appPtr = Application::create(clock, cfg);
    appPtr->start();
    return appPtr;
}

TEST_CASE("Auto calibrated single node load test", "[autoload][!hide]")
{
    VirtualClock clock(VirtualClock::REAL_TIME);
    auto appPtr = newLoadTestApp(clock);
    // Create accounts
    appPtr->generateLoad(true, 100000, 0, 0, 10, 3, true);
    auto& io = clock.getIOService();
    asio::io_service::work mainWork(io);
    auto& complete =
        appPtr->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");
    while (!io.stopped() && complete.count() == 0)
    {
        clock.crank();
    }
    // Generate payments
    appPtr->generateLoad(false, 100000, 0, 100000, 10, 100, true);
    while (!io.stopped() && complete.count() == 1)
    {
        clock.crank();
    }
}

class ScaleReporter
{
    std::vector<std::string> mColumns;
    std::string mFilename;
    std::ofstream mOut;
    size_t mNumWritten{0};
    static std::string
    join(std::vector<std::string> const& parts, std::string const& sep)
    {
        std::string sum;
        bool first = true;
        for (auto const& s : parts)
        {
            if (first)
            {
                first = false;
            }
            else
            {
                sum += sep;
            }
            sum += s;
        }
        return sum;
    }

  public:
    ScaleReporter(std::vector<std::string> const& columns)
        : mColumns(columns)
        , mFilename(fmt::format("{:s}-{:d}.csv", join(columns, "-vs-"),
                                std::time(nullptr)))
        , mOut(mFilename)
    {
        LOG(INFO) << "Opened " << mFilename << " for writing";
        mOut << join(columns, ",") << std::endl;
    }

    ~ScaleReporter()
    {
        LOG(INFO) << "Wrote " << mNumWritten << " rows to " << mFilename;
    }

    void
    write(std::vector<double> const& vals)
    {
        assert(vals.size() == mColumns.size());
        std::ostringstream oss;
        for (size_t i = 0; i < vals.size(); ++i)
        {
            if (i != 0)
            {
                oss << ", ";
                mOut << ",";
            }
            oss << mColumns.at(i) << "=" << std::fixed << vals.at(i);
            mOut << std::fixed << vals.at(i);
        }
        LOG(INFO) << std::fixed << "Writing " << oss.str();
        mOut << std::endl;
        ++mNumWritten;
    }
};

TEST_CASE("Accounts vs latency", "[scalability][!hide]")
{
    ScaleReporter r({"accounts", "txcount", "latencymin", "latencymax",
                     "latency50", "latency95", "latency99"});

    VirtualClock clock;
    auto appPtr = newLoadTestApp(clock);
    auto& app = *appPtr;

    auto& lg = app.getLoadGenerator();
    auto& txtime = app.getMetrics().NewTimer({"ledger", "operation", "apply"});
    uint32_t numItems = 500000;

    // Create accounts
    lg.generateLoad(true, numItems, 0, 0, 10, 100, true);

    auto& complete =
        appPtr->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");

    auto& io = clock.getIOService();
    asio::io_service::work mainWork(io);
    while (!io.stopped() && complete.count() == 0)
    {
        clock.crank();
    }

    txtime.Clear();

    // Generate payment txs
    lg.generateLoad(false, numItems, 0, numItems / 10, 10, 100, true);
    while (!io.stopped() && complete.count() == 1)
    {
        clock.crank();
    }

    // Report latency
    app.reportCfgMetrics();
    r.write({(double)numItems, (double)txtime.count(), txtime.min(),
             txtime.max(), txtime.GetSnapshot().getMedian(),
             txtime.GetSnapshot().get95thPercentile(),
             txtime.GetSnapshot().get99thPercentile()});
}

static void
netTopologyTest(std::string const& name,
                std::function<Simulation::pointer(int numNodes)> mkSim)
{
    ScaleReporter r(
        {name + "nodes", "in-msg", "in-byte", "out-msg", "out-byte"});

    for (int numNodes = 4; numNodes < 64; numNodes += 4)
    {
        auto sim = mkSim(numNodes);
        sim->startAllNodes();
        sim->crankUntil([&]() { return sim->haveAllExternalized(5, 4); },
                        2 * 5 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        REQUIRE(sim->haveAllExternalized(5, 4));

        auto nodes = sim->getNodes();
        assert(!nodes.empty());
        auto& app = *nodes[0];

        app.getLoadGenerator().generateLoad(true, 50, 0, 0, 10, 100, false);
        auto& complete =
            app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");

        sim->crankUntil(
            [&]() {
                return sim->haveAllExternalized(8, 2) &&
                       sim->accountsOutOfSyncWithDb(app).empty() &&
                       complete.count() == 1;
            },
            2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        app.reportCfgMetrics();

        auto& inmsg = app.getMetrics().NewMeter({"overlay", "message", "read"},
                                                "message");
        auto& inbyte =
            app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte");

        auto& outmsg = app.getMetrics().NewMeter(
            {"overlay", "message", "write"}, "message");
        auto& outbyte =
            app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte");

        r.write({
            (double)numNodes,
            (double)inmsg.count(),
            (double)inbyte.count(),
            (double)outmsg.count(),
            (double)outbyte.count(),
        });
    }
}

TEST_CASE("Mesh nodes vs network traffic", "[scalability][!hide]")
{
    netTopologyTest("mesh", [&](int numNodes) -> Simulation::pointer {
        return Topologies::core(
            numNodes, 1.0, Simulation::OVER_LOOPBACK,
            sha256(fmt::format("nodes-{:d}", numNodes)),
            [&](int cfgNum) -> Config {
                Config res = getTestConfig(cfgNum);
                res.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
                res.MAX_PEER_CONNECTIONS = 1000;
                return res;
            });
    });
}

TEST_CASE("Cycle nodes vs network traffic", "[scalability][!hide]")
{
    netTopologyTest("cycle", [&](int numNodes) -> Simulation::pointer {
        return Topologies::cycle(
            numNodes, 1.0, Simulation::OVER_LOOPBACK,
            sha256(fmt::format("nodes-{:d}", numNodes)),
            [](int cfgCount) -> Config {
                Config res = getTestConfig(cfgCount);
                res.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
                res.MAX_PEER_CONNECTIONS = 1000;
                return res;
            });
    });
}

TEST_CASE("Branched cycle nodes vs network traffic", "[scalability][!hide]")
{
    netTopologyTest("branchedcycle", [&](int numNodes) -> Simulation::pointer {
        return Topologies::branchedcycle(
            numNodes, 1.0, Simulation::OVER_LOOPBACK,
            sha256(fmt::format("nodes-{:d}", numNodes)),
            [](int cfgCount) -> Config {
                Config res = getTestConfig(cfgCount);
                res.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
                res.MAX_PEER_CONNECTIONS = 1000;
                return res;
            });
    });
}

TEST_CASE("Bucket list entries vs write throughput", "[scalability][!hide]")
{
    VirtualClock clock;
    Config const& cfg = getTestConfig();

    Application::pointer app = Application::create(clock, cfg);
    autocheck::generator<std::vector<LedgerKey>> deadGen;

    auto& obj =
        app->getMetrics().NewMeter({"bucket", "object", "insert"}, "object");
    auto& batch = app->getMetrics().NewTimer({"bucket", "batch", "add"});
    auto& byte =
        app->getMetrics().NewMeter({"bucket", "byte", "insert"}, "byte");
    auto& merges = app->getMetrics().NewTimer({"bucket", "snap", "merge"});

    ScaleReporter r({"bucketobjs", "bytes", "objrate", "byterate",
                     "batchlatency99", "batchlatencymax", "merges",
                     "mergelatencymax", "mergelatencymean"});

    for (uint32_t i = 1;
         !app->getClock().getIOService().stopped() && i < 0x200000; ++i)
    {
        app->getClock().crank(false);
        app->getBucketManager().addBatch(
            *app, i, LedgerTestUtils::generateValidLedgerEntries(100),
            deadGen(5));

        if ((i & 0xff) == 0xff)
        {
            r.write({(double)obj.count(), (double)byte.count(),
                     obj.one_minute_rate(), byte.one_minute_rate(),
                     batch.GetSnapshot().get99thPercentile(), batch.max(),
                     (double)merges.count(), merges.max(), merges.mean()});

            app->getBucketManager().forgetUnreferencedBuckets();
        }
    }
}
