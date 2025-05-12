// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "lib/catch.hpp"
#include "main/ApplicationImpl.h"
#include "medida/metrics_registry.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "overlay/Tracker.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/Simulation.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "xdr/Stellar-types.h"

namespace stellar
{

namespace
{

class HerderStub : public HerderImpl
{
  public:
    HerderStub(Application& app)
        : HerderImpl(app, [&app]() -> LedgerManager::LedgerState const& {
            return app.getLedgerManager().getLCLState();
        }){};

    std::vector<int> received;

  private:
    EnvelopeStatus
    recvSCPEnvelope(SCPEnvelope const& envelope) override
    {
        received.push_back(envelope.statement.pledges.confirm().nPrepared);
        return Herder::ENVELOPE_STATUS_PROCESSED;
    }
};

class ApplicationStub : public TestApplication
{
  public:
    ApplicationStub(VirtualClock& clock, Config const& cfg)
        : TestApplication(clock, cfg)
    {
    }

    virtual HerderStub&
    getHerder() override
    {
        auto& herder = ApplicationImpl::getHerder();
        return static_cast<HerderStub&>(herder);
    }

  private:
    virtual std::unique_ptr<Herder>
    createHerder() override
    {
        return std::make_unique<HerderStub>(*this);
    }
};

SCPEnvelope
makeEnvelope(int id)
{
    static int slotIndex{0};

    auto result = SCPEnvelope{};
    result.statement.slotIndex = ++slotIndex;
    result.statement.pledges.type(SCP_ST_CONFIRM);
    result.statement.pledges.confirm().nPrepared = id;
    return result;
}
}

TEST_CASE("ItemFetcher fetches", "[overlay][ItemFetcher]")
{
    VirtualClock clock;
    std::shared_ptr<ApplicationStub> app =
        createTestApplication<ApplicationStub>(clock, getTestConfig(0));

    std::vector<Peer::pointer> asked;
    std::vector<VirtualClock::time_point> askedTP;
    std::vector<Hash> received;
    ItemFetcher itemFetcher(*app, [&](Peer::pointer peer, Hash hash) {
        asked.emplace_back(peer);
        askedTP.emplace_back(clock.now());
        peer->sendGetQuorumSet(hash);
    });

    auto checkFetchingFor = [&itemFetcher](Hash hash,
                                           std::vector<SCPEnvelope> envelopes) {
        auto fetchingFor = itemFetcher.fetchingFor(hash);
        std::sort(std::begin(envelopes), std::end(envelopes));
        std::sort(std::begin(fetchingFor), std::end(fetchingFor));
        REQUIRE(fetchingFor == envelopes);
    };

    auto zero = sha256(ByteSlice("zero"));
    auto ten = sha256(ByteSlice("ten"));
    auto twelve = sha256(ByteSlice("twelve"));
    auto fourteen = sha256(ByteSlice("fourteen"));

    auto tenEnvelope = makeEnvelope(10);
    auto twelveEnvelope1 = makeEnvelope(12);
    auto twelveEnvelope2 = makeEnvelope(12);

    itemFetcher.fetch(ten, tenEnvelope);
    itemFetcher.fetch(twelve, twelveEnvelope1);
    itemFetcher.fetch(twelve, twelveEnvelope2);

    REQUIRE(itemFetcher.getLastSeenSlotIndex(zero) == 0);
    REQUIRE(itemFetcher.getLastSeenSlotIndex(ten) != 0);
    REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) != 0);
    REQUIRE(itemFetcher.getLastSeenSlotIndex(fourteen) == 0);

    checkFetchingFor(zero, {});
    checkFetchingFor(ten, {tenEnvelope});
    checkFetchingFor(twelve, {twelveEnvelope1, twelveEnvelope2});
    checkFetchingFor(fourteen, {});

    auto& timer = app->getMetrics().NewTimer({"overlay", "fetch", "test"});

    SECTION("stop one")
    {
        itemFetcher.stopFetch(twelve, twelveEnvelope1);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) != 0);
        checkFetchingFor(twelve, {twelveEnvelope2});

        itemFetcher.recv(twelve, timer);
        itemFetcher.recv(ten, timer);

        auto expectedReceived = std::vector<int>{12, 10};
        REQUIRE(app->getHerder().received == expectedReceived);

        REQUIRE(itemFetcher.getLastSeenSlotIndex(zero) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(ten) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(fourteen) == 0);

        checkFetchingFor(zero, {});
        checkFetchingFor(ten, {});
        checkFetchingFor(twelve, {});
        checkFetchingFor(fourteen, {});
    }

    SECTION("stop all")
    {
        itemFetcher.stopFetch(twelve, twelveEnvelope1);
        itemFetcher.stopFetch(twelve, twelveEnvelope2);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) == 0);
        checkFetchingFor(twelve, {});

        itemFetcher.recv(twelve, timer);
        itemFetcher.recv(ten, timer);

        auto expectedReceived = std::vector<int>{10};
        REQUIRE(app->getHerder().received == expectedReceived);

        REQUIRE(itemFetcher.getLastSeenSlotIndex(zero) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(ten) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(fourteen) == 0);

        checkFetchingFor(zero, {});
        checkFetchingFor(ten, {});
        checkFetchingFor(twelve, {});
        checkFetchingFor(fourteen, {});
    }

    SECTION("dont stop")
    {
        itemFetcher.recv(twelve, timer);
        itemFetcher.recv(ten, timer);

        auto expectedReceived = std::vector<int>{12, 12, 10};
        REQUIRE(app->getHerder().received == expectedReceived);

        REQUIRE(itemFetcher.getLastSeenSlotIndex(zero) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(ten) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) == 0);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(fourteen) == 0);

        checkFetchingFor(zero, {});
        checkFetchingFor(ten, {});
        checkFetchingFor(twelve, {});
        checkFetchingFor(fourteen, {});

        SECTION("no cache")
        {
            auto zeroEnvelope1 = makeEnvelope(0);
            itemFetcher.fetch(zero, zeroEnvelope1);
            itemFetcher.recv(zero, timer);

            auto zeroEnvelope2 = makeEnvelope(0);
            itemFetcher.fetch(zero, zeroEnvelope2); // no cache in current
                                                    // implementation, will
                                                    // re-fetch

            expectedReceived = std::vector<int>{12, 12, 10, 0};
            REQUIRE(app->getHerder().received == expectedReceived);

            REQUIRE(itemFetcher.getLastSeenSlotIndex(zero) != 0);
            REQUIRE(itemFetcher.getLastSeenSlotIndex(ten) == 0);
            REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) == 0);
            REQUIRE(itemFetcher.getLastSeenSlotIndex(fourteen) == 0);

            checkFetchingFor(zero, {zeroEnvelope2});
            checkFetchingFor(ten, {});
            checkFetchingFor(twelve, {});
            checkFetchingFor(fourteen, {});
        }

        SECTION("asks peers in turn")
        {
            auto other1 = createTestApplication(clock, getTestConfig(1));
            auto other2 = createTestApplication(clock, getTestConfig(2));
            LoopbackPeerConnection connection1(*app, *other1);
            auto peer1 = connection1.getInitiator();

            LoopbackPeerConnection connection2(*app, *other2);
            auto peer2 = connection2.getInitiator();

            auto waitConn = [&]() {
                // wait for peers to be setup
                while (!peer1->isAuthenticatedForTesting() ||
                       !peer2->isAuthenticatedForTesting())
                {
                    clock.crank(false);
                    clock.sleep_for(std::chrono::milliseconds(100));
                }
            };

            SECTION("success")
            {
                waitConn();

                REQUIRE(asked.size() == 0);

                SECTION("fetching once works")
                {
                    auto zeroEnvelope1 = makeEnvelope(0);
                    itemFetcher.fetch(zero, zeroEnvelope1);
                }
                SECTION(
                    "fetching twice does not trigger any additional network "
                    "activity")
                {
                    auto zeroEnvelope1 = makeEnvelope(0);
                    auto zeroEnvelope2 = makeEnvelope(0);
                    itemFetcher.fetch(zero, zeroEnvelope1);
                    itemFetcher.fetch(zero, zeroEnvelope2);
                }

                // itemFetcher asked the first peer
                REQUIRE(asked.size() == 1);

                // wait enough time that item fetcher should be asking the other
                // peer (but not too long as we don't want to retry)
                auto crankFor = [&](std::chrono::milliseconds t) {
                    auto timeout = clock.now() + t;
                    while (clock.now() < timeout)
                    {
                        clock.crank(false);
                        clock.sleep_for(std::chrono::milliseconds(500));
                    }
                };

                crankFor(Tracker::MS_TO_WAIT_FOR_FETCH_REPLY * 2);

                REQUIRE(asked.size() == 2);

                itemFetcher.recv(zero, timer);

                // crank for a while, nothing should happen now that we received
                // what we were looking for
                crankFor(std::chrono::minutes(1));

                REQUIRE(asked.size() == 2);

                REQUIRE(std::count(asked.begin(), asked.end(), peer1) == 1);
                REQUIRE(std::count(asked.begin(), asked.end(), peer2) == 1);
            }
            SECTION("not found")
            {
                auto zeroEnvelope1 = makeEnvelope(0);
                itemFetcher.fetch(zero, zeroEnvelope1);
                REQUIRE(asked.size() == 0); // no connections yet

                waitConn();

                auto testNotFound = [&](bool respond) {
                    int constexpr ITERATIONS = 100;
                    for (auto i = ITERATIONS; i != 0; --i)
                    {
                        // first, check that we're at the begining of an
                        // iteration
                        auto askCountBefore1 =
                            std::count(asked.begin(), asked.end(), peer1);
                        auto askCountBefore2 =
                            std::count(asked.begin(), asked.end(), peer2);
                        REQUIRE(askCountBefore1 == askCountBefore2);
                        size_t lastAsked = asked.size();
                        // now, crank until we've asked both peers again
                        while ((asked.size() != (lastAsked + 2)) &&
                               clock.crank(false) > 0)
                        {
                            if (respond)
                            {
                                // if a request was done, pretend the peer
                                // replied
                                if (lastAsked != asked.size())
                                {
                                    itemFetcher.doesntHave(zero, asked.back());
                                }
                            }
                            clock.sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                    REQUIRE(asked.size() == askedTP.size());
                    REQUIRE(asked.size() % 2 == 0);
                    VirtualClock::time_point prevGroup;

                    for (size_t i = 0; i < asked.size(); i += 2)
                    {
                        // check for alternation within an iteration
                        REQUIRE(asked[i] != asked[i + 1]);

                        auto refTP = askedTP[i];
                        auto delta = askedTP[i + 1] - refTP;
                        // check time when alternating between peers
                        if (respond)
                        {
                            // response should be fast
                            REQUIRE(delta < std::chrono::milliseconds(200));
                        }
                        else
                        {
                            REQUIRE(delta >=
                                    Tracker::MS_TO_WAIT_FOR_FETCH_REPLY);
                        }
                        if (i > 0)
                        {
                            auto deltaGroup = refTP - prevGroup;
                            // gap between groups depend on number of retries
                            auto nextTry =
                                Tracker::MS_TO_WAIT_FOR_FETCH_REPLY *
                                std::min(Tracker::MAX_REBUILD_FETCH_LIST,
                                         (static_cast<int>(i - 1)) / 2);
                            REQUIRE(deltaGroup >= nextTry);
                        }
                        prevGroup = askedTP[i + 1];
                    }
                };

                SECTION("peers timeout")
                {
                    testNotFound(false);
                }
                SECTION("peers actively respond not found")
                {
                    testNotFound(true);
                }
            }
            testutil::shutdownWorkScheduler(*other2);
            testutil::shutdownWorkScheduler(*other1);
            testutil::shutdownWorkScheduler(*app);
        }

        SECTION("ignore not asked items")
        {
            itemFetcher.recv(zero, timer);
            REQUIRE(app->getHerder().received ==
                    expectedReceived); // no new data received
        }
    }
}

TEST_CASE("next peer strategy", "[overlay][ItemFetcher]")
{
    auto networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto sim =
        std::make_shared<Simulation>(Simulation::OVER_LOOPBACK, networkID);

    auto cfgMain = getTestConfig(1);
    auto cfg1 = getTestConfig(2);
    auto cfg2 = getTestConfig(3);

    SIMULATION_CREATE_NODE(Main);
    SIMULATION_CREATE_NODE(Node1);
    SIMULATION_CREATE_NODE(Node2);
    sim->addNode(vMainSecretKey, cfgMain.QUORUM_SET, &cfgMain);

    sim->addNode(vNode1SecretKey, cfg1.QUORUM_SET, &cfg1);
    sim->addPendingConnection(vMainNodeID, vNode1NodeID);
    sim->startAllNodes();
    auto conn1 = sim->getLoopbackConnection(vMainNodeID, vNode1NodeID);
    auto peer1 = conn1->getInitiator();

    auto app = sim->getNode(vMainNodeID);

    int askCount = 0;
    ItemFetcher itemFetcher(*app, [&](Peer::pointer, Hash) { askCount++; });

    sim->crankUntil([&]() { return peer1->isAuthenticatedForTesting(); },
                    std::chrono::seconds{3}, false);

    // this causes to fetch from `peer1` as it's the only one
    // connected
    auto hundredEnvelope1 = makeEnvelope(100);
    auto hundred = sha256(ByteSlice("100"));
    itemFetcher.fetch(hundred, hundredEnvelope1);
    auto tracker = itemFetcher.getTracker(hundred);
    REQUIRE(tracker);
    Peer::pointer trPeer1;
    trPeer1 = tracker->getLastAskedPeer();
    REQUIRE(trPeer1 == peer1);

    REQUIRE(askCount == 1);

    SECTION("doesn't try the same peer")
    {
        tracker->tryNextPeer();
        // ran out of peers to try
        REQUIRE(!tracker->getLastAskedPeer());
        REQUIRE(askCount == 1);
    }
    SECTION("with more peers")
    {
        sim->addNode(vNode2SecretKey, cfg2.QUORUM_SET, &cfg2);
        sim->addPendingConnection(vMainNodeID, vNode2NodeID);
        sim->startAllNodes();
        auto conn2 = sim->getLoopbackConnection(vMainNodeID, vNode2NodeID);
        auto peer2 = conn2->getInitiator();

        sim->crankUntil([&]() { return peer2->isAuthenticatedForTesting(); },
                        std::chrono::seconds{3}, false);

        // still connected
        REQUIRE(peer1->isAuthenticatedForTesting());

        SECTION("try new peer")
        {
            tracker->tryNextPeer();
            REQUIRE(askCount == 2);
            auto trPeer2 = tracker->getLastAskedPeer();
            REQUIRE(trPeer2 == peer2);

            // ran out of peers
            tracker->tryNextPeer();
            REQUIRE(askCount == 2);
            REQUIRE(!tracker->getLastAskedPeer());

            // try again, this time we ask peers again

            tracker->tryNextPeer();
            REQUIRE(tracker->getLastAskedPeer());
            REQUIRE(askCount == 3);
        }
        SECTION("peer1 told us that it knows")
        {
            StellarMessage msg(SCP_MESSAGE);
            msg.envelope() = hundredEnvelope1;
            app->getOverlayManager().recvFloodedMsgID(msg, peer1,
                                                      xdrBlake2(msg));
            tracker->tryNextPeer();
            REQUIRE(askCount == 2);
            auto trPeer1b = tracker->getLastAskedPeer();
            REQUIRE(trPeer1b == peer1);

            // next time, we try a new peer
            tracker->tryNextPeer();
            REQUIRE(askCount == 3);
            auto trPeer2 = tracker->getLastAskedPeer();
            REQUIRE(trPeer2 == peer2);

            // ran out of peers
            tracker->tryNextPeer();
            REQUIRE(askCount == 3);
            REQUIRE(!tracker->getLastAskedPeer());
        }
    }
}
}
