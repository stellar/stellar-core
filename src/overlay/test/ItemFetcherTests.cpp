// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "lib/catch.hpp"
#include "main/ApplicationImpl.h"
#include "overlay/ItemFetcher.h"
#include "overlay/LoopbackPeer.h"
#include "overlay/OverlayManager.h"
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
    HerderStub(Application& app) : HerderImpl(app){};

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
    std::vector<Hash> received;
    ItemFetcher itemFetcher(*app, [&](Peer::pointer peer, Hash hash) {
        asked.push_back(peer);
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

    SECTION("stop one")
    {
        itemFetcher.stopFetch(twelve, twelveEnvelope1);
        REQUIRE(itemFetcher.getLastSeenSlotIndex(twelve) != 0);
        checkFetchingFor(twelve, {twelveEnvelope2});

        itemFetcher.recv(twelve);
        itemFetcher.recv(ten);

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

        itemFetcher.recv(twelve);
        itemFetcher.recv(ten);

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
        itemFetcher.recv(twelve);
        itemFetcher.recv(ten);

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
            itemFetcher.recv(zero);

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
            LoopbackPeerConnection connection2(*app, *other2);
            auto peer1 = connection1.getInitiator();
            auto peer2 = connection2.getInitiator();

            SECTION("fetching once works")
            {
                auto zeroEnvelope1 = makeEnvelope(0);
                itemFetcher.fetch(zero, zeroEnvelope1);
            }
            SECTION("fetching twice does not trigger any additional network "
                    "activity")
            {
                auto zeroEnvelope1 = makeEnvelope(0);
                auto zeroEnvelope2 = makeEnvelope(0);
                itemFetcher.fetch(zero, zeroEnvelope1);
                itemFetcher.fetch(zero, zeroEnvelope2);
            }
            REQUIRE(asked.size() == 0);

            while (asked.size() < 4)
            {
                clock.crank(true);
            }

            itemFetcher.recv(zero);

            while (clock.crank(false) > 0)
            {
            }

            REQUIRE(asked.size() == 4);

            REQUIRE(std::count(asked.begin(), asked.end(), peer1) == 2);
            REQUIRE(std::count(asked.begin(), asked.end(), peer2) == 2);
        }

        SECTION("ignore not asked items")
        {
            itemFetcher.recv(zero);
            REQUIRE(app->getHerder().received ==
                    expectedReceived); // no new data received
        }
    }
}
}
