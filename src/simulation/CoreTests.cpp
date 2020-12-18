// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketManagerImpl.h"
#include "bucket/LedgerCmp.h"
#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "medida/stats/snapshot.h"
#include "overlay/StellarXDR.h"
#include "simulation/Topologies.h"
#include "test/test.h"
#include "transactions/TransactionFrame.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/types.h"
#include "xdrpp/autocheck.h"
#include <fmt/format.h>
#include <sstream>

using namespace stellar;

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

    LOG_INFO(DEFAULT_LOG,
             "Time spent closing {} ledgers with {} nodes : {} seconds",
             nLedgers, sim->getNodes().size(), t.count());

    LOG_INFO(DEFAULT_LOG, "{}", sim->metricsSummary("scp"));
}

TEST_CASE("3 nodes 2 running threshold 2", "[simulation][core3][acceptance]")
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

        LOG_INFO(DEFAULT_LOG,
                 "#######################################################");

        simulation->startAllNodes();

        int nLedgers = 10;
        simulation->crankUntil(
            [&simulation, nLedgers]() {
                return simulation->haveAllExternalized(nLedgers + 1, 5);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        REQUIRE(simulation->haveAllExternalized(nLedgers + 1, 5));
    }
    LOG_DEBUG(DEFAULT_LOG, "done with core3 test");
}

TEST_CASE("assymetric topology report cost", "[simulation][!hide]")
{
    // Ensure we close enough ledgers to start purging slots
    // (which is when cost gets reported)
    const int nLedgers = 20;

    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation =
        Topologies::assymetric(Simulation::OVER_LOOPBACK, networkID);
    simulation->startAllNodes();

    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(nLedgers + 2, 4); },
        10 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    REQUIRE(simulation->haveAllExternalized(nLedgers, 4));

    auto checkNode = SecretKey::fromSeed(sha256("TIER_1_NODE_SEED_0"));
    auto app = simulation->getNode(checkNode.getPublicKey());

    auto lcl = app->getLedgerManager().getLastClosedLedgerNum();

    LOG_WARNING(DEFAULT_LOG, "Cost information for recent ledgers:");
    for (auto count = lcl; count > lcl - 5; count--)
    {
        auto qinfo = app->getHerder().getJsonQuorumInfo(
            checkNode.getPublicKey(), false, false, count);
        LOG_WARNING(DEFAULT_LOG, "{}", qinfo["qset"]["cost"].toStyledString());
    }
}

TEST_CASE("core topology 4 ledgers at scales 2 to 4",
          "[simulation][acceptance]")
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
        Simulation::pointer sim = Topologies::core(size, 1.0, mode, networkID);
        sim->startAllNodes();

        int nLedgers = 4;
        sim->crankUntil(
            [&sim, nLedgers]() {
                return sim->haveAllExternalized(nLedgers + 1, nLedgers);
            },
            2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

        REQUIRE(sim->haveAllExternalized(nLedgers + 1, 5));
    }
}

static void
resilienceTest(Simulation::pointer sim)
{
    auto nodes = sim->getNodeIDs();
    auto nbNodes = nodes.size();

    sim->startAllNodes();

    std::uniform_int_distribution<size_t> gen(0, nbNodes - 1);

    // bring network to a good place
    uint32 targetLedger = LedgerManager::GENESIS_LEDGER_SEQ + 1;
    const uint32 nbLedgerStep = 2;

    auto crankForward = [&](uint32 step, uint32 maxGap) {
        targetLedger += step;
        sim->crankUntil(
            [&]() { return sim->haveAllExternalized(targetLedger, maxGap); },
            5 * nbLedgerStep * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        REQUIRE(sim->haveAllExternalized(targetLedger, maxGap));
    };

    crankForward(1, 1);

    for (size_t rounds = 0; rounds < 2; rounds++)
    {
        // now restart a random node i, will reconnect to
        // j to join the network
        auto i = gen(gRandomEngine);
        auto j = (i + 1) % nbNodes;

        auto victimID = nodes[i];
        auto otherID = nodes[j];
        INFO(fmt::format("restart victim {}", i));

        targetLedger =
            sim->getNode(otherID)->getLedgerManager().getLastClosedLedgerNum();

        auto victimConfig = sim->getNode(victimID)->getConfig();
        // don't force SCP, it's just a restart
        victimConfig.FORCE_SCP = false;
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

        // check that all slots were validated
        auto herderImpl = static_cast<HerderImpl*>(&refreshedApp->getHerder());
        auto& scp = herderImpl->getSCP();
        scp.processSlotsAscendingFrom(0, [&](uint64 slot) {
            bool validated = scp.isSlotFullyValidated(slot);
            REQUIRE(validated);
            return true;
        });

        // network should be fully in sync now
        crankForward(nbLedgerStep, 1);

        // reconnect to all other peers for the next iteration
        for (size_t k = 0; k < nbNodes; k++)
        {
            if (k != i && k != j)
            {
                sim->addConnection(victimID, nodes[k]);
            }
        }
    }
}
TEST_CASE("resilience tests", "[resilience][simulation][!hide]")
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
    LOG_DEBUG(DEFAULT_LOG, "starting topo test {} : {}", nLedgers, nBranches);

    Simulation::pointer sim =
        Topologies::hierarchicalQuorum(nBranches, mode, networkID);
    sim->startAllNodes();

    sim->crankUntil(
        [&sim, nLedgers]() {
            return sim->haveAllExternalized(nLedgers + 1, 5);
        },
        20 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    REQUIRE(sim->haveAllExternalized(nLedgers + 1, 5));
}

TEST_CASE("hierarchical topology scales 1 to 3", "[simulation][acceptance]")
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
        LOG_DEBUG(DEFAULT_LOG, "OVER_LOOPBACK");
        mode = Simulation::OVER_LOOPBACK;
        test();
    }
    SECTION("Over tcp")
    {
        LOG_DEBUG(DEFAULT_LOG, "OVER_TCP");
        mode = Simulation::OVER_TCP;
        test();
    }
}

static void
hierarchicalSimplifiedTest(int nLedgers, int nbCore, int nbOuterNodes,
                           Simulation::Mode mode, Hash const& networkID)
{
    LOG_DEBUG(DEFAULT_LOG, "starting simplified test {} : {}", nLedgers,
              nbCore);

    Simulation::pointer sim = Topologies::hierarchicalQuorumSimplified(
        nbCore, nbOuterNodes, mode, networkID);
    sim->startAllNodes();

    sim->crankUntil(
        [&sim, nLedgers]() {
            return sim->haveAllExternalized(nLedgers + 1, 3);
        },
        20 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);

    REQUIRE(sim->haveAllExternalized(nLedgers + 1, 3));
}

TEST_CASE("core nodes with outer nodes", "[simulation][acceptance]")
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

    auto& loadGen = app.getLoadGenerator();
    loadGen.generateLoad(true, 3, 0, 0, 10, 100, std::chrono::seconds(0), 0);
    try
    {
        simulation->crankUntil(
            [&]() {
                // we need to wait 2 rounds in case the tx don't propagate
                // to the second node in time and the second node gets the
                // nomination
                return simulation->haveAllExternalized(5, 2) &&
                       loadGen.checkAccountSynced(app, true).empty();
            },
            3 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

        loadGen.generateLoad(false, 3, 0, 10, 10, 100, std::chrono::seconds(0),
                             0);
        simulation->crankUntil(
            [&]() {
                return simulation->haveAllExternalized(8, 2) &&
                       loadGen.checkAccountSynced(app, false).empty();
            },
            2 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, true);
    }
    catch (...)
    {
        auto problems = loadGen.checkAccountSynced(app, false);
        REQUIRE(problems.empty());
    }

    LOG_INFO(DEFAULT_LOG, "{}", simulation->metricsSummary("database"));
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
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 10000;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    Application::pointer appPtr = Application::create(clock, cfg);
    appPtr->start();
    return appPtr;
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
    {
        mOut.exceptions(std::ios::failbit | std::ios::badbit);
        mOut.open(mFilename);
        LOG_INFO(DEFAULT_LOG, "Opened {} for writing", mFilename);
        mOut << join(columns, ",") << std::endl;
    }

    ~ScaleReporter()
    {
        LOG_INFO(DEFAULT_LOG, "Wrote {} rows to {}", mNumWritten, mFilename);
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
        LOG_INFO(DEFAULT_LOG, "Writing {}", oss.str());
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

    auto& loadGen = app.getLoadGenerator();
    auto& txtime = app.getMetrics().NewTimer({"ledger", "operation", "apply"});
    uint32_t numItems = 500000;

    // Create accounts
    loadGen.generateLoad(true, numItems, 0, 0, 10, 100, std::chrono::seconds(0),
                         0);

    auto& complete =
        appPtr->getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");

    auto& io = clock.getIOContext();
    asio::io_context::work mainWork(io);
    while (!io.stopped() && complete.count() == 0)
    {
        clock.crank();
    }

    txtime.Clear();

    // Generate payment txs
    loadGen.generateLoad(false, numItems, 0, numItems / 10, 10, 100,
                         std::chrono::seconds(0), 0);
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

        auto& loadGen = app.getLoadGenerator();
        loadGen.generateLoad(true, 50, 0, 0, 10, 100, std::chrono::seconds(0),
                             0);
        auto& complete =
            app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run");

        sim->crankUntil(
            [&]() {
                return sim->haveAllExternalized(8, 2) &&
                       loadGen.checkAccountSynced(app, true).empty() &&
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
                res.TARGET_PEER_CONNECTIONS = 1000;
                res.MAX_ADDITIONAL_PEER_CONNECTIONS = 1000;
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
                res.TARGET_PEER_CONNECTIONS = 1000;
                res.MAX_ADDITIONAL_PEER_CONNECTIONS = 1000;
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
                res.TARGET_PEER_CONNECTIONS = 1000;
                res.MAX_ADDITIONAL_PEER_CONNECTIONS = 1000;
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
         !app->getClock().getIOContext().stopped() && i < 0x200000; ++i)
    {
        app->getClock().crank(false);
        app->getBucketManager().addBatch(
            *app, i, Config::CURRENT_LEDGER_PROTOCOL_VERSION,
            LedgerTestUtils::generateValidLedgerEntries(100),
            LedgerTestUtils::generateValidLedgerEntries(20), deadGen(5));

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
