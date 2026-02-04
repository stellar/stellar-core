// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerManager.h"
#include "overlay/RandomPeerSource.h"
#include "overlay/StellarXDR.h"
#include "test/Catch2.h"
#include "test/TestUtils.h"
#include "test/test.h"

namespace stellar
{

using namespace std;

static PeerBareAddress
localhost(unsigned short port)
{
    return PeerBareAddress{asio::ip::address_v4::loopback(), port};
}

TEST_CASE("toXdr", "[overlay][PeerManager]")
{
    auto run = [](asio::ip::address ip) {
        VirtualClock clock;
        Application::pointer app =
            createTestApplication(clock, getTestConfig());
        auto& pm = app->getOverlayManager().getPeerManager();
        auto address = PeerBareAddress(ip, 256);

        SECTION("toXdr")
        {
            REQUIRE(address.getIP().to_string() == ip.to_string());
            REQUIRE(address.getPort() == 256);

            auto xdr = toXdr(address);
            REQUIRE(xdr.port == 256);
            if (ip.is_v4())
            {
                REQUIRE(xdr.ip.type() == IPv4);
                REQUIRE(xdr.ip.ipv4() == ip.to_v4().to_bytes());
            }
            else
            {
                REQUIRE(xdr.ip.type() == IPv6);
                REQUIRE(xdr.ip.ipv6() == ip.to_v6().to_bytes());
            }
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
    };

    SECTION("IPv4")
    {
        run(asio::ip::address::from_string("1.25.50.200"));
    }
    SECTION("IPv6")
    {
        run(asio::ip::address::from_string("2001:0db8::"));
    }
}

TEST_CASE("private addresses", "[overlay][PeerManager]")
{
    auto checkRange4 = [](char const* cidr, bool checkPrev = true,
                          bool checkNext = true) {
        auto network = asio::ip::make_network_v4(cidr);
        if (checkPrev)
        {
            auto addr4 =
                asio::ip::make_address_v4(network.network().to_uint() - 1);
            auto addr6 = asio::ip::make_address_v6(asio::ip::v4_mapped, addr4);
            CHECK(!PeerBareAddress(addr4, 15).isPrivate());
            CHECK(!PeerBareAddress(addr6, 15).isPrivate());
        }

        auto addr4 = network.network();
        auto addr6 = asio::ip::make_address_v6(asio::ip::v4_mapped, addr4);
        CHECK(PeerBareAddress(addr4, 15).isPrivate());
        CHECK(PeerBareAddress(addr6, 15).isPrivate());

        addr4 = network.broadcast();
        addr6 = asio::ip::make_address_v6(asio::ip::v4_mapped, addr4);
        CHECK(PeerBareAddress(addr4, 15).isPrivate());
        CHECK(PeerBareAddress(addr6, 15).isPrivate());

        if (checkNext)
        {
            addr4 =
                asio::ip::make_address_v4(network.broadcast().to_uint() + 1);
            addr6 = asio::ip::make_address_v6(asio::ip::v4_mapped, addr4);
            CHECK(!PeerBareAddress(addr4, 15).isPrivate());
            CHECK(!PeerBareAddress(addr6, 15).isPrivate());
        }
    };

    auto checkRange6 = [](char const* ip, int bits, bool checkPrev = true,
                          bool checkNext = true) {
        asio::ip::address_v6 start = asio::ip::make_address_v6(ip);
        CHECK(PeerBareAddress(start, 15).isPrivate());
        if (checkPrev)
        {
            auto prevBytes = start.to_bytes();
            for (int i = 15; i >= 0; --i)
            {
                if (prevBytes[i]-- != 0)
                {
                    break;
                }
            }
            auto prev = asio::ip::address_v6(prevBytes);
            CHECK(!PeerBareAddress(prev, 15).isPrivate());
        }

        auto endBytes = start.to_bytes();
        for (int i = 0; i < 16; i++)
        {
            if (bits >= 8)
            {
                bits -= 8;
            }
            else
            {
                endBytes[i] |= 0xFF >> bits;
                bits = 0;
            }
        }
        CHECK(PeerBareAddress(asio::ip::address_v6(endBytes), 15).isPrivate());

        if (checkNext)
        {
            for (int i = 15; i >= 0; --i)
            {
                if (++endBytes[i] != 0)
                {
                    break;
                }
            }
            CHECK(!PeerBareAddress(asio::ip::address_v6(endBytes), 15)
                       .isPrivate());
        }
    };

    SECTION("non-private addresses")
    {
        PeerBareAddress pa(asio::ip::address::from_string("1.2.3.4"), 15);
        CHECK(!pa.isPrivate());
        pa = PeerBareAddress(asio::ip::address::from_string("127.0.0.1"), 15);
        CHECK(!pa.isPrivate());
        CHECK(pa.isLocalhost());
        pa = PeerBareAddress(asio::ip::address::from_string("::1"), 15);
        CHECK(!pa.isPrivate());
        CHECK(pa.isLocalhost());
    }

    SECTION("0.0.0.0/8")
    {
        checkRange4("0.0.0.0/8", false);
    }
    SECTION("10.0.0.0/8")
    {
        checkRange4("10.0.0.0/8");
    }
    SECTION("100.64.0.0/10")
    {
        checkRange4("100.64.0.0/10");
    }
    SECTION("169.254.0.0/16")
    {
        checkRange4("169.254.0.0/16");
    }
    SECTION("172.16.0.0/12")
    {
        checkRange4("172.16.0.0/12");
    }
    SECTION("192.0.0.0/24")
    {
        checkRange4("192.0.0.0/24");
    }
    SECTION("192.0.2.0/24")
    {
        checkRange4("192.0.2.0/24");
    }
    SECTION("192.168.0.0/16")
    {
        checkRange4("192.168.0.0/16");
    }
    SECTION("198.18.0.0/15")
    {
        checkRange4("198.18.0.0/15");
    }
    SECTION("198.51.100.0/24")
    {
        checkRange4("198.51.100.0/24");
    }
    SECTION("203.0.113.0/24")
    {
        checkRange4("203.0.113.0/24");
    }
    SECTION("240.0.0.0/4")
    {
        checkRange4("240.0.0.0/4", true, false);
    }

    SECTION("::/128")
    {
        checkRange6("::", 128);
    }
    SECTION("100::/64")
    {
        checkRange6("100::", 64);
    }
    SECTION("2001::/32")
    {
        checkRange6("2001::", 32);
    }
    SECTION("2001:2::/48")
    {
        checkRange6("2001:2::", 48);
    }
    SECTION("2001:db8::/32")
    {
        checkRange6("2001:db8::", 32);
    }
    SECTION("2001:10::/28")
    {
        checkRange6("2001:10::", 28);
    }
    SECTION("fc00::/7")
    {
        checkRange6("fc00::", 7);
    }
    SECTION("fe80::/10")
    {
        checkRange6("fe80::", 10);
    }
}

TEST_CASE("create peer record", "[overlay][PeerManager]")
{
    SECTION("valid data")
    {
        auto pa = localhost(80);
        REQUIRE(pa.getIP().to_string() == "127.0.0.1");
        REQUIRE(pa.getPort() == 80);
    }
}

TEST_CASE("parse peer record", "[overlay][PeerManager]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());

    auto requireSuccess = [&app](std::string const& str,
                                 std::string const& expectedIP,
                                 unsigned short expectedPort) {
        auto pr = PeerBareAddress::resolve(str, *app);
        REQUIRE(pr.getIP().to_string() == expectedIP);
        REQUIRE(pr.getPort() == expectedPort);
    };

    auto requireThrow = [&app](std::string const& str) {
        REQUIRE_THROWS_AS(PeerBareAddress::resolve(str, *app),
                          std::runtime_error);
    };

    SECTION("empty")
    {
        requireThrow("");
    }

    SECTION("random string")
    {
        requireThrow("random string");
    }

    SECTION("invalid ipv4")
    {
        requireThrow("127.0.0.256");
        requireThrow("256.256.256.256");
    }

    SECTION("ipv4 mask instead of address")
    {
        requireThrow("127.0.0.1/8");
        requireThrow("127.0.0.1/16");
        requireThrow("127.0.0.1/24");
    }

    SECTION("valid non-bracketed ipv6")
    {
        requireThrow("2001:db8:a0b:12f0::1");
        requireThrow("2001:0db8:0a0b:12f0:0000:0000:0000:0001");
    }

    SECTION("invalid non-bracketed ipv6")
    {
        requireThrow("10000:db8:a0b:12f0::1");
        requireThrow("2001:0db8:0a0b:12f0:0000:10000:0000:0001");
    }

    SECTION("non-bracketed ipv6 mask instead of address")
    {
        requireThrow("2001:db8:a0b:12f0::1/16");
        requireThrow("2001:db8:a0b:12f0::1/32");
        requireThrow("2001:db8:a0b:12f0::1/64");
    }

    SECTION("valid ipv4 with empty port")
    {
        requireThrow("127.0.0.2:");
    }

    SECTION("valid ipv6 with empty port")
    {
        requireThrow("[2001:db8:a0b:12f0::1]:");
    }

    SECTION("valid ipv4 with invalid port")
    {
        requireThrow("127.0.0.2:-1");
        requireThrow("127.0.0.2:0");
        requireThrow("127.0.0.2:65536");
        requireThrow("127.0.0.2:65537");
    }

    SECTION("valid ipv6 with invalid port")
    {
        requireThrow("[2001:db8:a0b:12f0::1]:-1");
        requireThrow("[2001:db8:a0b:12f0::1]:0");
        requireThrow("[2001:db8:a0b:12f0::1]:65536");
        requireThrow("[2001:db8:a0b:12f0::1]:65537");
    }

    SECTION("valid ipv4 with default port")
    {
        requireSuccess("127.0.0.2", "127.0.0.2", DEFAULT_PEER_PORT);
        requireSuccess("8.8.8.8", "8.8.8.8", DEFAULT_PEER_PORT);
    }

    SECTION("valid ipv6 with default port")
    {
        requireSuccess("[2001:db8:a0b:12f0::1]", "2001:db8:a0b:12f0::1",
                       DEFAULT_PEER_PORT);
        requireSuccess("[2001:0db8:0a0b:12f0:0000:0000:0000:0001]",
                       "2001:db8:a0b:12f0::1", DEFAULT_PEER_PORT);
    }

    SECTION("valid ipv4 with different default port")
    {
        auto pr = PeerBareAddress::resolve("127.0.0.2", *app, 10);
        REQUIRE(pr.getIP().to_string() == "127.0.0.2");
        REQUIRE(pr.getPort() == 10);

        pr = PeerBareAddress::resolve("8.8.8.8", *app, 10);
        REQUIRE(pr.getIP().to_string() == "8.8.8.8");
        REQUIRE(pr.getPort() == 10);
    }

    SECTION("valid ipv6 with different default port")
    {
        auto pr = PeerBareAddress::resolve("[2001:db8:a0b:12f0::1]", *app, 10);
        REQUIRE(pr.getIP().to_string() == "2001:db8:a0b:12f0::1");
        REQUIRE(pr.getPort() == 10);

        pr = PeerBareAddress::resolve(
            "[2001:0db8:0a0b:12f0:0000:0000:0000:0001]", *app, 10);
        REQUIRE(pr.getIP().to_string() == "2001:db8:a0b:12f0::1");
        REQUIRE(pr.getPort() == 10);
    }

    SECTION("valid ipv4 with valid port")
    {
        requireSuccess("127.0.0.2:1", "127.0.0.2", 1);
        requireSuccess("127.0.0.2:1234", "127.0.0.2", 1234);
        requireSuccess("127.0.0.2:65534", "127.0.0.2", 65534);
        requireSuccess("127.0.0.2:65535", "127.0.0.2", 65535);
    }

    SECTION("valid ipv6 with valid port")
    {
        requireSuccess("[2001:db8:a0b:12f0::1]:1", "2001:db8:a0b:12f0::1", 1);
        requireSuccess("[2001:db8:a0b:12f0::1]:1234", "2001:db8:a0b:12f0::1",
                       1234);
        requireSuccess("[2001:db8:a0b:12f0::1]:65534", "2001:db8:a0b:12f0::1",
                       65534);
        requireSuccess("[2001:db8:a0b:12f0::1]:65535", "2001:db8:a0b:12f0::1",
                       65535);
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
        for (size_t numFailures : {0, 1})
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
        if (peerQuery.mMaxNumFailures.has_value())
        {
            if (peerRecord.mNumFailures > *peerQuery.mMaxNumFailures)
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
        for (std::optional<size_t> maxNumFailures :
             {std::optional<size_t>(std::nullopt),
              std::make_optional<size_t>(1)})
        {
            for (auto filter :
                 {PeerTypeFilter::INBOUND_ONLY, PeerTypeFilter::OUTBOUND_ONLY,
                  PeerTypeFilter::PREFERRED_ONLY, PeerTypeFilter::ANY_OUTBOUND})
            {
                auto query = PeerQuery{useNextAttempt, maxNumFailures, filter};
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
    auto myAddress =
        PeerBareAddress(asio::ip::address::from_string("127.0.0.255"), 1);
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
    auto record = [&app](size_t numFailures) {
        return PeerRecord{
            VirtualClock::systemPointToTm(app->getClock().system_now()),
            numFailures, static_cast<int>(PeerType::INBOUND)};
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
