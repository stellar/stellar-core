// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/Curve25519.h"
#include "lib/catch.hpp"
#include "main/CommandHandler.h"
#include "overlay/OverlayManager.h"
#include "overlay/SurveyDataManager.h"
#include "overlay/SurveyManager.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"

using namespace std::chrono_literals;
using namespace stellar;

namespace
{
// Begin survey collecting from `node`
void
startSurveyCollecting(Application& node, uint32_t nonce)
{
    std::string const cmd =
        "startsurveycollecting?nonce=" + std::to_string(nonce);
    node.getCommandHandler().manualCmd(cmd);
}

// Stop survey collecting from `node`
void
stopSurveyCollecting(Application& node, uint32_t nonce)
{
    std::string const cmd = "stopsurveycollecting";
    node.getCommandHandler().manualCmd(cmd);
}

// Request survey data from `surveyed`. Returns `true` iff the request succeeded
// in adding `surveyed` to the backlog.
bool
surveyTopologyTimeSliced(Application& surveyor, PublicKey const& surveyed,
                         uint32_t inboundPeerIndex, uint32_t outboundPeerIndex)
{
    std::string const cmd =
        "surveytopologytimesliced?node=" + KeyUtils::toStrKey(surveyed) +
        "&inboundpeerindex=" + std::to_string(inboundPeerIndex) +
        "&outboundpeerindex=" + std::to_string(outboundPeerIndex);
    std::string const response = surveyor.getCommandHandler().manualCmd(cmd);

    // Detect failure by looking for the word "failed" in the response
    return response.find("failed") == std::string::npos;
}

// Get survey results from `node`
Json::Value
getSurveyResult(Application& node)
{
    auto const strResult =
        node.getCommandHandler().manualCmd("getsurveyresult");
    Json::Value result;
    Json::Reader reader;
    REQUIRE(reader.parse(strResult, result));
    return result;
}

// Shared setup function for tests with 5 node unchanging network topology
std::shared_ptr<Simulation>
setupStaticNetworkTopology(std::vector<Config>& configList,
                           std::vector<PublicKey>& keyList,
                           std::vector<std::string>& keyStrList)
{
    enum
    {
        A,
        B,
        C,
        D, // not in transitive quorum
        E
    };

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    for (int i = A; i <= E; ++i)
    {
        auto cfg = simulation->newConfig();
        configList.emplace_back(cfg);

        keyList.emplace_back(cfg.NODE_SEED.getPublicKey());
        keyStrList.emplace_back(cfg.NODE_SEED.getStrKeyPublic());
    }

    // B will only respond to/relay messages from A and D
    configList[B].SURVEYOR_KEYS.emplace(keyList[A]);
    configList[B].SURVEYOR_KEYS.emplace(keyList[E]);

    // Note that peer D is in SURVEYOR_KEYS of A and B, but is not in transitive
    // quorum, meaning that it's request messages will be dropped by relay nodes
    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(keyList[A]);
    qSet.validators.push_back(keyList[C]);

    for (int i = A; i <= E; ++i)
    {
        auto const& cfg = configList[i];
        simulation->addNode(cfg.NODE_SEED, qSet, &cfg);
    }

    // D->A->B->C B->E
    simulation->addPendingConnection(keyList[D], keyList[A]);
    simulation->addPendingConnection(keyList[A], keyList[B]);
    simulation->addPendingConnection(keyList[B], keyList[C]);
    simulation->addPendingConnection(keyList[B], keyList[E]);

    simulation->startAllNodes();

    // wait for ledgers to close so nodes get the updated transitive quorum
    int nLedgers = 1;
    simulation->crankUntil(
        [&simulation, nLedgers]() {
            return simulation->haveAllExternalized(nLedgers + 1, 1);
        },
        2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    REQUIRE(simulation->haveAllExternalized(nLedgers + 1, 1));
    return simulation;
}

} // namespace

TEST_CASE("topology encrypted response memory check",
          "[overlay][survey][topology]")
{
    SurveyResponseBody body;

    // Test that `body` is within memory limits by attempting to encrypt it
    auto doEncryptTest = [&](auto const& body) {
        auto publicKey = curve25519DerivePublic(curve25519RandomSecret());
        // this will throw if EncryptedBody is too small
        curve25519Encrypt<EncryptedBody::max_size()>(publicKey,
                                                     xdr::xdr_to_opaque(body));
    };

    auto doOldStyleTest = [&](auto& body) {
        // Fill up the PeerStatLists
        for (uint32_t i = 0; i < PeerStatList::max_size(); ++i)
        {
            PeerStats s;
            s.versionStr = std::string(s.versionStr.max_size(), 'a');
            body.inboundPeers.push_back(s);
            body.outboundPeers.push_back(s);
        }

        doEncryptTest(body);
    };

    SECTION("V0")
    {
        body.type(SURVEY_TOPOLOGY_RESPONSE_V0);
        auto& topologyBody = body.topologyResponseBodyV0();
        doOldStyleTest(topologyBody);
    }
    SECTION("V1")
    {
        body.type(SURVEY_TOPOLOGY_RESPONSE_V1);
        auto& topologyBody = body.topologyResponseBodyV1();
        doOldStyleTest(topologyBody);
    }
    SECTION("V2")
    {
        body.type(SURVEY_TOPOLOGY_RESPONSE_V2);
        auto& topologyBody = body.topologyResponseBodyV2();

        // Fill up the TimeSlicedPeerDataLists
        for (uint32_t i = 0; i < TimeSlicedPeerDataList::max_size(); ++i)
        {
            TimeSlicedPeerData pd;
            pd.peerStats.versionStr =
                std::string(pd.peerStats.versionStr.max_size(), 'a');
            topologyBody.inboundPeers.push_back(pd);
            topologyBody.outboundPeers.push_back(pd);
        }

        doEncryptTest(topologyBody);
    }
}

TEST_CASE("topology survey", "[overlay][survey][topology]")
{
    enum
    {
        A,
        B,
        C,
        D, // not in transitive quorum
        E
    };
    std::vector<Config> configList;
    std::vector<PublicKey> keyList;
    std::vector<std::string> keyStrList;
    std::shared_ptr<Simulation> simulation =
        setupStaticNetworkTopology(configList, keyList, keyStrList);

    auto getResults = [&](NodeID const& nodeID) {
        simulation->crankForAtLeast(std::chrono::seconds(1), false);
        auto strResult =
            simulation->getNode(nodeID)->getCommandHandler().manualCmd(
                "getsurveyresult");

        Json::Value result;
        Json::Reader reader;
        REQUIRE(reader.parse(strResult, result));
        return result;
    };

    auto sendRequest = [&](PublicKey const& surveyor,
                           PublicKey const& surveyed) {
        std::string topologyCmd = "surveytopology?duration=100&node=";
        simulation->getNode(surveyor)->getCommandHandler().manualCmd(
            topologyCmd + KeyUtils::toStrKey(surveyed));
    };

    auto crankForSurvey = [&]() {
        simulation->crankForAtLeast(
            configList[A].getExpectedLedgerCloseTime() *
                SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT,
            false);
    };

    SECTION("5 normal nodes (A->B->C B->E)")
    {
        sendRequest(keyList[A], keyList[B]);
        crankForSurvey();

        auto result = getResults(keyList[A]);
        Json::Value topology = result["topology"];

        REQUIRE(topology.size() == 1);

        // B responds with 2 new nodes (C and E)
        REQUIRE(topology[keyStrList[B]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[A]);

        std::set<std::string> expectedOutboundPeers = {keyStrList[E],
                                                       keyStrList[C]};
        std::set<std::string> actualOutboundPeers = {
            topology[keyStrList[B]]["outboundPeers"][0]["nodeId"].asString(),
            topology[keyStrList[B]]["outboundPeers"][1]["nodeId"].asString()};

        REQUIRE(expectedOutboundPeers == actualOutboundPeers);

        // Peer counts are correct
        REQUIRE(topology[keyStrList[B]]["numTotalInboundPeers"].asUInt64() ==
                1);
        REQUIRE(topology[keyStrList[B]]["numTotalOutboundPeers"].asUInt64() ==
                expectedOutboundPeers.size());
        REQUIRE(topology[keyStrList[B]]["maxInboundPeerCount"].asUInt64() ==
                simulation->getNode(keyList[B])
                    ->getConfig()
                    .MAX_ADDITIONAL_PEER_CONNECTIONS);
        REQUIRE(topology[keyStrList[B]]["maxOutboundPeerCount"].asUInt64() ==
                simulation->getNode(keyList[B])
                    ->getConfig()
                    .TARGET_PEER_CONNECTIONS);

        sendRequest(keyList[A], keyList[C]);
        sendRequest(keyList[A], keyList[E]);

        crankForSurvey();

        result = getResults(keyList[A]);
        topology = result["topology"];

        // In the next round, we sent requests to C and E
        REQUIRE(topology.size() == 3);
        REQUIRE(topology[keyStrList[C]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[B]);
        REQUIRE(topology[keyStrList[C]]["outboundPeers"].isNull());

        REQUIRE(topology[keyStrList[E]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[B]);
        REQUIRE(topology[keyStrList[E]]["outboundPeers"].isNull());
    }

    SECTION("D is not in transitive quorum, so A doesn't respond or relay to B"
            "(D-/>A-/>B)")
    {
        sendRequest(keyList[D], keyList[A]);
        sendRequest(keyList[D], keyList[B]);

        // move time forward so next round of queries can go. requests should be
        // sent, but nodes shouldn't respond
        crankForSurvey();

        auto result = getResults(keyList[D]);
        Json::Value const& topology = result["topology"];

        REQUIRE(topology.size() == 2);
        for (auto const& value : topology)
        {
            REQUIRE(value.isNull());
        }
    }

    SECTION("B does not have C in SURVEYOR_KEYS, so B doesn't respond or relay "
            "to A (C-/>B-/>A)")
    {
        sendRequest(keyList[C], keyList[B]);
        sendRequest(keyList[C], keyList[A]);

        // move time forward so next round of queries can go
        crankForSurvey();

        auto result = getResults(keyList[C]);
        Json::Value const& topology = result["topology"];

        REQUIRE(topology.size() == 2);
        for (auto const& value : topology)
        {
            REQUIRE(value.isNull());
        }
    }
    SECTION("A (surveyor) filters out unknown responses")
    {
        auto getSM = [&](NodeID const& key) -> auto&
        {
            return simulation->getNode(key)
                ->getOverlayManager()
                .getSurveyManager();
        };

        // A sends survey request to B
        sendRequest(keyList[A], keyList[B]);

        // D responds to A's request, even though A did not ask
        // Create a fake request so that D can respond
        auto request = getSM(keyList[A]).makeOldStyleSurveyRequest(keyList[D]);
        auto peers = simulation->getNode(keyList[D])
                         ->getOverlayManager()
                         .getOutboundAuthenticatedPeers();
        REQUIRE(peers.find(keyList[A]) != peers.end());
        getSM(keyList[D]).relayOrProcessRequest(request, peers[keyList[A]]);

        crankForSurvey();
        auto result = getResults(keyList[A]);
        Json::Value topology = result["topology"];

        // A receives a response from D, but it gets filtered out
        // Result only contains response from B
        REQUIRE(topology.size() == 1);
        REQUIRE(topology.isMember(KeyUtils::toStrKey(keyList[B])));
    }

    simulation->stopAllNodes();
}

TEST_CASE("survey request process order", "[overlay][survey][topology]")
{
    // An arbitrary number reasonably larger than MAX_REQUEST_LIMIT_PER_LEDGER.
    int numberOfNodes = 20;

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    std::vector<Config> configList;
    std::vector<PublicKey> keyList;
    std::vector<std::string> keyStrList;
    std::map<std::string, int> keyToIndex;
    for (int i = 0; i < numberOfNodes; i++)
    {
        auto cfg = simulation->newConfig();
        configList.emplace_back(cfg);

        keyList.emplace_back(cfg.NODE_SEED.getPublicKey());
        keyStrList.emplace_back(cfg.NODE_SEED.getStrKeyPublic());
        keyToIndex[cfg.NODE_SEED.getStrKeyPublic()] = i;
    }

    // Construct a highly connected network with numberOfNodes nodes.
    for (int i = 0; i < numberOfNodes; i++)
    {
        configList[0].SURVEYOR_KEYS.emplace(keyList[i]);
    }

    SCPQuorumSet qSet;
    for (int i = 0; i < numberOfNodes; i++)
    {
        qSet.validators.push_back(keyList[i]);
    }
    qSet.threshold = 2 * numberOfNodes / 3;

    for (int i = 0; i < numberOfNodes; i++)
    {
        auto const& cfg = configList[i];
        simulation->addNode(cfg.NODE_SEED, qSet, &cfg);
    }

    for (int i = 0; i < numberOfNodes; i++)
    {
        for (int j = i + 1; j < numberOfNodes; j++)
        {
            simulation->addPendingConnection(keyList[i], keyList[j]);
        }
    }

    simulation->startAllNodes();

    // wait for ledgers to close so nodes get the updated transitive quorum
    int nLedgers = 1;
    simulation->crankUntil(
        [&simulation, nLedgers]() {
            return simulation->haveAllExternalized(nLedgers + 1, 1);
        },
        2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    REQUIRE(simulation->haveAllExternalized(nLedgers + 1, 1));

    auto getResults = [&](NodeID const& nodeID) {
        simulation->crankForAtLeast(std::chrono::seconds(1), false);
        Application& node = *simulation->getNode(nodeID);
        return getSurveyResult(node);
    };

    auto sendRequest = [&](PublicKey const& surveyor,
                           PublicKey const& surveyed) {
        std::string topologyCmd = "surveytopology?duration=100&node=";
        simulation->getNode(surveyor)->getCommandHandler().manualCmd(
            topologyCmd + KeyUtils::toStrKey(surveyed));
    };

    auto crankForSurvey = [&]() {
        simulation->crankForAtLeast(
            configList[0].getExpectedLedgerCloseTime() *
                SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT,
            false);
    };

    SECTION("request processed fifo")
    {
        // Request node 0 to survey 1, 2, ..., (numberOfNodes - 1),
        // and the requests should be processed in that order.
        for (int i = 1; i < numberOfNodes; i++)
        {
            sendRequest(keyList[0], keyList[i]);
        }

        for (int t = 0; t < 2; t++)
        {
            auto result = getResults(keyList[0]);
            std::set<int> backlog;

            // Check if all the indices of processed requests
            // are smaller than all the indices of requests
            // that haven't been processed.
            int largestProcessed = -1;
            int smallestInBacklog = numberOfNodes;
            for (auto const& key : result["backlog"])
            {
                int index = keyToIndex[key.asString()];
                backlog.insert(index);
                smallestInBacklog = std::min(smallestInBacklog, index);
            }
            for (int i = numberOfNodes - 1; i >= 0; i--)
            {
                if (backlog.find(i) == backlog.end())
                {
                    largestProcessed = i;
                    break;
                }
            }
            REQUIRE(largestProcessed < smallestInBacklog);
            crankForSurvey();
        }
    }
    simulation->stopAllNodes();
}

TEST_CASE("Time sliced static topology survey", "[overlay][survey][topology]")
{
    enum
    {
        A,
        B,
        C,
        D, // not in transitive quorum
        E
    };
    std::vector<Config> configList;
    std::vector<PublicKey> keyList;
    std::vector<std::string> keyStrList;
    std::shared_ptr<Simulation> simulation =
        setupStaticNetworkTopology(configList, keyList, keyStrList);

    auto crankForSurvey = [&]() {
        simulation->crankForAtLeast(
            configList[A].getExpectedLedgerCloseTime() *
                SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT * 2,
            false);
    };

    // A network survey with no topology changes throughout
    uint32_t constexpr nonce = 0xDEADBEEF;

    // Check that all nodes have the same survey nonce and phase. Set
    // `isReporting` to `true` if the nodes should be in the reporting phase.
    auto checkSurveyState = [&](bool isReporting) {
        for (int i = A; i <= E; ++i)
        {
            auto& surveyDataManager = simulation->getNode(keyList[i])
                                          ->getOverlayManager()
                                          .getSurveyManager()
                                          .getSurveyDataManagerForTesting();
            REQUIRE(surveyDataManager.surveyIsActive());
            REQUIRE(surveyDataManager.getNonce().value() == nonce);
            REQUIRE(surveyDataManager.nonceIsReporting(nonce) == isReporting);
        }
    };

    SECTION("Normal static topology survey")
    {
        // Start survey collecting
        Application& surveyor = *simulation->getNode(keyList[A]);
        startSurveyCollecting(surveyor, nonce);

        // Let survey run for a bit
        simulation->crankForAtLeast(5min, false);

        // All nodes should have active surveys
        checkSurveyState(false);

        // Stop survey collecting
        stopSurveyCollecting(surveyor, nonce);

        // Give the network time to transition to the reporting phase
        simulation->crankForAtLeast(1min, false);

        // All nodes should still have active surveys. All should be in
        // reporting mode
        checkSurveyState(true);

        // Request survey data from B
        REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[B], 0, 0));
        crankForSurvey();

        // Check results
        Json::Value topology = getSurveyResult(surveyor)["topology"];
        REQUIRE(topology.size() == 1);

        // B responds with 2 new nodes (C and E)
        REQUIRE(topology[keyStrList[B]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[A]);

        std::set<std::string> expectedOutboundPeers = {keyStrList[E],
                                                       keyStrList[C]};
        std::set<std::string> actualOutboundPeers = {
            topology[keyStrList[B]]["outboundPeers"][0]["nodeId"].asString(),
            topology[keyStrList[B]]["outboundPeers"][1]["nodeId"].asString()};

        REQUIRE(expectedOutboundPeers == actualOutboundPeers);

        // Peer counts are correct
        REQUIRE(topology[keyStrList[B]]["numTotalInboundPeers"].asUInt64() ==
                1);
        REQUIRE(topology[keyStrList[B]]["numTotalOutboundPeers"].asUInt64() ==
                expectedOutboundPeers.size());
        REQUIRE(topology[keyStrList[B]]["maxInboundPeerCount"].asUInt64() ==
                simulation->getNode(keyList[B])
                    ->getConfig()
                    .MAX_ADDITIONAL_PEER_CONNECTIONS);
        REQUIRE(topology[keyStrList[B]]["maxOutboundPeerCount"].asUInt64() ==
                simulation->getNode(keyList[B])
                    ->getConfig()
                    .TARGET_PEER_CONNECTIONS);
        REQUIRE(topology[keyStrList[B]]["addedAuthenticatedPeers"].asUInt() ==
                0);
        REQUIRE(topology[keyStrList[B]]["droppedAuthenticatedPeers"].asUInt() ==
                0);

        // Validator check is correct
        REQUIRE(topology[keyStrList[B]]["isValidator"].asBool() ==
                configList[B].NODE_IS_VALIDATOR);

        // Request survey data from C and E
        REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[C], 0, 0));
        REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[E], 0, 0));
        crankForSurvey();

        // In the next round, we sent requests to C and E
        topology = getSurveyResult(surveyor)["topology"];
        REQUIRE(topology.size() == 3);
        REQUIRE(topology[keyStrList[C]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[B]);
        REQUIRE(topology[keyStrList[C]]["outboundPeers"].isNull());

        REQUIRE(topology[keyStrList[E]]["inboundPeers"][0]["nodeId"] ==
                keyStrList[B]);
        REQUIRE(topology[keyStrList[E]]["outboundPeers"].isNull());

        // Request survey data from B with non-zero peer indices.
        REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[B], 1, 1));
        crankForSurvey();
        topology = getSurveyResult(surveyor)["topology"];
        REQUIRE(topology.size() == 3);
        // Should have no inbound peers (requested index was too high)
        REQUIRE(topology[keyStrList[B]]["inboundPeers"].isNull());
        // Should have just 1 outbound peer
        REQUIRE(topology[keyStrList[B]]["outboundPeers"].size() == 1);

        // Request survey data from B twice. The second call (with different
        // indices) should fail.
        REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[B], 0, 0));
        REQUIRE(!surveyTopologyTimeSliced(surveyor, keyList[B], 1, 1));
        crankForSurvey();
        topology = getSurveyResult(surveyor)["topology"];
        REQUIRE(topology.size() == 3);
        // Should have 1 inbound peer and 2 outbound peers, indicating that the
        // survey with 0 indices went through
        REQUIRE(topology[keyStrList[B]]["inboundPeers"].size() == 1);
        REQUIRE(topology[keyStrList[B]]["outboundPeers"].size() == 2);

        // Start a new survey collection with a different nonce from node B.
        // Call should fail as B should detect the already running survey.
        uint32_t constexpr conflictingNonce = 0xCAFE;
        startSurveyCollecting(*simulation->getNode(keyList[B]),
                              conflictingNonce);

        // Let survey run (though it shouldn't matter as B shouldn't even
        // generate a message to send)
        crankForSurvey();

        // Check that all nodes still have the old nonce
        checkSurveyState(true);

        // Reduce phase durations
        std::chrono::minutes constexpr phaseDuration = 1min;
        for (int i = A; i <= E; ++i)
        {
            simulation->getNode(keyList[i])
                ->getOverlayManager()
                .getSurveyManager()
                .getSurveyDataManagerForTesting()
                .setPhaseMaxDurationsForTesting(phaseDuration);
        }

        // Advance survey
        simulation->crankForAtLeast(phaseDuration, false);

        // All surveys should now be inactive
        for (int i = A; i <= E; ++i)
        {
            auto& surveyDataManager = simulation->getNode(keyList[i])
                                          ->getOverlayManager()
                                          .getSurveyManager()
                                          .getSurveyDataManagerForTesting();
            REQUIRE(!surveyDataManager.surveyIsActive());
            REQUIRE(!surveyDataManager.getNonce().has_value());
        }

        // Start new survey collecting phase
        startSurveyCollecting(surveyor, nonce);
        crankForSurvey();

        // All nodes should have active surveys
        checkSurveyState(false);

        // Wait for collecting phase to time out
        simulation->crankForAtLeast(phaseDuration, false);

        // Surveys should have automatically transitioned to reporting phase
        for (int i = A; i <= E; ++i)
        {
            auto& surveyDataManager = simulation->getNode(keyList[i])
                                          ->getOverlayManager()
                                          .getSurveyManager()
                                          .getSurveyDataManagerForTesting();
            REQUIRE(surveyDataManager.nonceIsReporting(nonce));
        }
    }
}

// A time sliced survey with changing topology during the collecting phase
TEST_CASE("Time sliced dynamic topology survey", "[overlay][survey][topology]")
{
    enum
    {
        A,
        B,
        C,
        D, // not in transitive quorum
        E, // will disconnect partway through test
        F  // not initially connected
    };

    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    std::vector<Config> configList;
    std::vector<PublicKey> keyList;
    std::vector<std::string> keyStrList;
    for (int i = A; i <= F; ++i)
    {
        auto cfg = simulation->newConfig();
        configList.emplace_back(cfg);

        keyList.emplace_back(cfg.NODE_SEED.getPublicKey());
        keyStrList.emplace_back(cfg.NODE_SEED.getStrKeyPublic());
    }

    // B will only respond to/relay messages from A, D, and F
    configList[B].SURVEYOR_KEYS.emplace(keyList[A]);
    configList[B].SURVEYOR_KEYS.emplace(keyList[E]);
    configList[B].SURVEYOR_KEYS.emplace(keyList[F]);

    // Note that peer D is in SURVEYOR_KEYS of A and B, but is not in transitive
    // quorum, meaning that it's request messages will be dropped by relay nodes
    SCPQuorumSet qSet;
    qSet.threshold = 2;
    qSet.validators.push_back(keyList[A]);
    qSet.validators.push_back(keyList[C]);
    qSet.validators.push_back(keyList[F]);

    // Add all nodes but F to the simulation
    for (int i = A; i <= E; ++i)
    {
        auto const& cfg = configList[i];
        simulation->addNode(cfg.NODE_SEED, qSet, &cfg);
    }

    // D->A->B->C B->E (F not connected)
    simulation->addPendingConnection(keyList[D], keyList[A]);
    simulation->addPendingConnection(keyList[A], keyList[B]);
    simulation->addPendingConnection(keyList[B], keyList[C]);
    simulation->addPendingConnection(keyList[B], keyList[E]);

    simulation->startAllNodes();

    // wait for ledgers to close so nodes get the updated transitive quorum
    int nLedgers = 1;
    simulation->crankUntil(
        [&simulation, nLedgers]() {
            return simulation->haveAllExternalized(nLedgers + 1, 1);
        },
        2 * nLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS, false);

    REQUIRE(simulation->haveAllExternalized(nLedgers + 1, 1));

    auto crankForSurvey = [&]() {
        simulation->crankForAtLeast(
            configList[A].getExpectedLedgerCloseTime() *
                SurveyManager::SURVEY_THROTTLE_TIMEOUT_MULT * 2,
            false);
    };

    uint32_t constexpr nonce = 0xDEADBEEF;

    // Check that all nodes in `indices` have the same survey nonce and phase.
    // Set `isReporting` to `true` if the nodes should be in the reporting
    // phase.
    auto checkSurveyState = [&](std::optional<uint32_t> expectedNonce,
                                bool isReporting,
                                std::vector<size_t> const& indices) {
        for (size_t i : indices)
        {
            auto& surveyDataManager = simulation->getNode(keyList[i])
                                          ->getOverlayManager()
                                          .getSurveyManager()
                                          .getSurveyDataManagerForTesting();
            REQUIRE(surveyDataManager.surveyIsActive() ==
                    expectedNonce.has_value());
            REQUIRE(surveyDataManager.nonceIsReporting(nonce) == isReporting);
            REQUIRE(surveyDataManager.getNonce() == expectedNonce);
        }
    };

    // Start survey collection from A
    Application& surveyor = *simulation->getNode(keyList[A]);
    startSurveyCollecting(surveyor, nonce);
    crankForSurvey();

    // A through E should all be in the collecting phase
    checkSurveyState(nonce, /*isReporting*/ false, {A, B, C, D, E});

    // Add F to the simulation and connect to B
    simulation->addNode(configList.at(F).NODE_SEED, qSet, &configList.at(F))
        ->start();
    simulation->addConnection(keyList[F], keyList[B]);

    // Let survey run for a bit to establish connection
    crankForSurvey();

    // Disconnect E from B
    simulation->dropConnection(keyList[B], keyList[E]);

    // Let survey run for a bit
    crankForSurvey();

    // A through E should all still be in the collecting phase
    checkSurveyState(nonce, /*isReporting*/ false, {A, B, C, D, E});

    // F should not be aware of the survey
    checkSurveyState(/*expectedNonce*/ std::nullopt, /*isReporting*/ false,
                     {F});

    // Stop survey collecting
    stopSurveyCollecting(surveyor, nonce);
    crankForSurvey();

    // A through D should be in the reporting phase
    checkSurveyState(nonce, /*isReporting*/ true, {A, B, C, D});

    // E should remain in the collecting phase
    checkSurveyState(nonce, /*isReporting*/ false, {E});

    // F's survey state should remain inactive
    checkSurveyState(/*expectedNonce*/ std::nullopt, /*isReporting*/ false,
                     {F});

    // Reconnect E
    simulation->addConnection(keyList[B], keyList[E]);
    crankForSurvey();

    // Survey states should be unchanged
    checkSurveyState(nonce, /*isReporting*/ true, {A, B, C, D});
    checkSurveyState(nonce, /*isReporting*/ false, {E});
    checkSurveyState(/*expectedNonce*/ std::nullopt, /*isReporting*/ false,
                     {F});

    // Request survey data from B, E, and F
    REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[B], 0, 0));
    REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[E], 0, 0));
    REQUIRE(surveyTopologyTimeSliced(surveyor, keyList[F], 0, 0));
    crankForSurvey();

    // Check results
    Json::Value topology = getSurveyResult(surveyor)["topology"];
    REQUIRE(topology.size() == 3);

    // B has 1 inbound peer active for entire time slice (A)
    REQUIRE(topology[keyStrList[B]]["numTotalInboundPeers"].asUInt64() == 1);
    REQUIRE(topology[keyStrList[B]]["inboundPeers"][0]["nodeId"] ==
            keyStrList[A]);

    // B only has 1 outbound peer active for entire time slice (C)
    REQUIRE(topology[keyStrList[B]]["numTotalOutboundPeers"].asUInt64() == 1);
    REQUIRE(topology[keyStrList[B]]["outboundPeers"][0]["nodeId"] ==
            keyStrList[C]);

    // B has 1 dropped peer (E)
    REQUIRE(topology[keyStrList[B]]["droppedAuthenticatedPeers"].asUInt() == 1);

    // B has 1 added peer (F). E does not count as it reconnected after the
    // end of the collecting phase.
    REQUIRE(topology[keyStrList[B]]["addedAuthenticatedPeers"].asUInt() == 1);

    // E does not respond as it missed the stop survey collecting message and
    // remains in the collecting phase
    REQUIRE(topology[keyStrList[E]].isNull());

    // F does not respond as it did not receive the start survey collecting
    // message
    REQUIRE(topology[keyStrList[F]].isNull());

    // F tries to start a new survey. Unlike the static topology test, F will
    // broadcast the request as it does already have an active survey itself.
    // All other nodes should ignore the request.
    uint32_t constexpr conflictingNonce = 0xCAFE;
    startSurveyCollecting(*simulation->getNode(keyList[F]), conflictingNonce);
    crankForSurvey();

    // Nodes A through D should still be in the reporting phase with the old
    // nonce
    checkSurveyState(nonce, /*isReporting*/ true, {A, B, C, D});

    // Node E should still be in collecting phase with the old nonce
    checkSurveyState(nonce, /*isReporting*/ false, {E});

    // Node F should be in the collecting phase with the new nonce
    checkSurveyState(conflictingNonce, /*isReporting*/ false, {F});

    // Reduce phase durations
    std::chrono::minutes constexpr phaseDuration = 1min;
    for (int i = A; i <= F; ++i)
    {
        simulation->getNode(keyList[i])
            ->getOverlayManager()
            .getSurveyManager()
            .getSurveyDataManagerForTesting()
            .setPhaseMaxDurationsForTesting(phaseDuration);
    }

    // Advance survey
    simulation->crankForAtLeast(phaseDuration * 2, false);

    // All surveys should now be inactive
    checkSurveyState(/*expectedNonce*/ std::nullopt, /*isReporting*/ false,
                     {A, B, C, D, E, F});
}