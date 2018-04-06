// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/EnvelopeItemMap.h"
#include "herder/HerderTestUtils.h"

#include <lib/catch.hpp>

using namespace stellar;
using namespace stellar::HerderTestUtils;

namespace stellar
{
using xdr::operator==;
using xdr::operator<;
}

TEST_CASE("EnvelopeItemMap", "[herder][unit][EnvelopeItemMap]")
{
    auto item1 = ItemKey{ItemType::QUORUM_SET, {}};
    auto item2 = ItemKey{ItemType::TX_SET, {}};
    auto envelope1 = makeEnvelope({}, {}, 1);
    auto envelope2 = makeEnvelope({}, {}, 2);
    auto envelope3 = makeEnvelope({}, {}, 3);

    SECTION("empty")
    {
        EnvelopeItemMap eim;
        auto expectedItems = std::set<ItemKey>{};
        auto expectedEnvelopes = std::set<SCPEnvelope>{};

        REQUIRE(eim.envelopes(item1) == expectedEnvelopes);
        REQUIRE(eim.remove(item1) == expectedEnvelopes);
        REQUIRE(eim.remove(envelope1) == expectedItems);
    }

    SECTION("one connection")
    {
        EnvelopeItemMap eim;
        eim.add(envelope1, item1);

        auto expectedItems = std::set<ItemKey>{item1};
        auto expectedEnvelopes = std::set<SCPEnvelope>{envelope1};

        REQUIRE(eim.envelopes(item1) == expectedEnvelopes);

        SECTION("remove item")
        {
            REQUIRE(eim.remove(item1) == expectedEnvelopes);
        }

        SECTION("remove envelope")
        {
            REQUIRE(eim.remove(envelope1) == expectedItems);
        }

        auto emptyEnvelopes = std::set<SCPEnvelope>{};
        REQUIRE(eim.envelopes(item1) == emptyEnvelopes);
    }

    SECTION("multiple connections")
    {
        EnvelopeItemMap eim;
        eim.add(envelope1, item1);
        eim.add(envelope1, item2);
        eim.add(envelope2, item1);
        eim.add(envelope3, item2);

        auto envelope1Items = std::set<ItemKey>{item1, item2};
        auto envelope2Items = std::set<ItemKey>{item1};
        auto envelope3Items = std::set<ItemKey>{item2};
        auto item1Envelopes = std::set<SCPEnvelope>{envelope1, envelope2};
        auto item2Envelopes = std::set<SCPEnvelope>{envelope1, envelope3};

        REQUIRE(eim.envelopes(item1) == item1Envelopes);
        REQUIRE(eim.envelopes(item2) == item2Envelopes);

        SECTION("remove item1 then item2")
        {
            auto expectedEnvelopes = std::set<SCPEnvelope>{envelope2};
            REQUIRE(eim.remove(item1) == expectedEnvelopes);
            expectedEnvelopes = std::set<SCPEnvelope>{envelope1, envelope3};
            REQUIRE(eim.remove(item2) == expectedEnvelopes);
        }

        SECTION("remove item2 then item1")
        {
            auto expectedEnvelopes = std::set<SCPEnvelope>{envelope3};
            REQUIRE(eim.remove(item2) == expectedEnvelopes);
            expectedEnvelopes = std::set<SCPEnvelope>{envelope1, envelope2};
            REQUIRE(eim.remove(item1) == expectedEnvelopes);
        }

        SECTION("remove item1 then envelope1")
        {
            auto expectedEnvelopes = std::set<SCPEnvelope>{envelope2};
            REQUIRE(eim.remove(item1) == expectedEnvelopes);
            auto expectedItems = std::set<ItemKey>{};
            REQUIRE(eim.remove(envelope1) == expectedItems);
        }

        SECTION("remove item1 then envelope2")
        {
            auto expectedEnvelopes = std::set<SCPEnvelope>{envelope2};
            REQUIRE(eim.remove(item1) == expectedEnvelopes);
            auto expectedItems = std::set<ItemKey>{};
            REQUIRE(eim.remove(envelope2) == expectedItems);
        }

        SECTION("remove envelope1 then envelope2 then envelope3")
        {
            auto expectedItems = std::set<ItemKey>{};
            REQUIRE(eim.remove(envelope1) == expectedItems);
            expectedItems = std::set<ItemKey>{item1};
            REQUIRE(eim.remove(envelope2) == expectedItems);
            expectedItems = std::set<ItemKey>{item2};
            REQUIRE(eim.remove(envelope3) == expectedItems);
        }

        SECTION("remove envelope3 then envelope1 then envelope2")
        {
            auto expectedItems = std::set<ItemKey>{};
            REQUIRE(eim.remove(envelope3) == expectedItems);
            expectedItems = std::set<ItemKey>{item2};
            REQUIRE(eim.remove(envelope1) == expectedItems);
            expectedItems = std::set<ItemKey>{item1};
            REQUIRE(eim.remove(envelope2) == expectedItems);
        }

        SECTION("remove envelopes with slot less than 3")
        {
            auto expectedItems = std::set<ItemKey>{item1};
            auto removed = eim.removeIf([](SCPEnvelope const& envelope) {
                return envelope.statement.slotIndex < 3;
            });
            REQUIRE(removed == expectedItems);
        }
    }
}
