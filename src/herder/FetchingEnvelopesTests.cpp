// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/FetchingEnvelopes.h"
#include "herder/HerderTestUtils.h"
#include "main/ApplicationImpl.h"
#include "test/test.h"

#include <lib/catch.hpp>
#include <xdrpp/marshal.h>

using namespace stellar;
using namespace stellar::HerderTestUtils;

namespace stellar
{
using xdr::operator<;
}

TEST_CASE("FetchingEnvelopes", "[herder][unit][FetchingEnvelopes]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    ApplicationImpl app{clock, cfg};
    app.initialize();

    auto saneQSet = makeSaneQuorumSet();
    auto saneQSetHash = sha256(xdr::xdr_to_opaque(saneQSet));
    auto bigQSet = makeBigQuorumSet();
    auto bigQSetHash = sha256(xdr::xdr_to_opaque(bigQSet));
    auto txSet = std::make_shared<TxSetFrame>(Hash{});
    auto txSetHash = txSet->getContentsHash();

    auto saneEnvelope = makeEnvelope(txSetHash, saneQSetHash, 4);
    auto bigEnvelope = makeEnvelope(txSetHash, bigQSetHash, 4);

    SECTION("start fetching when data not yet available")
    {
        FetchingEnvelopes fetchingEnvelopes{app};

        REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
        REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
    }

    SECTION("stop fetching after data comes")
    {
        FetchingEnvelopes fetchingEnvelopes{app};

        SECTION("quorum set first")
        {
            REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
            REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));

            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{saneEnvelope});
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));

            REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});

            REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
            REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
        }

        SECTION("tx set first")
        {
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));

            REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::set<SCPEnvelope>{saneEnvelope});
            REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));

            REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});

            REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
            REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
        }
    }

    SECTION("do not start fetching when data is already available")
    {
        FetchingEnvelopes fetchingEnvelopes{app};

        REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet, true) ==
                std::set<SCPEnvelope>{});
        REQUIRE(fetchingEnvelopes.handleTxSet(txSet, true) ==
                std::set<SCPEnvelope>{});

        REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
        REQUIRE(fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
    }

    SECTION("discard envelope with too big quorum set")
    {
        FetchingEnvelopes fetchingEnvelopes{app};

        REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, bigEnvelope));
        REQUIRE(!fetchingEnvelopes.isDiscarded(bigEnvelope));

        SECTION("quorum set first")
        {
            REQUIRE(fetchingEnvelopes.handleQuorumSet(bigQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.isDiscarded(bigEnvelope));
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.isDiscarded(bigEnvelope));
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, bigEnvelope));
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, bigEnvelope));
        }

        SECTION("tx set first")
        {
            REQUIRE(fetchingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(!fetchingEnvelopes.isDiscarded(bigEnvelope));
            REQUIRE(fetchingEnvelopes.handleQuorumSet(bigQSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(fetchingEnvelopes.isDiscarded(bigEnvelope));
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, bigEnvelope));
            REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, bigEnvelope));
        }
    }

    SECTION("do not mark old envelope ready")
    {
        FetchingEnvelopes fetchingEnvelopes{app};

        REQUIRE(fetchingEnvelopes.handleQuorumSet(saneQSet, true) ==
                std::set<SCPEnvelope>{});
        REQUIRE(fetchingEnvelopes.handleTxSet(txSet, true) ==
                std::set<SCPEnvelope>{});
        fetchingEnvelopes.setMinimumSlotIndex(5);
        REQUIRE(!fetchingEnvelopes.handleEnvelope(nullptr, saneEnvelope));
    }

    SECTION("discard all old envelopes")
    {
        FetchingEnvelopes fetchingEnvelopes{app};

        REQUIRE(!fetchingEnvelopes.isDiscarded(saneEnvelope));
        REQUIRE(!fetchingEnvelopes.isDiscarded(bigEnvelope));

        fetchingEnvelopes.setMinimumSlotIndex(5);
        REQUIRE(fetchingEnvelopes.isDiscarded(saneEnvelope));
        REQUIRE(fetchingEnvelopes.isDiscarded(bigEnvelope));
    }
}
