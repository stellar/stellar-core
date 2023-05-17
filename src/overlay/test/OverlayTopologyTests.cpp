// Copyright 2023 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/KeyUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "overlay/OverlayManagerImpl.h"
#include "overlay/test/LoopbackPeer.h"
#include "overlay/test/OverlayTestUtils.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/test.h"
#include "util/Logging.h"

#include <fmt/format.h>
#include <numeric>

using namespace stellar;
using namespace stellar::overlaytestutils;

namespace
{

using TraverseFunc =
    std::function<void(Application& app, Application& otherApp)>;

// A basic DFS algorithm to traverse the topology
void
dfs(Application& app, std::unordered_set<NodeID>& visited, TraverseFunc f)
{
    visited.emplace(app.getConfig().NODE_SEED.getPublicKey());
    for (auto const& node : app.getOverlayManager().getAuthenticatedPeers())
    {
        if (visited.find(node.first) == visited.end())
        {
            auto& overlayApp = static_cast<ApplicationLoopbackOverlay&>(app);
            auto port = node.second->getAddress().getPort();
            auto otherApp = overlayApp.getSim().getAppFromPeerMap(port);
            if (f)
            {
                f(app, *otherApp);
            }
            dfs(*otherApp, visited, f);
        }
    }
}

bool
isConnected(int numNodes, int numWatchers, Simulation::pointer simulation)
{
    // Check if a graph is fully connected
    std::unordered_set<NodeID> visited;
    dfs(*simulation->getNodes()[0], visited, nullptr);
    return visited.size() == (numNodes + numWatchers);
}

// Log basic information about each node in the topology
void
logTopologyInfo(Simulation::pointer simulation)
{
    for (auto const& node : simulation->getNodes())
    {
        CLOG_INFO(
            Overlay,
            "Connections for node ({}) {} --> outbound {}/inbound "
            "{}, LCL={}",
            (node->getConfig().NODE_IS_VALIDATOR ? "validator" : "watcher"),
            node->getConfig().toShortString(
                node->getConfig().NODE_SEED.getPublicKey()),
            node->getOverlayManager().getOutboundAuthenticatedPeers().size(),
            node->getOverlayManager().getInboundAuthenticatedPeers().size(),
            node->getLedgerManager().getLastClosedLedgerNum());
    }
    CLOG_INFO(Overlay, "Total connections = {}",
              numberOfSimulationConnections(simulation));
}

void
populateGraphJson(Application& app, std::unordered_set<NodeID>& visited,
                  Json::Value& res)
{
    auto func = [&](Application& app, Application& otherApp) {
        auto id = KeyUtils::toStrKey(app.getConfig().NODE_SEED.getPublicKey());
        auto otherId =
            KeyUtils::toStrKey(otherApp.getConfig().NODE_SEED.getPublicKey());
        Json::Value pp;
        pp[otherId] = otherApp.getConfig().NODE_IS_VALIDATOR;
        res[id]["peers"].append(pp);
    };
    dfs(app, visited, func);
}

void
exportGraphJson(Json::Value graphJson, std::string prefix, int testID)
{
    std::string refJsonPath =
        fmt::format("src/testdata/test-{}-topology-{}-{}.json", testID, prefix,
                    std::to_string(rand_uniform(1, 100000)));

    std::ofstream outJson;
    outJson.exceptions(std::ios::failbit | std::ios::badbit);
    outJson.open(refJsonPath);

    outJson.write(graphJson.toStyledString().c_str(),
                  graphJson.toStyledString().size());
    outJson.close();
}

TEST_CASE("basic connectivity", "[overlay][connectivity][!hide]")
{
    auto test = [&](int maxOutbound, int maxInbound, int numNodes,
                    int numWatchers) {
        auto cfgs = std::vector<Config>{};
        auto peers = std::vector<std::string>{};

        for (int i = 1; i <= numNodes + numWatchers; ++i)
        {
            auto cfg = getTestConfig(i);
            cfgs.push_back(cfg);
            peers.push_back("127.0.0.1:" + std::to_string(cfg.PEER_PORT));
        }
        auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);

        // Threshold 1 means everyone must agree, graph must be connected
        auto simulation = Topologies::separate(
            numNodes, 1, Simulation::OVER_LOOPBACK, networkID, numWatchers,
            [&](int i) {
                // Ignore idle app
                if (i == 0)
                {
                    auto cfg = getTestConfig();
                    cfg.TARGET_PEER_CONNECTIONS = 0;
                    return cfg;
                }

                auto cfg = cfgs[i - 1];

                if (i > numNodes)
                {
                    cfg.NODE_IS_VALIDATOR = false;
                    cfg.FORCE_SCP = false;
                }
                cfg.TARGET_PEER_CONNECTIONS =
                    static_cast<unsigned short>(maxOutbound);
                cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = maxInbound;
                cfg.KNOWN_PEERS = peers;
                cfg.RUN_STANDALONE = false;
                return cfg;
            });

        simulation->startAllNodes();
        simulation->crankForAtLeast(std::chrono::seconds(10), false);
        logTopologyInfo(simulation);

        simulation->crankUntil(
            [&] { return simulation->haveAllExternalized(4, 1); },
            5 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        REQUIRE(isConnected(numNodes, numWatchers, simulation));
        return simulation;
    };

    int topTierSize = 23;

    SECTION("not enough capacity")
    {
        // Fail most of the time because topology is too sparse
        REQUIRE_THROWS_AS(test(1, 20, topTierSize, 100), std::runtime_error);
    }
    SECTION("small fanout sufficient to stay connected")
    {
        // 2 outbound, 2 inbound, top tier only
        test(2, 2, topTierSize, 0);
    }
    SECTION("large network is connected")
    {
        // 2 outbound, 4 inbound, 100 watchers
        test(2, 4, topTierSize, 100);
    }
}

TEST_CASE("peer churn", "[overlay][connectivity][!hide]")
{
    auto cfgs = std::vector<Config>{};
    auto peers = std::vector<std::string>{};
    int numNodes = 23;
    int numWatchers = 77;

    int maxOutbound = 2;
    int maxInbound = 4;

    std::vector<SecretKey> keys;
    SCPQuorumSet qSet;
    qSet.threshold = numNodes;
    for (int i = 0; i < numNodes + numWatchers; i++)
    {
        auto key =
            SecretKey::fromSeed(sha256("NODE_SEED_" + std::to_string(i)));
        keys.push_back(key);
        if (i < numNodes)
        {
            qSet.validators.push_back(key.getPublicKey());
        }
        auto cfg = getTestConfig(i + 1);

        cfg.NODE_IS_VALIDATOR = i < numNodes;
        cfg.FORCE_SCP = cfg.NODE_IS_VALIDATOR;
        cfg.TARGET_PEER_CONNECTIONS = static_cast<unsigned short>(maxOutbound);
        cfg.MAX_ADDITIONAL_PEER_CONNECTIONS = maxInbound;
        cfg.KNOWN_PEERS = peers;
        cfg.RUN_STANDALONE = false;
        cfg.MODE_DOES_CATCHUP = false;

        cfgs.push_back(cfg);
        peers.push_back("127.0.0.1:" + std::to_string(cfg.PEER_PORT));
    }

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    // Gradually add (randomized) peers, and ensure everyone can connect
    SECTION("add new peers")
    {
        std::vector<int> randomIndexes;
        for (int i = 0; i < keys.size(); i++)
        {
            randomIndexes.push_back(i);
        }
        stellar::shuffle(std::begin(randomIndexes), std::end(randomIndexes),
                         gRandomEngine);
        // One-by-one add a node, ensure everyone can connect
        for (int i = 0; i < randomIndexes.size(); i++)
        {
            CLOG_INFO(Overlay, "NODE {}", i);
            auto index = randomIndexes[i];
            auto newNode = simulation->addNode(keys[index], qSet, &cfgs[index]);
            newNode->start();
            simulation->crankForAtLeast(
                std::chrono::seconds(rand_uniform(0, 3)), false);
        }

        simulation->crankForAtLeast(std::chrono::seconds(15), false);
        logTopologyInfo(simulation);

        // Verify graph is connected
        REQUIRE(simulation->getNodes().size() == (numNodes + numWatchers));
        REQUIRE(isConnected(numNodes, numWatchers, simulation));
    }
    SECTION("remove peers")
    {
        for (int i = 0; i < keys.size(); i++)
        {
            simulation->addNode(keys[i], qSet, &cfgs[i]);
        }
        simulation->startAllNodes();
        simulation->crankUntil(
            [&] { return simulation->haveAllExternalized(3, 1); },
            5 * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);
        REQUIRE(isConnected(numNodes, numWatchers, simulation));

        SECTION("basic churn - remove and add")
        {
            auto otherCfgs = std::vector<Config>{};

            // Add a bit of churn by stopping and starting two random (possibly
            // same) peers
            for (int i = 0; i < 2; i++)
            {
                auto peerToChurn = rand_element(keys);
                auto cfg = simulation->getNode(peerToChurn.getPublicKey())
                               ->getConfig();
                simulation->removeNode(peerToChurn.getPublicKey());
                simulation->crankForAtLeast(std::chrono::seconds(30), false);
                auto node = simulation->addNode(cfg.NODE_SEED, qSet, &cfg);
                CLOG_INFO(Overlay, "Restart node {}",
                          node->getConfig().toShortString(
                              node->getConfig().NODE_SEED.getPublicKey()));
                node->start();
                simulation->crankForAtLeast(std::chrono::seconds(30), false);
                REQUIRE(isConnected(numNodes, numWatchers, simulation));
            }
        }
        SECTION("churn overtime")
        {
            // Note: increase iteration count to run the test for longer and
            // increase confidence in topology convergence
            int iterations = 10;
            int numPeersToChurn = 3;
            for (int i = 0; i < iterations; i++)
            {
                CLOG_INFO(Overlay, "Iteration: {}", i);

                // Crank for random amount of time
                simulation->crankForAtLeast(
                    std::chrono::seconds(rand_uniform(0, 20)), false);

                auto otherCfgs = std::vector<Config>{};
                std::vector<SecretKey> peersToChurn;
                while (peersToChurn.size() < numPeersToChurn)
                {
                    auto peerToChurn = rand_element(keys);
                    if (std::find(peersToChurn.begin(), peersToChurn.end(),
                                  peerToChurn) == peersToChurn.end())
                    {
                        peersToChurn.push_back(peerToChurn);
                    }
                }

                // Delete 3 random unique peers
                for (auto const& peerToChurn : peersToChurn)
                {
                    auto cfg = simulation->getNode(peerToChurn.getPublicKey())
                                   ->getConfig();
                    otherCfgs.push_back(cfg);
                    simulation->removeNode(peerToChurn.getPublicKey());
                }

                // Wait 60 seconds to allow the network to re-configure, then
                // restart removed nodes
                simulation->crankForAtLeast(std::chrono::seconds(60), false);
                for (int j = 0; j < peersToChurn.size(); j++)
                {
                    auto peerToChurn = peersToChurn[j];
                    auto node = simulation->addNode(otherCfgs[j].NODE_SEED,
                                                    qSet, &otherCfgs[j]);
                    CLOG_INFO(Overlay, "Restart NODE {}",
                              node->getConfig().toShortString(
                                  node->getConfig().NODE_SEED.getPublicKey()));
                    node->start();
                }

                // Allow nodes to reconnect
                simulation->crankForAtLeast(std::chrono::seconds(60), false);
                logTopologyInfo(simulation);
                REQUIRE(isConnected(numNodes, numWatchers, simulation));
            }

            // Export resulting graph to JSON file
            std::unordered_set<NodeID> visited;
            Json::Value res;
            populateGraphJson(*(simulation->getNodes()[0]), visited, res);
            auto testID = rand_uniform(0, 100000);
            exportGraphJson(res, "churn-overtime", testID);
        }
    }
}
}