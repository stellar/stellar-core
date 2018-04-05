// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PeerRecord.h"
#include "database/Database.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/StellarXDR.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include <soci.h>

namespace stellar
{

using namespace std;

TEST_CASE("toXdr", "[overlay][PeerRecord]")
{
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, getTestConfig());
    auto address = PeerBareAddress::resolve("1.25.50.200:256", *app);
    auto pr = PeerRecord{address, app->getClock().now(), 0};
    pr.mNumFailures = 2;

    SECTION("fromIPPort and toXdr")
    {
        REQUIRE(address.getIP() == "1.25.50.200");
        REQUIRE(address.getPort() == 256);

        PeerAddress xdr;
        pr.toXdr(xdr);
        REQUIRE(xdr.port == 256);
        REQUIRE(xdr.ip.ipv4()[0] == 1);
        REQUIRE(xdr.ip.ipv4()[1] == 25);
        REQUIRE(xdr.ip.ipv4()[2] == 50);
        REQUIRE(xdr.ip.ipv4()[3] == 200);
        REQUIRE(xdr.numFailures == 2);
    }
    SECTION("loadPeerRecord and storePeerRecord")
    {
        pr.mNextAttempt = pr.mNextAttempt + chrono::seconds(12);
        REQUIRE(pr.insertIfNew(app->getDatabase()));

        {
            // second time should return false and not modify it
            PeerRecord pr2(pr);
            pr2.mNumFailures++;
            REQUIRE(!pr2.insertIfNew(app->getDatabase()));

            auto actualPR =
                PeerRecord::loadPeerRecord(app->getDatabase(), pr.getAddress());
            REQUIRE(*actualPR == pr);
        }

        PeerRecord other(PeerBareAddress{"1.2.3.4", 15}, clock.now());
        other.storePeerRecord(app->getDatabase());

        pr.mNextAttempt = pr.mNextAttempt + chrono::seconds(12);
        pr.storePeerRecord(app->getDatabase());
        auto actual1 =
            PeerRecord::loadPeerRecord(app->getDatabase(), pr.getAddress());
        REQUIRE(*actual1 == pr);

        auto actual2 = PeerRecord::loadPeerRecord(
            app->getDatabase(), PeerBareAddress{"1.2.3.4", 15});
        REQUIRE(*actual2 == other);
    }
}

TEST_CASE("private addresses", "[overlay][PeerRecord]")
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

TEST_CASE("create peer rercord", "[overlay][PeerRecord]")
{
    SECTION("empty")
    {
        REQUIRE_THROWS_AS(PeerBareAddress("", 0), std::runtime_error);
    }

    SECTION("empty ip")
    {
        REQUIRE_THROWS_AS(PeerBareAddress("", 80), std::runtime_error);
    }

    SECTION("zero port")
    {
        REQUIRE_THROWS_AS(PeerBareAddress("127.0.0.1", 0), std::runtime_error);
    }

    SECTION("random string") // PeerBareAddress does not validate IP format
    {
        auto pa = PeerBareAddress("random string", 80);
        REQUIRE(pa.getIP() == "random string");
        REQUIRE(pa.getPort() == 80);
    }

    SECTION("valid data")
    {
        auto pa = PeerBareAddress("127.0.0.1", 80);
        REQUIRE(pa.getIP() == "127.0.0.1");
        REQUIRE(pa.getPort() == 80);
    }
}

TEST_CASE("parse peer rercord", "[overlay][PeerRecord]")
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
}
