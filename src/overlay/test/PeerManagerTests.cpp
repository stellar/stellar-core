// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerManager.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/StellarXDR.h"
#include "test/TestUtils.h"
#include "test/test.h"

namespace stellar
{

using namespace std;

PeerBareAddress
localhost(unsigned short port)
{
    return PeerBareAddress{"127.0.0.1", port};
}

TEST_CASE("toXdr", "[overlay][PeerManager]")
{
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, getTestConfig());
    auto& pm = app->getOverlayManager().getPeerManager();
    auto address = PeerBareAddress::resolve("1.25.50.200:256", *app);

    SECTION("toXdr")
    {
        REQUIRE(address.getIP() == "1.25.50.200");
        REQUIRE(address.getPort() == 256);

        auto xdr = toXdr(address);
        REQUIRE(xdr.port == 256);
        REQUIRE(xdr.ip.ipv4()[0] == 1);
        REQUIRE(xdr.ip.ipv4()[1] == 25);
        REQUIRE(xdr.ip.ipv4()[2] == 50);
        REQUIRE(xdr.ip.ipv4()[3] == 200);
        REQUIRE(xdr.numFailures == 0);
    }

    SECTION("database roundtrip")
    {
        auto test = [&](PeerType peerType) {
            auto loadedPR = pm.load(address);
            REQUIRE(!loadedPR.second);

            auto storedPr = loadedPR.first;
            storedPr.mType = static_cast<int>(peerType);
            pm.store(address, storedPr, false);

            auto actualPR = pm.load(address);
            REQUIRE(actualPR.second);
            REQUIRE(actualPR.first == storedPr);
        };

        SECTION("inbound")
        {
            test(PeerType::INBOUND);
        }

        SECTION("outbound")
        {
            test(PeerType::OUTBOUND);
        }

        SECTION("preferred")
        {
            test(PeerType::PREFERRED);
        }
    }
}

TEST_CASE("private addresses", "[overlay][PeerManager]")
{
    PeerBareAddress pa("1.2.3.4", 15);
    CHECK(!pa.isPrivate());
    pa = PeerBareAddress("10.1.2.3", 15);
    CHECK(pa.isPrivate());
    pa = PeerBareAddress("172.17.1.2", 15);
    CHECK(pa.isPrivate());
    pa = PeerBareAddress("192.168.1.2", 15);
    CHECK(pa.isPrivate());
}

TEST_CASE("create peer record", "[overlay][PeerManager]")
{
    SECTION("empty")
    {
        REQUIRE_THROWS_AS(PeerBareAddress("", 0), std::runtime_error);
    }

    SECTION("empty ip")
    {
        REQUIRE_THROWS_AS(PeerBareAddress("", 80), std::runtime_error);
    }

    SECTION("random string") // PeerBareAddress does not validate IP format
    {
        auto pa = PeerBareAddress("random string", 80);
        REQUIRE(pa.getIP() == "random string");
        REQUIRE(pa.getPort() == 80);
    }

    SECTION("valid data")
    {
        auto pa = localhost(80);
        REQUIRE(pa.getIP() == "127.0.0.1");
        REQUIRE(pa.getPort() == 80);
    }
}

TEST_CASE("parse peer record", "[overlay][PeerManager]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    SECTION("empty")
    {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("", *app),
                          std::runtime_error);
    }

    SECTION("random string")
    {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("random string", *app),
                          std::runtime_error);
    }

    SECTION("invalid ipv4")
    {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.256", *app),
                          std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("256.256.256.256", *app),
                          std::runtime_error);
    }

    SECTION("ipv4 mask instead of address")
    {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.1/8", *app),
                          std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.1/16", *app),
                          std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.1/24", *app),
                          std::runtime_error);
    }

    SECTION("valid ipv6")
    {
        REQUIRE_THROWS_AS(
            PeerBareAddress::resolve("2001:db8:a0b:12f0::1", *app),
            std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve(
                              "2001:0db8:0a0b:12f0:0000:0000:0000:0001", *app),
                          std::runtime_error);
    }

    SECTION("invalid ipv6")
    {
        REQUIRE_THROWS_AS(
            PeerBareAddress::resolve("10000:db8:a0b:12f0::1", *app),
            std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve(
                              "2001:0db8:0a0b:12f0:0000:10000:0000:0001", *app),
                          std::runtime_error);
    }

    SECTION("ipv6 mask instead of address")
    {
        REQUIRE_THROWS_AS(
            PeerBareAddress::resolve("2001:db8:a0b:12f0::1/16", *app),
            std::runtime_error);
        REQUIRE_THROWS_AS(
            PeerBareAddress::resolve("2001:db8:a0b:12f0::1/32", *app),
            std::runtime_error);
        REQUIRE_THROWS_AS(
            PeerBareAddress::resolve("2001:db8:a0b:12f0::1/64", *app),
            std::runtime_error);
    }

    SECTION("valid ipv4 with empty port")
    {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.2:", *app),
                          std::runtime_error);
    }

    SECTION("valid ipv4 with invalid port")
    {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.2:-1", *app),
                          std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.2:0", *app),
                          std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.2:65536", *app),
                          std::runtime_error);
        REQUIRE_THROWS_AS(PeerBareAddress::resolve("127.0.0.2:65537", *app),
                          std::runtime_error);
    }

    SECTION("valid ipv4 with default port")
    {
        auto pr = PeerBareAddress::resolve("127.0.0.2", *app);
        REQUIRE(pr.getIP() == "127.0.0.2");
        REQUIRE(pr.getPort() == DEFAULT_PEER_PORT);

        pr = PeerBareAddress::resolve("8.8.8.8", *app);
        REQUIRE(pr.getIP() == "8.8.8.8");
        REQUIRE(pr.getPort() == DEFAULT_PEER_PORT);
    }

    SECTION("valid ipv4 with different default port")
    {
        auto pr = PeerBareAddress::resolve("127.0.0.2", *app, 10);
        REQUIRE(pr.getIP() == "127.0.0.2");
        REQUIRE(pr.getPort() == 10);

        pr = PeerBareAddress::resolve("8.8.8.8", *app, 10);
        REQUIRE(pr.getIP() == "8.8.8.8");
        REQUIRE(pr.getPort() == 10);
    }

    SECTION("valid ipv4 with valid port")
    {
        auto pr = PeerBareAddress::resolve("127.0.0.2:1", *app);
        REQUIRE(pr.getIP() == "127.0.0.2");
        REQUIRE(pr.getPort() == 1);

        pr = PeerBareAddress::resolve("127.0.0.2:1234", *app);
        REQUIRE(pr.getIP() == "127.0.0.2");
        REQUIRE(pr.getPort() == 1234);

        pr = PeerBareAddress::resolve("127.0.0.2:65534", *app);
        REQUIRE(pr.getIP() == "127.0.0.2");
        REQUIRE(pr.getPort() == 65534);

        pr = PeerBareAddress::resolve("127.0.0.2:65535", *app);
        REQUIRE(pr.getIP() == "127.0.0.2");
        REQUIRE(pr.getPort() == 65535);
    }
}

TEST_CASE("loadRandomPeers", "[overlay][PeerManager]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& peerManager = app->getOverlayManager().getPeerManager();

    auto getPorts = [&](PeerQuery const& query) {
        auto peers = peerManager.loadRandomPeers(query, 1000);
        auto result = std::set<int>{};
        std::transform(
            std::begin(peers), std::end(peers),
            std::inserter(result, std::end(result)),
            [](PeerBareAddress const& address) { return address.getPort(); });
        return result;
    };

    auto now = clock.system_now();
    auto past = clock.system_now() - std::chrono::seconds(1);
    auto future = clock.system_now() + std::chrono::seconds(1);

    unsigned short port = 1;
    auto peerRecords = std::map<int, PeerRecord>{};
    for (auto time : {past, now, future})
    {
        for (auto numFailures : {0, 1})
        {
            for (auto type :
                 {PeerType::INBOUND, PeerType::OUTBOUND, PeerType::PREFERRED})
            {
                auto peerRecord =
                    PeerRecord{VirtualClock::systemPointToTm(time), numFailures,
                               static_cast<int>(type)};
                peerRecords[port] = peerRecord;
                peerManager.store(localhost(port), peerRecord, false);
                port++;
            }
        }
    }

    auto valid = [&](PeerQuery const& peerQuery, PeerRecord const& peerRecord) {
        if (peerQuery.mUseNextAttempt)
        {
            if (VirtualClock::tmToSystemPoint(peerRecord.mNextAttempt) > now)
            {
                return false;
            }
        }
        if (peerQuery.mMaxNumFailures >= 0)
        {
            if (peerRecord.mNumFailures > peerQuery.mMaxNumFailures)
            {
                return false;
            }
        }
        switch (peerQuery.mTypeFilter)
        {
        case PeerTypeFilter::INBOUND_ONLY:
        {
            return peerRecord.mType == static_cast<int>(PeerType::INBOUND);
        }
        case PeerTypeFilter::OUTBOUND_ONLY:
        {
            return peerRecord.mType == static_cast<int>(PeerType::OUTBOUND);
        }
        case PeerTypeFilter::PREFERRED_ONLY:
        {
            return peerRecord.mType == static_cast<int>(PeerType::PREFERRED);
        }
        case PeerTypeFilter::ANY_OUTBOUND:
        {
            return peerRecord.mType == static_cast<int>(PeerType::OUTBOUND) ||
                   peerRecord.mType == static_cast<int>(PeerType::PREFERRED);
        }
        default:
        {
            abort();
        }
        }
    };

    for (auto useNextAttempt : {false, true})
    {
        for (auto numFailures : {-1, 0})
        {
            for (auto filter :
                 {PeerTypeFilter::INBOUND_ONLY, PeerTypeFilter::OUTBOUND_ONLY,
                  PeerTypeFilter::PREFERRED_ONLY, PeerTypeFilter::ANY_OUTBOUND})
            {
                auto query = PeerQuery{useNextAttempt, numFailures, filter};
                auto ports = getPorts(query);
                for (auto record : peerRecords)
                {
                    if (ports.find(record.first) != std::end(ports))
                    {
                        REQUIRE(valid(query, record.second));
                    }
                    else
                    {
                        REQUIRE(!valid(query, record.second));
                    }
                }
            }
        }
    }
}

TEST_CASE("getPeersToSend", "[overlay][PeerManager]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& peerManager = app->getOverlayManager().getPeerManager();
    auto myAddress = PeerBareAddress("127.0.0.255", 1);
    auto getSize = [&](int requestedSize) {
        return peerManager.getPeersToSend(requestedSize, myAddress).size();
    };
    auto createPeers = [&](unsigned short normalInboundCount,
                           unsigned short failedInboundCount,
                           unsigned short normalOutboundCount,
                           unsigned short failedOutboundCount) {
        unsigned short port = 1;
        for (auto i = 0; i < normalInboundCount; i++)
        {
            peerManager.ensureExists(localhost(port++));
        }
        for (auto i = 0; i < failedInboundCount; i++)
        {
            peerManager.store(
                localhost(port++),
                PeerRecord{{}, 11, static_cast<int>(PeerType::INBOUND)}, false);
        }
        for (auto i = 0; i < normalOutboundCount; i++)
        {
            peerManager.update(localhost(port++), PeerType::OUTBOUND, false);
        }
        for (auto i = 0; i < failedOutboundCount; i++)
        {
            peerManager.store(
                localhost(port++),
                PeerRecord{{}, 11, static_cast<int>(PeerType::OUTBOUND)},
                false);
        }
    };

    SECTION("no peers in database")
    {
        REQUIRE(getSize(0) == 0);
        REQUIRE(getSize(10) == 0);
        REQUIRE(getSize(50) == 0);
    }

    SECTION("less peers in database than requested")
    {
        SECTION("only inbound peers")
        {
            createPeers(8, 0, 0, 0);
            REQUIRE(getSize(10) == 8);
            REQUIRE(getSize(50) == 8);
        }
        SECTION("only outbound peers")
        {
            createPeers(0, 0, 8, 0);
            REQUIRE(getSize(10) == 8);
            REQUIRE(getSize(50) == 8);
        }
        SECTION("mixed peers")
        {
            createPeers(4, 0, 4, 0);
            REQUIRE(getSize(10) == 8);
            REQUIRE(getSize(50) == 8);
        }
    }

    SECTION("as many peers in database as requested")
    {
        SECTION("only inbound peers")
        {
            createPeers(8, 0, 0, 0);
            REQUIRE(getSize(8) == 8);
        }
        SECTION("only outbound peers")
        {
            createPeers(0, 0, 8, 0);
            REQUIRE(getSize(8) == 8);
        }
        SECTION("mixed peers")
        {
            createPeers(4, 0, 4, 0);
            REQUIRE(getSize(8) == 8);
        }
    }

    SECTION("more peers in database than requested")
    {
        SECTION("only inbound peers")
        {
            createPeers(50, 0, 0, 0);
            REQUIRE(getSize(30) == 30);
        }
        SECTION("only outbound peers")
        {
            createPeers(0, 0, 50, 0);
            REQUIRE(getSize(30) == 30);
        }
        SECTION("mixed peers")
        {
            createPeers(25, 0, 25, 0);
            REQUIRE(getSize(30) == 30);
        }
    }

    SECTION("more peers in database than requested, but half failed")
    {
        SECTION("only inbound peers")
        {
            createPeers(25, 25, 0, 0);
            REQUIRE(getSize(30) == 25);
        }
        SECTION("only outbound peers")
        {
            createPeers(0, 0, 25, 25);
            REQUIRE(getSize(30) == 25);
        }
        SECTION("mixed peers")
        {
            createPeers(13, 12, 13, 12);
            REQUIRE(getSize(30) == 26);
        }
    }
}

TEST_CASE("RandomPeerSource::nextAttemptCutoff also limits maxFailures",
          "[overlay][PeerManager]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& peerManager = app->getOverlayManager().getPeerManager();
    auto randomPeerSource = RandomPeerSource{
        peerManager, RandomPeerSource::nextAttemptCutoff(PeerType::OUTBOUND)};

    auto now = VirtualClock::systemPointToTm(clock.system_now());
    peerManager.store(localhost(1),
                      {now, 0, static_cast<int>(PeerType::INBOUND)}, false);
    peerManager.store(localhost(2),
                      {now, 0, static_cast<int>(PeerType::OUTBOUND)}, false);
    peerManager.store(localhost(3),
                      {now, 120, static_cast<int>(PeerType::INBOUND)}, false);
    peerManager.store(localhost(4),
                      {now, 120, static_cast<int>(PeerType::OUTBOUND)}, false);
    peerManager.store(localhost(5),
                      {now, 121, static_cast<int>(PeerType::INBOUND)}, false);
    peerManager.store(localhost(6),
                      {now, 121, static_cast<int>(PeerType::OUTBOUND)}, false);

    auto peers = randomPeerSource.getRandomPeers(
        50, [](PeerBareAddress const&) { return true; });
    REQUIRE(peers.size() == 2);
    REQUIRE(std::find(std::begin(peers), std::end(peers), localhost(2)) !=
            std::end(peers));
    REQUIRE(std::find(std::begin(peers), std::end(peers), localhost(4)) !=
            std::end(peers));
}

TEST_CASE("purge peer table", "[overlay][PeerManager]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto& peerManager = app->getOverlayManager().getPeerManager();
    auto record = [](int numFailures) {
        return PeerRecord{{}, numFailures, static_cast<int>(PeerType::INBOUND)};
    };

    peerManager.store(localhost(1), record(1), false);
    peerManager.store(localhost(2), record(2), false);
    peerManager.store(localhost(3), record(3), false);
    peerManager.store(localhost(4), record(4), false);
    peerManager.store(localhost(5), record(5), false);

    peerManager.removePeersWithManyFailures(3);
    REQUIRE(peerManager.load(localhost(1)).second);
    REQUIRE(peerManager.load(localhost(2)).second);
    REQUIRE(!peerManager.load(localhost(3)).second);
    REQUIRE(!peerManager.load(localhost(4)).second);
    REQUIRE(!peerManager.load(localhost(5)).second);

    auto localhost2 = localhost(2);
    peerManager.removePeersWithManyFailures(3, &localhost2);
    REQUIRE(peerManager.load(localhost(2)).second);

    peerManager.removePeersWithManyFailures(2, &localhost2);
    REQUIRE(!peerManager.load(localhost(2)).second);
}
}
