// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "main/ApplicationImpl.h"
#include "overlay/OverlayManager.h"
#include "overlay/PendingEnvelopes.h"
#include "test/TestUtils.h"
#include "test/test.h"

#include <lib/catch.hpp>
#include <xdrpp/marshal.h>

using namespace stellar;
using namespace stellar::testutil;

TEST_CASE("PendingEnvelopes", "[herder][unit][PendingEnvelopes]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;
    ApplicationImpl app{clock, cfg};
    app.initialize();

    auto saneQSet = makeSingletonQuorumSet(makePublicKey(0));
    auto saneQSetHash = sha256(xdr::xdr_to_opaque(saneQSet));
    auto bigQSet = makeBigQuorumSet();
    auto bigQSetHash = sha256(xdr::xdr_to_opaque(bigQSet));
    TransactionSet txSet;
    auto txSetFrame = TxSetFrame{app.getNetworkID(), txSet};
    auto txSetHash = txSetFrame.getContentsHash();

    auto saneEnvelope = makeEnvelope(txSetHash, saneQSetHash, 4);
    auto bigEnvelope = makeEnvelope(txSetHash, bigQSetHash, 4);

    auto& pendingEnvelopes = app.getOverlayManager().getPendingEnvelopes();

    SECTION("return FETCHING when first receiving envelope")
    {
        // check if the return value change only when it was READY on previous
        // call
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_FETCHING);

        SECTION("and then READY when all data came (quorum set first)")
        {
            REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::make_pair(true, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_FETCHING);
            REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::make_pair(false, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_FETCHING);

            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{saneEnvelope});
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_READY);

            REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::make_pair(false, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});

            SECTION("and then READY again")
            {
                REQUIRE(
                    pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_READY);
                REQUIRE(
                    pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_READY);
            }
        }

        SECTION("and then READY when all data came (tx set first)")
        {
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_FETCHING);
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_FETCHING);

            REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::make_pair(true, std::set<SCPEnvelope>{saneEnvelope}));
            REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::make_pair(false, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_READY);

            REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                    std::make_pair(false, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});

            SECTION("and then READY again")
            {
                REQUIRE(
                    pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_READY);
                REQUIRE(
                    pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_READY);
            }
        }
    }

    SECTION("return READY when receiving envelope with quorum set and tx "
            "set that were manually added before")
    {
        REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet, true) ==
                std::make_pair(true, std::set<SCPEnvelope>{}));
        REQUIRE(pendingEnvelopes.handleTxSet(txSet, true) ==
                std::set<SCPEnvelope>{});

        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_READY);
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_READY);
    }

    SECTION("return DISCARDED when receiving envelope with too big quorum set")
    {
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, bigEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_FETCHING);

        SECTION("quorum set first")
        {
            REQUIRE(pendingEnvelopes.handleQuorumSet(bigQSet) ==
                    std::make_pair(false, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, bigEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_DISCARDED);
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, bigEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_DISCARDED);
        }

        SECTION("tx set first")
        {
            REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                    std::set<SCPEnvelope>{});
            REQUIRE(pendingEnvelopes.handleQuorumSet(bigQSet) ==
                    std::make_pair(false, std::set<SCPEnvelope>{}));
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, bigEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_DISCARDED);
            REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, bigEnvelope) ==
                    EnvelopeHandler::ENVELOPE_STATUS_DISCARDED);
        }
    }

    SECTION("envelopes from different slots asking for the same quorum set and "
            "tx set")
    {
        auto saneEnvelope2 = makeEnvelope(txSetHash, saneQSetHash, 5);
        auto saneEnvelope3 = makeEnvelope(txSetHash, saneQSetHash, 6);

        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.handleQuorumSet(saneQSet) ==
                std::make_pair(true, std::set<SCPEnvelope>{}));
        REQUIRE(pendingEnvelopes.handleTxSet(txSet) ==
                std::set<SCPEnvelope>{saneEnvelope});
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope) ==
                EnvelopeHandler::ENVELOPE_STATUS_READY);
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope2) ==
                EnvelopeHandler::ENVELOPE_STATUS_READY);
        REQUIRE(pendingEnvelopes.handleEnvelope(nullptr, saneEnvelope3) ==
                EnvelopeHandler::ENVELOPE_STATUS_READY);
    }
}
