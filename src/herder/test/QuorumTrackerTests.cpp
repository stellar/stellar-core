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
#include "xdr/Stellar-ledger.h"

using namespace stellar;

void
testQuorumTracker()
{
    Config cfg(getTestConfig(0, Config::TESTDB_ON_DISK_SQLITE));
    cfg.MANUAL_CLOSE = false;

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

    auto valSigner = SecretKey::pseudoRandomForTesting();

    struct ValuesTxSet
    {
        Value mSignedV;
        TxSetFramePtr mTxSet;
    };

    auto recvEnvelope = [&](SCPEnvelope envelope, uint64 slotID,
                            SecretKey const& k, SCPQuorumSet const& qSet,
                            std::vector<ValuesTxSet> const& pp) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        envelope.statement.slotIndex = slotID;
        auto qSetH = sha256(xdr::xdr_to_opaque(qSet));
        envelope.statement.nodeID = k.getPublicKey();
        envelope.signature = k.sign(xdr::xdr_to_opaque(
            app->getNetworkID(), ENVELOPE_TYPE_SCP, envelope.statement));
        herder->recvSCPEnvelope(envelope);
        herder->recvSCPQuorumSet(qSetH, qSet);
        for (auto& p : pp)
        {
            herder->recvTxSet(p.mTxSet->getContentsHash(), *p.mTxSet);
        }
    };
    auto recvNom = [&](uint64 slotID, SecretKey const& k,
                       SCPQuorumSet const& qSet,
                       std::vector<ValuesTxSet> const& pp) {
        SCPEnvelope envelope;
        envelope.statement.pledges.type(SCP_ST_NOMINATE);
        auto& nom = envelope.statement.pledges.nominate();

        std::set<Value> values;
        for (auto& p : pp)
        {
            values.insert(p.mSignedV);
        }
        nom.votes.insert(nom.votes.begin(), values.begin(), values.end());
        auto qSetH = sha256(xdr::xdr_to_opaque(qSet));
        nom.quorumSetHash = qSetH;
        recvEnvelope(envelope, slotID, k, qSet, pp);
    };
    auto recvExternalize = [&](uint64 slotID, SecretKey const& k,
                               SCPQuorumSet const& qSet, ValuesTxSet const& v) {
        SCPEnvelope envelope;
        envelope.statement.pledges.type(SCP_ST_EXTERNALIZE);
        auto& ext = envelope.statement.pledges.externalize();
        ext.commit.counter = UINT32_MAX;
        ext.commit.value = v.mSignedV;
        ext.nH = UINT32_MAX;

        auto qSetH = sha256(xdr::xdr_to_opaque(qSet));
        ext.commitQuorumSetHash = qSetH;
        std::vector<ValuesTxSet> pp = {v};
        recvEnvelope(envelope, slotID, k, qSet, pp);
    };
    auto makeValue = [&](int i) {
        auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        auto txSet = std::make_shared<TxSetFrame>(lcl.hash);
        StellarValue sv = herder->makeStellarValue(
            txSet->getContentsHash(), lcl.header.scpValue.closeTime + i,
            emptyUpgradeSteps, valSigner);
        auto v = xdr::xdr_to_opaque(sv);
        return ValuesTxSet{v, txSet};
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
        recvNom(3, cfg.NODE_SEED, cfg.QUORUM_SET, {vv});
        checkInQuorum({0, 1});
    }
    SECTION("Expand 0")
    {
        checkInQuorum({0, 1});
        recvNom(3, otherKeys[0], qSet0, {vv});
        checkInQuorum({0, 1, 2, 3});
        SECTION("Expand 2")
        {
            recvNom(3, otherKeys[2], qSet2, {vv});
            checkInQuorum({0, 1, 2, 3, 5, 6});
            SECTION("node restart")
            {
                // externalize -> we persist quorum information
                recvExternalize(3, otherKeys[0], qSet0, vv);
                // use qSet0 for node1
                recvExternalize(3, otherKeys[1], qSet0, vv);
                checkInQuorum({0, 1, 2, 3, 5, 6});
                app.reset();
                clock.reset();

                clock = std::make_shared<VirtualClock>();
                app = Application::create(*clock, cfg, false);
                app->start();
                herder = static_cast<HerderImpl*>(&app->getHerder());
                penEnvs = &herder->getPendingEnvelopes();
                checkInQuorum({0, 1, 2, 3, 5, 6});
            }
        }
        SECTION("Update 0's qSet")
        {
            auto vv2 = makeValue(2);
            recvNom(3, otherKeys[0], qSet0b, {vv, vv2});
            checkInQuorum({0, 1, 4, 5});
        }
        SECTION("Update 0's qSet in an old slot")
        {
            auto vv2 = makeValue(2);
            recvNom(2, otherKeys[0], qSet0b, {vv, vv2});
            // nothing changes (slot 3 has precedence)
            checkInQuorum({0, 1, 2, 3});
        }
    }
}

TEST_CASE("quorum tracker", "[quorum][herder]")
{
    testQuorumTracker();
}

TEST_CASE("quorum tracker closest validators", "[quorum][herder]")
{
    Config cfg(getTestConfig(0, Config::TESTDB_IN_MEMORY_SQLITE));

    std::vector<PublicKey> otherKeys;
    int const kKeysCount = 7;
    otherKeys.push_back(cfg.NODE_SEED.getPublicKey());
    int self = 0;
    for (int i = self + 1; i < kKeysCount; i++)
    {
        otherKeys.emplace_back(
            SecretKey::pseudoRandomForTesting().getPublicKey());
    }

    auto makeQset = [&](std::vector<int> const& validatorIndexes,
                        int selfIndex) {
        SCPQuorumSet q;
        q.threshold = static_cast<uint32>(validatorIndexes.size()) + 1;
        std::transform(validatorIndexes.begin(), validatorIndexes.end(),
                       std::back_inserter(q.validators),
                       [&](int i) { return otherKeys[i]; });
        q.validators.push_back(otherKeys[self]);
        return std::make_shared<SCPQuorumSet>(q);
    };

    auto selfQSet = makeQset({2, 1}, self);
    cfg.QUORUM_SET = *selfQSet;

    auto clock = std::make_shared<VirtualClock>();
    Application::pointer app = createTestApplication(*clock, cfg);
    app->start();

    auto* herder = static_cast<HerderImpl*>(&app->getHerder());
    auto const localNodeID = herder->getSCP().getLocalNodeID();
    QuorumTracker qt(localNodeID);

    auto lookup = [&](NodeID const& node) -> SCPQuorumSetPtr {
        if (node == localNodeID)
        {
            return selfQSet;
        }
        auto it = std::find(otherKeys.begin(), otherKeys.end(), node);
        auto idx = std::distance(otherKeys.begin(), it);
        switch (idx)
        {
        case 0:
            return selfQSet;
        case 1:
            return makeQset({self, 2, 4, 5}, 1);
        case 2:
            return makeQset({self, 1, 5}, 2);
        case 5:
            return makeQset({1, 3, 4}, 5);
        case 4:
            return makeQset({2}, 4);
        case 3:
        case 6:
            return std::make_shared<SCPQuorumSet>();
        default:
            abort();
        }
    };

    auto checkRes = [&](NodeID const& key, std::set<NodeID> const& otherKeys,
                        bool notFound = false) {
        auto hasValidators = qt.isNodeDefinitelyInQuorum(key);
        if (notFound)
        {
            REQUIRE(!hasValidators);
        }
        else
        {
            REQUIRE(hasValidators);
            REQUIRE(otherKeys == qt.findClosestValidators(key));
        }
    };

    auto validateRebuildResult = [&]() {
        checkRes(otherKeys[1], std::set<NodeID>{otherKeys[1]});
        checkRes(otherKeys[2], std::set<NodeID>{otherKeys[2]});

        checkRes(otherKeys[3], std::set<NodeID>{otherKeys[1], otherKeys[2]});
        checkRes(otherKeys[4], std::set<NodeID>{otherKeys[1]});
        checkRes(otherKeys[5], std::set<NodeID>{otherKeys[1], otherKeys[2]});

        // No path for node 6
        checkRes(otherKeys[6], {}, true);
    };

    // Rebuilding quorum map from scratch yields optimal distances
    SECTION("rebuild")
    {
        qt.rebuild(lookup);
        validateRebuildResult();
    }
    // Incrementally expand quorum map: depending on the order of expansion,
    // intermediate "closest validators" might be suboptimal.
    // If finished successfully, result must be identical to
    // rebuilding from scratch (thus yielding optimal distances)
    SECTION("expand")
    {
        // Rebuild only knowing about "self"
        qt.rebuild([&](NodeID const& node) -> SCPQuorumSetPtr {
            if (node == cfg.NODE_SEED.getPublicKey())
            {
                return selfQSet;
            }
            else
            {
                return nullptr;
            }
        });
        checkRes(otherKeys[1], std::set<NodeID>{otherKeys[1]});
        checkRes(otherKeys[2], std::set<NodeID>{otherKeys[2]});
        checkRes(otherKeys[3], {}, true);
        checkRes(otherKeys[4], {}, true);
        checkRes(otherKeys[5], {}, true);
        checkRes(otherKeys[6], {}, true);

        SECTION("cannot expand some")
        {
            // Now expand qSets for 2 and 5
            REQUIRE(qt.expand(otherKeys[2], lookup(otherKeys[2])));
            REQUIRE(qt.expand(otherKeys[5], lookup(otherKeys[5])));

            // Distances are not optimal yet because some parts of
            // the quorum are unknown
            checkRes(otherKeys[1], std::set<NodeID>{otherKeys[1]});
            checkRes(otherKeys[2], std::set<NodeID>{otherKeys[2]});

            // "2" is associated with "3" (shortest path not discovered yet)
            checkRes(otherKeys[3], std::set<NodeID>{otherKeys[2]});
            // "2" is associated with "4" (shortest path not discovered yet)
            checkRes(otherKeys[4], std::set<NodeID>{otherKeys[2]});
            checkRes(otherKeys[5], std::set<NodeID>{otherKeys[2]});
            checkRes(otherKeys[6], {}, true);

            // Now learn about more qSets, but `expand` should return false
            // because closest validators for 3 could not be updated
            REQUIRE_FALSE(qt.expand(otherKeys[1], lookup(otherKeys[1])));

            checkRes(otherKeys[1], std::set<NodeID>{otherKeys[1]});
            checkRes(otherKeys[2], std::set<NodeID>{otherKeys[2]});

            checkRes(otherKeys[4], std::set<NodeID>{otherKeys[1]});

            // 5, 3 failed to update
            checkRes(otherKeys[3], std::set<NodeID>{otherKeys[2]});
            checkRes(otherKeys[5], std::set<NodeID>{otherKeys[2]});
            checkRes(otherKeys[6], {}, true);

            // Ensure rebuild runs correctly after `expand` attempts
            qt.rebuild(lookup);
            validateRebuildResult();
        }
        SECTION("expand success")
        {
            // Even though distances are not expanded by shortest distance,
            // still arrive at the same result as rebuild
            // "0" and "4" are expanded first
            REQUIRE(qt.expand(otherKeys[1], lookup(otherKeys[1])));
            REQUIRE(qt.expand(otherKeys[4], lookup(otherKeys[4])));

            // "1" and "5" are expanded next, success
            REQUIRE(qt.expand(otherKeys[2], lookup(otherKeys[2])));
            REQUIRE(qt.expand(otherKeys[5], lookup(otherKeys[5])));
            validateRebuildResult();
        }
    }
}
