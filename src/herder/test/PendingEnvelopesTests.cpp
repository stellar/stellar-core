// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "herder/PendingEnvelopes.h"
#include "herder/test/TestTxSetUtils.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE("PendingEnvelopes recvSCPEnvelope", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.MANUAL_CLOSE = false;

    VirtualClock clock;

    auto s = SecretKey::pseudoRandomForTesting();
    auto& pk = s.getPublicKey();
    cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());
    Application::pointer app = createTestApplication(clock, cfg);

    auto const lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto root = TestAccount::createRoot(*app);
    size_t numAccounts = 50;
    std::vector<TestAccount> accs;
    for (size_t i = 0; i < numAccounts; i++)
    {
        accs.push_back(TestAccount{*app, getAccount("A" + std::to_string(i))});
    }

    using TxPair = std::pair<Value, TxSetXDRFrameConstPtr>;
    auto makeTxPair = [&](TxSetXDRFrameConstPtr txSet, uint64_t closeTime,
                          StellarValueType svt) {
        StellarValue sv = herder.makeStellarValue(
            txSet->getContentsHash(), closeTime, emptyUpgradeSteps, s);
        sv.ext.v(svt);
        auto v = xdr::xdr_to_opaque(sv);

        return TxPair{v, txSet};
    };
    auto makeEnvelope = [&](TxPair const& p, Hash qSetHash,
                            uint64_t slotIndex) {
        // herder must want the TxSet before receiving it, so we are sending it
        // fake envelope
        auto envelope = SCPEnvelope{};
        envelope.statement.slotIndex = slotIndex;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        auto& prep = envelope.statement.pledges.prepare();
        prep.ballot.counter = 1;
        prep.ballot.value = p.first;
        prep.quorumSetHash = qSetHash;
        envelope.statement.nodeID = s.getPublicKey();
        herder.signEnvelope(s, envelope);
        return envelope;
    };
    size_t index = 0;
    auto makeTransactions = [&](Hash hash, size_t n) {
        REQUIRE(n <= accs.size());
        std::vector<TransactionFrameBasePtr> txs(n);
        std::generate(std::begin(txs), std::end(txs),
                      [&]() { return accs[index++].tx({payment(root, 1)}); });
        return makeTxSetFromTransactions(txs, *app, 0, 0).first;
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

    auto txSet = makeTransactions(lcl.hash, numAccounts);
    auto p = makeTxPair(txSet, 10, STELLAR_VALUE_SIGNED);
    auto saneEnvelope = makeEnvelope(p, saneQSetHash, lcl.header.ledgerSeq + 1);
    auto bigEnvelope = makeEnvelope(p, bigQSetHash, lcl.header.ledgerSeq + 1);

    auto& pendingEnvelopes = herder.getPendingEnvelopes();

    SECTION("return FETCHING when first receiving envelope")
    {
        // check if the return value change only when it was READY on previous
        // call
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);

        SECTION("process when all data comes (quorum set first)")
        {
            REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            // still waiting for txset
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);

            REQUIRE(herder.getSCP().getLatestMessage(pk) == nullptr);
            // -> processes saneEnvelope
            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));

            auto m = herder.getSCP().getLatestMessage(pk);
            REQUIRE(m);
            REQUIRE(*m == saneEnvelope);

            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));

            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);
        }

        SECTION("process when all data came (tx set first)")
        {
            REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                               p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_FETCHING);

            REQUIRE(herder.getSCP().getLatestMessage(pk) == nullptr);

            // this triggers process
            REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            auto m = herder.getSCP().getLatestMessage(pk);
            REQUIRE(m);
            REQUIRE(*m == saneEnvelope);
            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);

            REQUIRE(!pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
            REQUIRE(!pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                p.second));
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);
        }
    }

    SECTION("process when data added manually")
    {
        SECTION("as not-removable")
        {
            pendingEnvelopes.addSCPQuorumSet(saneQSetHash, saneQSet);
            pendingEnvelopes.addTxSet(p.second->getContentsHash(), 0, p.second);
        }

        SECTION("as removable")
        {
            pendingEnvelopes.addSCPQuorumSet(saneQSetHash, saneQSet);
            pendingEnvelopes.addTxSet(
                p.second->getContentsHash(),
                2 * app->getConfig().MAX_SLOTS_TO_REMEMBER + 1, p.second);
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

    SECTION("different slots asking for same qset txset")
    {
        auto saneEnvelope2 = makeEnvelope(
            p, saneQSetHash,
            lcl.header.ledgerSeq + app->getConfig().MAX_SLOTS_TO_REMEMBER + 1);
        auto saneEnvelope3 =
            makeEnvelope(p, saneQSetHash,
                         lcl.header.ledgerSeq +
                             2 * app->getConfig().MAX_SLOTS_TO_REMEMBER + 1);

        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));

        // saneEnvelope gets processed
        REQUIRE(
            pendingEnvelopes.recvTxSet(p.second->getContentsHash(), p.second));
        auto m = herder.getSCP().getLatestMessage(pk);
        REQUIRE(m);
        REQUIRE(*m == saneEnvelope);
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                Herder::ENVELOPE_STATUS_PROCESSED);

        auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();
        auto lastCheckpointSeq =
            HistoryManager::lastLedgerBeforeCheckpointContaining(
                lclNum, app->getConfig());

        SECTION("with slotIndex difference less or equal than "
                "MAX_SLOTS_TO_REMEMBER")
        {
            pendingEnvelopes.eraseBelow(
                saneEnvelope2.statement.slotIndex -
                    app->getConfig().MAX_SLOTS_TO_REMEMBER,
                lastCheckpointSeq);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope2) ==
                    Herder::ENVELOPE_STATUS_READY);
            pendingEnvelopes.eraseBelow(
                saneEnvelope3.statement.slotIndex -
                    app->getConfig().MAX_SLOTS_TO_REMEMBER,
                lastCheckpointSeq);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                    Herder::ENVELOPE_STATUS_READY);
        }

        SECTION("with slotIndex difference bigger than MAX_SLOTS_TO_REMEMBER")
        {
            auto const minSlot = saneEnvelope3.statement.slotIndex -
                                 app->getConfig().MAX_SLOTS_TO_REMEMBER;
            pendingEnvelopes.eraseBelow(minSlot, lastCheckpointSeq);
            auto saneQSetP = pendingEnvelopes.getQSet(saneQSetHash);

            // 3 as we have "p", "txSet" and SCP
            REQUIRE(txSet.use_count() == 3);
            // 4 as we have "saneQSetP", SCP, cache and quorum tracker
            REQUIRE(saneQSetP.use_count() == 4);

            // clears SCP
            herder.getSCP().purgeSlots(minSlot, lastCheckpointSeq);
            REQUIRE(txSet.use_count() == 2);
            REQUIRE(saneQSetP.use_count() == 3);

            SECTION("With txset references")
            {
                SECTION("with qset")
                {
                    REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                            Herder::ENVELOPE_STATUS_READY);
                }
                SECTION("without qset")
                {
                    pendingEnvelopes.clearQSetCache();
                    // 2: "saneQSetP" and quorum tracker
                    REQUIRE(saneQSetP.use_count() == 2);
                    pendingEnvelopes.rebuildQuorumTrackerState();
                    REQUIRE(saneQSetP.use_count() == 1);
                    REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                            Herder::ENVELOPE_STATUS_FETCHING);
                }
            }
            SECTION("Without txset references")
            {
                // remove all references to that txset
                p.second.reset();
                txSet.reset();
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
            }
        }
    }

    SECTION("do not fetch if txsets are not signed")
    {
        auto p2 = makeTxPair(txSet, 10, STELLAR_VALUE_BASIC);
        auto envNoSign =
            makeEnvelope(p2, saneQSetHash, lcl.header.ledgerSeq + 1);

        // Make sure to discard the envelope
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(envNoSign) ==
                Herder::ENVELOPE_STATUS_DISCARDED);
    }

    SECTION("can receive malformed tx set")
    {
        GeneralizedTransactionSet malformedXdrSet(1);
        auto malformedTxSet = TxSetXDRFrame::makeFromWire(malformedXdrSet);
        auto p2 = makeTxPair(malformedTxSet, 10, STELLAR_VALUE_SIGNED);
        auto malformedEnvelope =
            makeEnvelope(p2, saneQSetHash, lcl.header.ledgerSeq + 1);
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(malformedEnvelope) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
        REQUIRE(herder.getSCP().getLatestMessage(pk) == nullptr);
        REQUIRE(pendingEnvelopes.recvTxSet(p2.second->getContentsHash(),
                                           p2.second));
    }
}
