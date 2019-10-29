// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "herder/PendingEnvelopes.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("PendingEnvelopes recvSCPEnvelope", "[herder]")
{
    Config cfg(getTestConfig());
    VirtualClock clock;

    auto s = SecretKey::pseudoRandomForTesting();
    cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());
    Application::pointer app = createTestApplication(clock, cfg);

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    auto& scp = herder.getSCP();

    auto root = TestAccount::createRoot(*app);
    auto a1 = TestAccount{*app, getAccount("A")};
    using TxPair = std::pair<Value, TxSetFramePtr>;
    auto makeTxPair = [](TxSetFramePtr txSet, uint64_t closeTime) {
        txSet->sortForHash();
        auto sv = StellarValue{txSet->getContentsHash(), closeTime,
                               emptyUpgradeSteps, STELLAR_VALUE_BASIC};
        auto v = xdr::xdr_to_opaque(sv);

        return TxPair{v, txSet};
    };
    auto makeEnvelope = [&s, &herder](uint32 c, TxPair const& p,
                                      Hash const& qSetHash,
                                      uint64_t slotIndex) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotIndex;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        auto& prep = envelope.statement.pledges.prepare();
        prep.ballot.counter = c;
        prep.ballot.value = p.first;
        prep.quorumSetHash = qSetHash;
        envelope.statement.nodeID = s.getPublicKey();
        herder.signEnvelope(s, envelope);
        return envelope;
    };
    auto addTransactions = [&](TxSetFramePtr txSet, int n) {
        txSet->mTransactions.resize(n);
        std::generate(std::begin(txSet->mTransactions),
                      std::end(txSet->mTransactions),
                      [&]() { return root.tx({createAccount(a1, 10000000)}); });
    };
    auto makeTransactions = [&](Hash hash, int n) {
        auto result = std::make_shared<TxSetFrame>(hash);
        addTransactions(result, n);
        return result;
    };

    auto makePublicKey = [](int i) {
        auto hash = sha256("NODE_SEED_" + std::to_string(i));
        auto secretKey = SecretKey::fromSeed(hash);
        return secretKey.getPublicKey();
    };

    auto makeSingleton = [](const PublicKey& key) {
        auto result = SCPQuorumSet{};
        result.threshold = 1;
        result.validators.push_back(key);
        return result;
    };

    auto keys = std::vector<PublicKey>{};
    for (auto i = 0; i < 1001; i++)
    {
        keys.push_back(makePublicKey(i));
    }

    auto saneQSet = makeSingleton(keys[0]);
    auto saneQSetHash = sha256(xdr::xdr_to_opaque(saneQSet));

    auto bigQSet = SCPQuorumSet{};
    bigQSet.threshold = 1;
    bigQSet.validators.push_back(keys[0]);
    for (auto i = 0; i < 10; i++)
    {
        bigQSet.innerSets.push_back({});
        bigQSet.innerSets.back().threshold = 1;
        for (auto j = i * 100 + 1; j <= (i + 1) * 100; j++)
            bigQSet.innerSets.back().validators.push_back(keys[j]);
    }
    auto bigQSetHash = sha256(xdr::xdr_to_opaque(bigQSet));

    auto sane2QSet = makeSingleton(keys[1]);
    auto sane2QSetHash = sha256(xdr::xdr_to_opaque(sane2QSet));

    auto transactions = makeTransactions(lcl.hash, 50);
    auto p = makeTxPair(transactions, 10);
    auto saneEnvelope =
        makeEnvelope(1, p, saneQSetHash, lcl.header.ledgerSeq + 1);
    auto bigEnvelope =
        makeEnvelope(2, p, bigQSetHash, lcl.header.ledgerSeq + 1);
    auto sane2Envelope =
        makeEnvelope(3, p, sane2QSetHash, lcl.header.ledgerSeq + 1);

    auto scpKnows = [&](SCPEnvelope const& e) {
        auto lastM = scp.getLatestMessage(e.statement.nodeID);
        return lastM != nullptr && *lastM == e;
    };

    auto& pendingEnvelopes = herder.getPendingEnvelopes();

    SECTION("receving envelope first")
    {
        REQUIRE(herder.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!pendingEnvelopes.isProcessed(saneEnvelope));
        REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));
        // still fetching on second call
        REQUIRE(herder.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(!pendingEnvelopes.isProcessed(saneEnvelope));
        REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));
        REQUIRE(pendingEnvelopes.getQSet(saneQSetHash) == nullptr);
        pendingEnvelopes.DropUnrefencedQsets();

        SECTION("processes when receive qset and then txset")
        {
            REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.isProcessed(saneEnvelope));
            REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));
            REQUIRE(*pendingEnvelopes.getQSet(saneQSetHash) == saneQSet);

            pendingEnvelopes.DropUnrefencedQsets();
            REQUIRE(*pendingEnvelopes.getQSet(saneQSetHash) == saneQSet);

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));

            pendingEnvelopes.DropUnrefencedQsets();
            REQUIRE(*pendingEnvelopes.getQSet(saneQSetHash) == saneQSet);

            REQUIRE(!pendingEnvelopes.isProcessed(saneEnvelope));
            REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));

            REQUIRE(!scpKnows(saneEnvelope));

            // tx set triggers processing
            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
            REQUIRE(scpKnows(saneEnvelope));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);
            REQUIRE(pendingEnvelopes.isProcessed(saneEnvelope));
            REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));

            // now receive sane2Envelope that depends on the same txset,
            // different qset

            REQUIRE(herder.recvSCPEnvelope(sane2Envelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);

            pendingEnvelopes.DropUnrefencedQsets();
            REQUIRE(*pendingEnvelopes.getQSet(saneQSetHash) == saneQSet);
            REQUIRE(pendingEnvelopes.getQSet(sane2QSetHash) == nullptr);

            // receive quorum set causes it to be processed
            REQUIRE(
                pendingEnvelopes.recvSCPQuorumSet(sane2QSetHash, sane2QSet));
            REQUIRE(!scpKnows(saneEnvelope));
            REQUIRE(scpKnows(sane2Envelope));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(sane2Envelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);

            pendingEnvelopes.DropUnrefencedQsets();
            REQUIRE(pendingEnvelopes.getQSet(saneQSetHash) == nullptr);
            REQUIRE(*pendingEnvelopes.getQSet(sane2QSetHash) == sane2QSet);
        }

        SECTION("processes when receive txset and then qset")
        {
            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));

            REQUIRE(!pendingEnvelopes.isProcessed(saneEnvelope));
            REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));
            REQUIRE(pendingEnvelopes.getQSet(saneQSetHash) == nullptr);

            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));

            REQUIRE(!scpKnows(saneEnvelope));

            // qset triggers processing
            REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));

            REQUIRE(*pendingEnvelopes.getQSet(saneQSetHash) == saneQSet);
            REQUIRE(scpKnows(saneEnvelope));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);
            REQUIRE(pendingEnvelopes.isProcessed(saneEnvelope));
            REQUIRE(!pendingEnvelopes.isReady(saneEnvelope));

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
        }
    }

    SECTION("return READY when receiving envelope with quorum set and tx set "
            "that were manually added before")
    {
        SECTION("txset not-removable")
        {
            pendingEnvelopes.addSCPQuorumSet(saneQSetHash, saneQSet);
            pendingEnvelopes.addTxSet(p.second->getContentsHash(), 0, p.second);
        }

        SECTION("txset removable")
        {
            pendingEnvelopes.addSCPQuorumSet(saneQSetHash, saneQSet);
            pendingEnvelopes.addTxSet(p.second->getContentsHash(),
                                      2 * Herder::MAX_SLOTS_TO_REMEMBER + 1,
                                      p.second);
        }

        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_READY);
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_PROCESSED);
    }

    SECTION("return DISCARDED when receiving envelope with too big quorum set")
    {
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(bigEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);

        SECTION("quorum set first")
        {
            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(bigQSetHash, bigQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_DISCARDED);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_DISCARDED);
        }

        SECTION("tx set first")
        {
            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));
            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(bigQSetHash, bigQSet));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_DISCARDED);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(bigEnvelope) ==
                    Herder::ENVELOPE_STATUS_DISCARDED);
        }
    }

    SECTION("envelopes from different slots asking for the same quorum set and "
            "tx set")
    {
        auto saneEnvelope2 = makeEnvelope(
            1, p, saneQSetHash,
            lcl.header.ledgerSeq + Herder::MAX_SLOTS_TO_REMEMBER + 1);
        auto saneEnvelope3 = makeEnvelope(
            1, p, saneQSetHash,
            lcl.header.ledgerSeq + 2 * Herder::MAX_SLOTS_TO_REMEMBER + 1);

        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
        REQUIRE(
            pendingEnvelopes.recvTxSet(p.second->getContentsHash(), p.second));
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_PROCESSED);

        SECTION("with slotIndex difference less or equal than "
                "MAX_SLOTS_TO_REMEMBER")
        {
            pendingEnvelopes.eraseBelow(saneEnvelope2.statement.slotIndex -
                                        Herder::MAX_SLOTS_TO_REMEMBER);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope2) ==
                    Herder::ENVELOPE_STATUS_READY);
            pendingEnvelopes.eraseBelow(saneEnvelope3.statement.slotIndex -
                                        Herder::MAX_SLOTS_TO_REMEMBER);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                    Herder::ENVELOPE_STATUS_READY);
        }

        SECTION("with slotIndex difference bigger than MAX_SLOTS_TO_REMEMBER")
        {
            pendingEnvelopes.eraseBelow(saneEnvelope3.statement.slotIndex -
                                        Herder::MAX_SLOTS_TO_REMEMBER);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
        }
    }
}
