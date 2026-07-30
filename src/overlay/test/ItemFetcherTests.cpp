// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/TxSetFrame.h"
#include "main/ApplicationImpl.h"
#include "overlay/ItemFetcher.h"
#include "overlay/OverlayManager.h"
#include "overlay/Tracker.h"
#include "overlay/test/LoopbackPeer.h"
#include "simulation/Simulation.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "xdr/Stellar-types.h"
#include <algorithm>
#include <optional>
#include <xdrpp/marshal.h>

namespace stellar
{

namespace
{

class HerderStub : public HerderImpl
{
  public:
    HerderStub(Application& app) : HerderImpl(app) {};

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

// Shared setup for the fetcher/claim simulation tests
struct FetchTestSim
{
    Simulation::pointer sim;
    // Main's app, and its Peer objects
    Application::pointer app;
    std::vector<std::shared_ptr<LoopbackPeer>> peers;
    // The remote ends, used to send TO Main, and the remote apps
    std::vector<std::shared_ptr<LoopbackPeer>> senders;
    std::vector<Application::pointer> remoteApps;

    // `legacyRemote` pins that remote's overlay version below
    // FIRST_OVERLAY_VERSION_SUPPORTING_HAVE_TX_SET.
    FetchTestSim(size_t numRemotes,
                 std::optional<size_t> legacyRemote = std::nullopt)
        : sim(std::make_shared<Simulation>(
              Simulation::OVER_LOOPBACK,
              sha256(getTestConfig().NETWORK_PASSPHRASE)))
        , mMainKey(SecretKey::fromSeed(sha256("NODE_SEED_MAIN")))
    {
        auto cfgMain = getTestConfig(1);
        quieten(cfgMain);
        sim->addNode(mMainKey, cfgMain.QUORUM_SET, &cfgMain);
        sim->startAllNodes();
        app = sim->getNode(mMainKey.getPublicKey());
        for (size_t i = 0; i < numRemotes; ++i)
        {
            addRemote(legacyRemote == i);
        }
    }

    // Add another remote node connected to Main, crank until authenticated,
    // and return Main's Peer object for it.
    std::shared_ptr<LoopbackPeer>
    addRemote(bool legacy = false)
    {
        size_t const i = peers.size();
        auto cfg = getTestConfig(static_cast<int>(2 + i));
        if (legacy)
        {
            cfg.OVERLAY_PROTOCOL_VERSION =
                Peer::FIRST_OVERLAY_VERSION_SUPPORTING_HAVE_TX_SET - 1;
        }
        quieten(cfg);
        auto const key = SecretKey::fromSeed(
            sha256("NODE_SEED_REMOTE_" + std::to_string(i)));
        sim->addNode(key, cfg.QUORUM_SET, &cfg);
        sim->addPendingConnection(mMainKey.getPublicKey(), key.getPublicKey());
        sim->startAllNodes();
        auto conn = sim->getLoopbackConnection(mMainKey.getPublicKey(),
                                               key.getPublicKey());
        releaseAssert(conn);
        peers.emplace_back(conn->getInitiator());
        senders.emplace_back(conn->getAcceptor());
        remoteApps.emplace_back(sim->getNode(key.getPublicKey()));
        sim->crankUntil(
            [&]() {
                return std::all_of(peers.begin(), peers.end(),
                                   [](auto const& p) {
                                       return p->isAuthenticatedForTesting();
                                   }) &&
                       std::all_of(senders.begin(), senders.end(),
                                   [](auto const& p) {
                                       return p->isAuthenticatedForTesting();
                                   });
            },
            std::chrono::seconds{3}, false);
        return peers.back();
    }

  private:
    static void
    quieten(Config& cfg)
    {
        cfg.NODE_IS_VALIDATOR = false;
        cfg.FORCE_SCP = false;
    }

    SecretKey const mMainKey;
};
}

TEST_CASE("ItemFetcher fetches", "[overlay][ItemFetcher]")
{
    VirtualClock clock;
    std::shared_ptr<ApplicationStub> app =
        createTestApplication<ApplicationStub>(clock, getTestConfig(0));

    std::vector<Peer::pointer> asked;
    std::vector<VirtualClock::time_point> askedTP;
    std::vector<Hash> received;
    ItemFetcher itemFetcher(
        *app,
        [&](Peer::pointer peer, Hash hash) {
            asked.emplace_back(peer);
            askedTP.emplace_back(clock.now());
            peer->sendGetQuorumSet(hash);
        },
        ItemFetcherKind::QuorumSet);

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

                crankFor(Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS * 2);

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
                        // first, check that we're at the beginning of an
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
                                    Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS);
                        }
                        if (i > 0)
                        {
                            auto deltaGroup = refTP - prevGroup;
                            // gap between groups depend on number of retries
                            auto nextTry =
                                Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS *
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
    FetchTestSim s(1);
    auto app = s.app;
    auto peer1 = s.peers[0];

    int askCount = 0;
    ItemFetcher itemFetcher(
        *app, [&](Peer::pointer, Hash) { askCount++; },
        ItemFetcherKind::QuorumSet);

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
        auto peer2 = s.addRemote();

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
            app->getOverlayManager().recvFloodedMsgID(peer1, xdrBlake2(msg));
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

TEST_CASE("ItemFetcher claims", "[overlay][ItemFetcher]")
{
    FetchTestSim s(2);
    auto sim = s.sim;
    auto app = s.app;
    auto peer1 = s.peers[0];
    auto peer2 = s.peers[1];

    int askCount = 0;
    ItemFetcher itemFetcher(
        *app, [&](Peer::pointer, Hash) { askCount++; }, ItemFetcherKind::TxSet);

    auto& claimAsk = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-ask"}, "item-fetcher");

    auto env = makeEnvelope(200);
    auto hash = sha256(ByteSlice("200"));

    SECTION("claim with no live tracker is a no-op")
    {
        itemFetcher.peerClaimsItem(hash, peer1);
        REQUIRE(askCount == 0);
    }

    SECTION("claim re-enables a missed peer and targets it")
    {
        itemFetcher.fetch(hash, env);
        // Ride out the claim grace period: with no claim known, the first
        // (blind) ask fires once the grace expires.
        sim->crankUntil([&]() { return askCount == 1; },
                        Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS * 2, false);
        auto tracker = itemFetcher.getTracker(hash);
        REQUIRE(tracker);
        auto first = tracker->getLastAskedPeer();
        REQUIRE(first);
        auto second = (first == peer1) ? peer2 : peer1;

        // First peer misses; the fetcher moves on to the second
        tracker->doesntHave(first);
        REQUIRE(askCount == 2);
        REQUIRE(tracker->getLastAskedPeer() == second);

        // A claim from the missed peer while an ask is outstanding is
        // recorded but does not interrupt the outstanding ask
        tracker->peerClaims(first);
        REQUIRE(askCount == 2);

        // When the second peer also misses, the claiming peer is re-asked
        // even though it was asked (and missed) before
        auto const claimAsksBefore = claimAsk.count();
        tracker->doesntHave(second);
        REQUIRE(askCount == 3);
        REQUIRE(tracker->getLastAskedPeer() == first);
        REQUIRE(claimAsk.count() == claimAsksBefore + 1);
    }

    SECTION("claim while idling acts immediately")
    {
        itemFetcher.fetch(hash, env);
        // Ride out the claim grace: with no claim known, the first (blind)
        // ask fires once the grace expires.
        sim->crankUntil([&]() { return askCount == 1; },
                        Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS * 2, false);
        auto tracker = itemFetcher.getTracker(hash);
        REQUIRE(tracker);
        auto first = tracker->getLastAskedPeer();
        auto second = (first == peer1) ? peer2 : peer1;

        // Both peers miss; with no askable candidates the tracker idles
        // waiting for a rebuild
        tracker->doesntHave(first);
        REQUIRE(askCount == 2);
        tracker->doesntHave(second);
        REQUIRE(askCount == 2);
        REQUIRE(!tracker->getLastAskedPeer());

        // A claim is acted on immediately, without waiting out the timer
        tracker->peerClaims(second);
        REQUIRE(askCount == 3);
        REQUIRE(tracker->getLastAskedPeer() == second);
    }
}

TEST_CASE("ItemFetcher claim buffer and grace period", "[overlay][ItemFetcher]")
{
    FetchTestSim s(2);
    auto app = s.app;
    auto peer1 = s.peers[0];
    auto peer2 = s.peers[1];

    int askCount = 0;
    Peer::pointer lastAsked;
    ItemFetcher itemFetcher(
        *app,
        [&](Peer::pointer p, Hash) {
            ++askCount;
            lastAsked = p;
        },
        ItemFetcherKind::TxSet);

    auto& claimAsk = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-ask"}, "item-fetcher");
#ifdef CAP_0083
    auto& graceSatisfied = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-grace-satisfied"}, "item-fetcher");
    auto& graceExpired = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-grace-expired"}, "item-fetcher");
#endif

    auto env = makeEnvelope(300);
    auto hash = sha256(ByteSlice("300"));

    SECTION("buffered claim seeds the tracker's first ask")
    {
        // Claim arrives before we are tracking the item: it must be buffered,
        // not dropped.
        itemFetcher.peerClaimsItem(hash, peer1);
        REQUIRE(askCount == 0);

        // When the tracker is created, the buffered claim seeds the claims
        // tier so the very first ask targets the claimer.
        itemFetcher.fetch(hash, env);
        REQUIRE(askCount == 1);
        REQUIRE(lastAsked == peer1);
        REQUIRE(claimAsk.count() == 1);
    }

    SECTION("repeated claims from one peer are deduplicated")
    {
        // Claims are keyed by peer: re-claiming the same hash cannot grow
        // the buffered entry, so a peer's footprint per hash is one slot.
        itemFetcher.peerClaimsItem(hash, peer1);
        itemFetcher.peerClaimsItem(hash, peer1);
        itemFetcher.peerClaimsItem(hash, peer1);
        REQUIRE(itemFetcher.getNumBufferedClaims(hash) == 1);

        // A distinct claimer occupies its own slot.
        itemFetcher.peerClaimsItem(hash, peer2);
        REQUIRE(itemFetcher.getNumBufferedClaims(hash) == 2);
        REQUIRE(askCount == 0);

        // Creating the tracker consumes the buffered claims; the first ask
        // targets one of the claimers.
        itemFetcher.fetch(hash, env);
        REQUIRE(askCount == 1);
        REQUIRE((lastAsked == peer1 || lastAsked == peer2));
        REQUIRE(claimAsk.count() == 1);
        REQUIRE(itemFetcher.getNumBufferedClaims(hash) == 0);
    }

#ifdef CAP_0083
    SECTION("grace period defers the first ask until a claim arrives")
    {
        itemFetcher.fetch(hash, env);
        // With the grace period active and no claimer known, the first ask is
        // deferred rather than blind-asking a peer.
        REQUIRE(askCount == 0);

        // A claim during the grace window is acted on immediately.
        itemFetcher.peerClaimsItem(hash, peer2);
        REQUIRE(askCount == 1);
        REQUIRE(lastAsked == peer2);
        REQUIRE(claimAsk.count() == 1);
        REQUIRE(graceSatisfied.count() == 1);
        REQUIRE(graceExpired.count() == 0);
    }

    SECTION("grace expires and falls back to a blind ask")
    {
        auto const start = app->getClock().now();
        itemFetcher.fetch(hash, env);
        REQUIRE(askCount == 0);

        // No claim arrives; once the grace expires we fall back to asking a
        // peer anyway (liveness is preserved).
        s.sim->crankUntil([&]() { return askCount == 1; },
                          Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS +
                              std::chrono::seconds{1},
                          false);
        auto const elapsed = app->getClock().now() - start;

        REQUIRE(elapsed >= Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS);
        REQUIRE(graceExpired.count() == 1);
        REQUIRE(graceSatisfied.count() == 0);
    }
#endif // CAP_0083
}

TEST_CASE("HAVE_TX_SET admission cap", "[overlay][ItemFetcher]")
{
    FetchTestSim s(2);
    auto sim = s.sim;
    auto app = s.app;
    // Main's view of each remote peer (where the admission counter lives)
    auto peer1 = s.peers[0];
    auto peer2 = s.peers[1];
    // The remote ends, used to send claims TO Main
    auto sender1 = s.senders[0];
    auto sender2 = s.senders[1];

    auto& dropped = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-dropped"}, "item-fetcher");

    auto claimMsg = [](std::string const& seed) {
        StellarMessage msg;
        msg.type(HAVE_TX_SET);
        msg.haveTxSet().txSetHash = sha256(ByteSlice(seed));
        return std::make_shared<StellarMessage const>(msg);
    };

    uint32_t const Q = Peer::HAVE_TX_SET_MAX_PER_PERIOD;
    uint32_t const K = 8;

    // Fill the budget and then some: exactly Q admissions, K drops.
    for (uint32_t i = 0; i < Q + K; i++)
    {
        sender1->sendMessage(claimMsg("cap-" + std::to_string(i)), false);
    }
    sim->crankUntil([&]() { return dropped.count() >= K; },
                    std::chrono::seconds{5}, false);
    REQUIRE(dropped.count() == K);
    REQUIRE(peer1->getHaveTxSetAdmittedForTesting() == Q);

    // The budget is per peer: another peer's claims are unaffected.
    sender2->sendMessage(claimMsg("iso"), false);
    sim->crankUntil(
        [&]() { return peer2->getHaveTxSetAdmittedForTesting() == 1; },
        std::chrono::seconds{5}, false);
    REQUIRE(peer2->getHaveTxSetAdmittedForTesting() == 1);
    REQUIRE(dropped.count() == K);

    // Each peer's recurrent timer resets its budget once per period.
    sim->crankUntil(
        [&]() {
            return peer1->getHaveTxSetAdmittedForTesting() == 0 &&
                   peer2->getHaveTxSetAdmittedForTesting() == 0;
        },
        Peer::RECURRENT_TIMER_PERIOD * 2, false);

    // Post-reset claims are admitted again.
    sender1->sendMessage(claimMsg("post-reset"), false);
    sim->crankUntil(
        [&]() { return peer1->getHaveTxSetAdmittedForTesting() == 1; },
        std::chrono::seconds{5}, false);
    REQUIRE(peer1->getHaveTxSetAdmittedForTesting() == 1);
    REQUIRE(dropped.count() == K);
}

TEST_CASE("HAVE_TX_SET announce respects peer capability",
          "[overlay][ItemFetcher]")
{
    // The second remote predates HAVE_TX_SET: it must never be sent the
    // message
    FetchTestSim s(2, /*legacyRemote=*/1);
    auto sim = s.sim;
    auto app = s.app;
    auto peer1 = s.peers[0];
    auto peer2 = s.peers[1];

    auto& sent = app->getMetrics().NewMeter({"overlay", "send", "have-tx-set"},
                                            "message");

    auto& pe = static_cast<HerderImpl&>(app->getHerder()).getPendingEnvelopes();
    auto txset = TxSetXDRFrame::makeEmpty(
        app->getLedgerManager().getLastClosedLedgerHeader());
    auto hash = sha256(ByteSlice("announce"));

    // Obtaining a tx set announces it — but only to the capable peer.
    pe.addTxSet(hash, 10, txset);
    REQUIRE(sent.count() == 1);

    // Announce-once per hash: re-adding does not re-announce.
    pe.addTxSet(hash, 10, txset);
    REQUIRE(sent.count() == 1);

    // Tx sets restored from disk at startup (slot 0 sentinel) are not
    // announced.
    pe.addTxSet(sha256(ByteSlice("restored")), 0, txset);
    REQUIRE(sent.count() == 1);
}

TEST_CASE("HAVE_TX_SET claims accompany SCP state", "[overlay][ItemFetcher]")
{
    // The second remote predates HAVE_TX_SET: it must never be sent claims
    FetchTestSim s(2, /*legacyRemote=*/1);
    auto sim = s.sim;
    auto app = s.app;
    auto peer1 = s.peers[0];
    auto peer2 = s.peers[1];

    auto& sent = app->getMetrics().NewMeter({"overlay", "send", "have-tx-set"},
                                            "message");
    auto const sentBase = sent.count();

    auto& pe = static_cast<HerderImpl&>(app->getHerder()).getPendingEnvelopes();
    auto txset = TxSetXDRFrame::makeEmpty(
        app->getLedgerManager().getLastClosedLedgerHeader());
    auto heldHash = sha256(ByteSlice("held"));
    auto unheldHash = sha256(ByteSlice("unheld"));
    // Take possession directly, without addTxSet's announce side effect.
    pe.putTxSet(heldHash, 2, txset);

    // An envelope whose nomination votes reference a held set, an unheld
    // set, and the empty-tx-set value.
    SCPEnvelope env;
    env.statement.slotIndex = 2;
    env.statement.pledges.type(SCP_ST_NOMINATE);
    auto addVote = [&](Hash const& h) {
        StellarValue sv;
        sv.txSetHash = h;
        sv.closeTime = 1;
        env.statement.pledges.nominate().votes.emplace_back(
            xdr::xdr_to_opaque(sv));
    };
    addVote(heldHash);
    addVote(unheldHash);
    addVote(Herder::EMPTY_TX_SET_HASH);

    // Only the held set is claimed: the unheld set and the emtpy-tx-set are
    // skipped.
    UnorderedSet<Hash> claimed;
    pe.sendHaveTxSetClaims(env, peer1, claimed);
    REQUIRE(sent.count() == sentBase + 1);
    REQUIRE(claimed == UnorderedSet<Hash>{heldHash});

    // Within one exchange, further envelopes referencing the same set do not
    // re-claim it.
    pe.sendHaveTxSetClaims(env, peer1, claimed);
    REQUIRE(sent.count() == sentBase + 1);

    // Legacy peers are never sent claims.
    UnorderedSet<Hash> claimedLegacy;
    pe.sendHaveTxSetClaims(env, peer2, claimedLegacy);
    REQUIRE(sent.count() == sentBase + 1);
    REQUIRE(claimedLegacy.empty());

    // The claims actually arrive at the capable peer, and both connections
    // stay up.
    auto node1 = s.remoteApps[0];
    sim->crankUntil(
        [&]() {
            return node1->getMetrics()
                       .NewTimer({"overlay", "recv", "have-tx-set"})
                       .count() >= 1;
        },
        std::chrono::seconds{5}, false);
    REQUIRE(peer1->isAuthenticatedForTesting());
    REQUIRE(peer2->isAuthenticatedForTesting());
}

TEST_CASE("HAVE_TX_SET from unsupporting peer is rejected",
          "[overlay][ItemFetcher]")
{
    // The remote advertises a pre-HAVE_TX_SET overlay version.
    FetchTestSim s(1, /*legacyRemote=*/0);
    auto sim = s.sim;
    auto peer1 = s.peers[0];
    auto sender1 = s.senders[0];

    // The legacy remote sends a HAVE_TX_SET message
    StellarMessage msg;
    msg.type(HAVE_TX_SET);
    msg.haveTxSet().txSetHash = sha256(ByteSlice("violation"));
    sender1->sendMessage(std::make_shared<StellarMessage const>(msg), false);

    // Legacy remote is dropped
    sim->crankUntil([&]() { return !peer1->isAuthenticatedForTesting(); },
                    std::chrono::seconds{5}, false);
    REQUIRE(!peer1->isAuthenticatedForTesting());
}

#ifdef CAP_0083
TEST_CASE("relayer possession semantics", "[overlay][ItemFetcher]")
{
    // Once the protocol allows empty-tx-set values, an SCP message relayer
    // merely knows OF an item and must claim possession explicitly; a legacy
    // relayer keeps the historical implies-possession semantics.
    bool legacyPeer = GENERATE(false, true);

    FetchTestSim s(1, legacyPeer ? std::optional<size_t>{0} : std::nullopt);
    auto sim = s.sim;
    auto app = s.app;
    auto peer1 = s.peers[0];

    int askCount = 0;
    ItemFetcher itemFetcher(
        *app, [&](Peer::pointer, Hash) { askCount++; }, ItemFetcherKind::TxSet);

    // With no relayer knowledge, the first ask goes out via the random tier
    // (asked without assumed possession).
    auto env = makeEnvelope(500);
    auto hash = sha256(ByteSlice("500"));
    itemFetcher.fetch(hash, env);
    // Ride out the claim grace period: with no claim known, the first (blind)
    // ask fires once the grace period expires.
    sim->crankForAtLeast(Tracker::MS_TO_WAIT_FOR_FETCH_PROGRESS +
                             std::chrono::milliseconds(100),
                         false);
    auto tracker = itemFetcher.getTracker(hash);
    REQUIRE(tracker);
    REQUIRE(askCount == 1);
    REQUIRE(tracker->getLastAskedPeer() == peer1);

    // Now peer1 relays the envelope.
    StellarMessage msg(SCP_MESSAGE);
    msg.envelope() = env;
    app->getOverlayManager().recvFloodedMsgID(peer1, xdrBlake2(msg));

    tracker->tryNextPeer();
    if (legacyPeer)
    {
        // Historical semantics: knowing of the envelope implies having the
        // tx set, so the peer is re-asked.
        REQUIRE(askCount == 2);
        REQUIRE(tracker->getLastAskedPeer() == peer1);
    }
    else
    {
        // Knowing OF the item does not re-enable the peer; the fetch idles
        // in a rebuild instead. Possession must be claimed explicitly (via
        // HAVE_TX_SET), which the claim tests cover.
        REQUIRE(askCount == 1);
        REQUIRE(!tracker->getLastAskedPeer());
    }
}

TEST_CASE("legacy relayer bypasses the claim grace period",
          "[overlay][ItemFetcher]")
{
    // A relayer whose overlay version predates HAVE_TX_SET provably fetched
    // the tx set before relaying, but can never claim possession. Its relay
    // is treated as a claim: the fetch targets it immediately instead of
    // waiting out the claim grace.
    FetchTestSim s(1, /*legacyRemote=*/0);
    auto app = s.app;
    auto peer1 = s.peers[0];

    int askCount = 0;
    Peer::pointer lastAsked;
    ItemFetcher itemFetcher(
        *app,
        [&](Peer::pointer p, Hash) {
            ++askCount;
            lastAsked = p;
        },
        ItemFetcherKind::TxSet);

    auto& claimAsk = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-ask"}, "item-fetcher");
    auto& graceSatisfied = app->getMetrics().NewMeter(
        {"overlay", "item-fetcher", "claim-grace-satisfied"}, "item-fetcher");

    // peer1 relays the envelope before the fetch starts.
    auto env = makeEnvelope(600);
    auto hash = sha256(ByteSlice("600"));
    StellarMessage msg(SCP_MESSAGE);
    msg.envelope() = env;
    app->getOverlayManager().recvFloodedMsgID(peer1, xdrBlake2(msg));

    // The first ask fires immediately (no grace period delay) and targets the
    // legacy relayer.
    itemFetcher.fetch(hash, env);
    REQUIRE(askCount == 1);
    REQUIRE(lastAsked == peer1);
    REQUIRE(claimAsk.count() == 1);
    REQUIRE(graceSatisfied.count() == 1);
}
#endif // CAP_0083
}
