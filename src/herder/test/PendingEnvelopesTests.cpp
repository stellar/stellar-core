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
    Application::pointer app = createTestApplication(clock, cfg);

    auto const& lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto root = TestAccount::createRoot(*app);
    auto a1 = TestAccount{*app, getAccount("A")};
    using TxPair = std::pair<Value, TxSetFramePtr>;
    auto makeTxPair = [](TxSetFramePtr txSet, uint64_t closeTime) {
        txSet->sortForHash();
        auto sv = StellarValue{txSet->getContentsHash(), closeTime,
                               emptyUpgradeSteps, 0};
        auto v = xdr::xdr_to_opaque(sv);

        return TxPair{v, txSet};
    };
    auto makeEnvelope = [&root](TxPair const& p, Hash qSetHash,
                                uint64_t slotIndex) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotIndex;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        envelope.statement.pledges.prepare().ballot.value = p.first;
        envelope.statement.pledges.prepare().quorumSetHash = qSetHash;
        envelope.signature =
            root.getSecretKey().sign(xdr::xdr_to_opaque(envelope.statement));
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

    auto transactions = makeTransactions(lcl.hash, 50);
    auto p = makeTxPair(transactions, 10);
    auto saneEnvelope = makeEnvelope(p, saneQSetHash, lcl.header.ledgerSeq + 1);
    auto bigEnvelope = makeEnvelope(p, bigQSetHash, lcl.header.ledgerSeq + 1);

    PendingEnvelopes pendingEnvelopes{
        *app, static_cast<HerderImpl&>(app->getHerder())};

    SECTION("return FETCHING when first receiving envelope")
    {
        // check if the return value change only when it was READY on previous
        // call
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);

        SECTION("and then READY when all data came (quorum set first)")
        {
            REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);

            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_READY);

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));

            SECTION("and then PROCESSED")
            {
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);
            }
        }

        SECTION("and then READY when all data came (tx set first)")
        {
            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);

            REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_READY);

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));

            SECTION("and then PROCESSED")
            {
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);
            }
        }
    }

    SECTION("return READY when receiving envelope with quorum set and tx set "
            "that were manually added before")
    {
        SECTION("as not-removable")
        {
            pendingEnvelopes.addSCPQuorumSet(saneQSetHash, saneQSet);
            pendingEnvelopes.addTxSet(p.second->getContentsHash(), 0, p.second);
        }

        SECTION("as removable")
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
            p, saneQSetHash,
            lcl.header.ledgerSeq + Herder::MAX_SLOTS_TO_REMEMBER + 1);
        auto saneEnvelope3 = makeEnvelope(
            p, saneQSetHash,
            lcl.header.ledgerSeq + 2 * Herder::MAX_SLOTS_TO_REMEMBER + 1);

        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
        REQUIRE(
            pendingEnvelopes.recvTxSet(p.second->getContentsHash(), p.second));
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_READY);

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
