// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/HerderImpl.h"
#include "herder/QuorumTracker.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "scp/SCP.h"
#include "test/TestUtils.h"
#include "test/test.h"

using namespace stellar;

TEST_CASE("quorum tracker", "[quorum][herder]")
{
    Config cfg(getTestConfig());

    std::vector<SecretKey> otherKeys;
    int const kKeysCount = 7;
    for (int i = 0; i < kKeysCount; i++)
    {
        otherKeys.emplace_back(SecretKey::pseudoRandomForTesting());
    }

    auto buildQSet = [&](int i) {
        SCPQuorumSet q;
        q.threshold = 2;
        q.validators.emplace_back(otherKeys[i].getPublicKey());
        q.validators.emplace_back(otherKeys[i + 1].getPublicKey());
        return q;
    };

    cfg.QUORUM_SET = buildQSet(0);
    auto qSet0 = buildQSet(2);
    auto qSet0b = buildQSet(4);
    auto qSet2 = buildQSet(5);
    qSet2.threshold++;
    qSet2.validators.emplace_back(cfg.NODE_SEED.getPublicKey());

    auto clock = std::make_shared<VirtualClock>();
    Application::pointer app = createTestApplication(*clock, cfg);

    app->start();

    auto* herder = static_cast<HerderImpl*>(&app->getHerder());
    auto* penEnvs = &herder->getPendingEnvelopes();

    // allow SCP messages from other slots to be processed
    herder->getHerderSCPDriver().lostSync();

    using TxPair = std::pair<Value, TxSetFramePtr>;

    auto recvEnvelope = [&](uint64 slotID, SecretKey const& k,
                            SCPQuorumSet const& qSet, std::vector<TxPair> pp) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotID;
        envelope.statement.pledges.type(SCP_ST_NOMINATE);
        auto& nom = envelope.statement.pledges.nominate();

        std::set<Value> values;
        for (auto& p : pp)
        {
            values.insert(p.first);
        }
        nom.votes.insert(nom.votes.begin(), values.begin(), values.end());
        auto qSetH = sha256(xdr::xdr_to_opaque(qSet));
        nom.quorumSetHash = qSetH;
        envelope.statement.nodeID = k.getPublicKey();
        envelope.signature = k.sign(xdr::xdr_to_opaque(
            app->getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
        herder->recvSCPEnvelope(envelope);
        herder->recvSCPQuorumSet(qSetH, qSet);
        for (auto& p : pp)
        {
            herder->recvTxSet(p.second->getContentsHash(), *p.second);
        }
    };

    auto makeValue = [&](int i) {
        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = std::make_shared<TxSetFrame>(lcl.hash);
        auto sv = StellarValue{txSet->getContentsHash(),
                               lcl.header.scpValue.closeTime + i,
                               emptyUpgradeSteps, 0};
        auto v = xdr::xdr_to_opaque(sv);

        return TxPair{v, txSet};
    };

    auto vv = makeValue(1);

    auto checkInQuorum = [&](std::set<int> ids) {
        REQUIRE(
            penEnvs->isNodeDefinitelyInQuorum(cfg.NODE_SEED.getPublicKey()));
        for (int j = 0; j < kKeysCount; j++)
        {
            bool inQuorum = (ids.find(j) != ids.end());
            REQUIRE(penEnvs->isNodeDefinitelyInQuorum(
                        otherKeys[j].getPublicKey()) == inQuorum);
        }
    };
    SECTION("Receive self")
    {
        checkInQuorum({0, 1});
        recvEnvelope(3, cfg.NODE_SEED, cfg.QUORUM_SET, {vv});
        checkInQuorum({0, 1});
    }
    SECTION("Expand 0")
    {
        checkInQuorum({0, 1});
        recvEnvelope(3, otherKeys[0], qSet0, {vv});
        checkInQuorum({0, 1, 2, 3});
        SECTION("Expand 2")
        {
            recvEnvelope(3, otherKeys[2], qSet2, {vv});
            checkInQuorum({0, 1, 2, 3, 5, 6});
        }
        SECTION("Update 0's qSet")
        {
            auto vv2 = makeValue(2);
            recvEnvelope(3, otherKeys[0], qSet0b, {vv, vv2});
            checkInQuorum({0, 1, 4, 5});
        }
        SECTION("Update 0's qSet in an old slot")
        {
            auto vv2 = makeValue(2);
            recvEnvelope(2, otherKeys[0], qSet0b, {vv, vv2});
            // nothing changes (slot 3 has precedence)
            checkInQuorum({0, 1, 2, 3});
        }
    }
}
