// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "herder/PendingEnvelopes.h"
#include "herder/test/TestTxSetUtils.h"
#include "main/Application.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "xdr/Stellar-ledger.h"
#include "xdrpp/marshal.h"

using namespace stellar;
using namespace stellar::txtest;

TEST_CASE_VERSIONS("PendingEnvelopes recvSCPEnvelope", "[herder]")
{
    Config cfg(getTestConfig());
    cfg.MANUAL_CLOSE = false;
    cfg.EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD = true;

    VirtualClock clock;

    auto s = SecretKey::pseudoRandomForTesting();
    auto& pk = s.getPublicKey();
    cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());
    Application::pointer app = createTestApplication(clock, cfg);

    auto const lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto root = app->getRoot();
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
        envelope.statement.pledges.type(SCP_ST_CONFIRM);
        auto& conf = envelope.statement.pledges.confirm();
        conf.ballot.counter = 1;
        conf.ballot.value = p.first;
        conf.nPrepared = 1;
        conf.nCommit = 1;
        conf.nH = 1;
        conf.quorumSetHash = qSetHash;
        envelope.statement.nodeID = s.getPublicKey();
        herder.signEnvelope(s, envelope);
        return envelope;
    };
    // PREPARE counterpart used by sections that explicitly exercise
    // parallel tx set downloading logic
    auto makePrepareEnvelope = [&](TxPair const& p, Hash qSetHash,
                                   uint64_t slotIndex) {
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
                      [&]() { return accs[index++].tx({payment(*root, 1)}); });
        return makeTxSetFromTransactions(txs, *app, 0, 0).first;
    };

    auto makePublicKey = [](int i) {
        auto hash = sha256("NODE_SEED_" + std::to_string(i));
        auto secretKey = SecretKey::fromSeed(hash);
        return secretKey.getPublicKey();
    };

    auto makeSingleton = [](PublicKey const& key) {
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

    SECTION("PREPARE: process before tx set arrives")
    {
        for_versions_from(
            static_cast<uint32_t>(SKIP_LEDGER_PROTOCOL_VERSION), *app, [&] {
                // Exercises the parallel tx set downloading code path: PREPARE
                // qualifies for early handoff to SCP once the qset is cached,
                // even while the tx set is still in flight
                auto prepEnv = makePrepareEnvelope(p, saneQSetHash,
                                                   lcl.header.ledgerSeq + 1);
                auto& fetchTimer =
                    app->getMetrics().NewTimer({"scp", "fetch", "envelope"});
                auto const initialCount = fetchTimer.count();

                // Initial receipt: nothing cached → FETCHING
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                        Herder::ENVELOPE_STATUS_FETCHING);
                REQUIRE(herder.getSCP().getLatestMessage(pk) == nullptr);

                // Qset arrives. Envelope is now READY.
                REQUIRE(
                    pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
                REQUIRE(herder.getSCP().getLatestMessage(pk) != nullptr);

                // Re-feeds during this state return PROCESSED
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);

                // Fetch duration not yet recorded as tx set has not arrived
                REQUIRE(fetchTimer.count() == initialCount);

                // Tx set arrives
                REQUIRE(pendingEnvelopes.recvTxSet(p.second->getContentsHash(),
                                                   p.second));

                // Fetch duration recorded after tx set arrival
                REQUIRE(fetchTimer.count() == initialCount + 1);

                // Subsequent re-feeds remain idempotent.
                REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                        Herder::ENVELOPE_STATUS_PROCESSED);
            });
    }

    SECTION("PREPARE: ready immediately when both resources cached")
    {
        // qset and tx set arrive prior to the envelope
        pendingEnvelopes.addSCPQuorumSet(saneQSetHash, saneQSet);
        pendingEnvelopes.addTxSet(p.second->getContentsHash(), 0, p.second);

        auto prepEnv =
            makePrepareEnvelope(p, saneQSetHash, lcl.header.ledgerSeq + 1);
        auto& fetchTimer =
            app->getMetrics().NewTimer({"scp", "fetch", "envelope"});
        auto const initialCount = fetchTimer.count();

        // Envelope is immediately ready
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                Herder::ENVELOPE_STATUS_READY);
        REQUIRE(fetchTimer.count() == initialCount + 1);

        // Returns PROCESSED on re-feed.
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
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
            pendingEnvelopes.eraseOutsideRange(
                saneEnvelope2.statement.slotIndex -
                    app->getConfig().MAX_SLOTS_TO_REMEMBER,
                std::nullopt, lastCheckpointSeq);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope2) ==
                    Herder::ENVELOPE_STATUS_READY);
            pendingEnvelopes.eraseOutsideRange(
                saneEnvelope3.statement.slotIndex -
                    app->getConfig().MAX_SLOTS_TO_REMEMBER,
                std::nullopt, lastCheckpointSeq);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                    Herder::ENVELOPE_STATUS_READY);
        }

        SECTION("with slotIndex difference bigger than MAX_SLOTS_TO_REMEMBER")
        {
            auto const minSlot = saneEnvelope3.statement.slotIndex -
                                 app->getConfig().MAX_SLOTS_TO_REMEMBER;
            pendingEnvelopes.eraseOutsideRange(minSlot, std::nullopt,
                                               lastCheckpointSeq);
            auto saneQSetP = pendingEnvelopes.getQSet(saneQSetHash);

            // 3 as we have "p", "txSet" and SCP
            REQUIRE(txSet.use_count() == 3);
            // 4 as we have "saneQSetP", SCP, cache and quorum tracker
            REQUIRE(saneQSetP.use_count() == 4);

            // clears SCP
            herder.getSCP().purgeSlotsOutsideRange(minSlot, std::nullopt,
                                                   lastCheckpointSeq);
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

        SECTION("eraseOutsideRange with upper bound only")
        {
            // Receive saneEnvelope2 and saneEnvelope3 via PendingEnvelopes.
            // Since qset and txset are already cached from processing
            // saneEnvelope, both go to READY.
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope2) ==
                    Herder::ENVELOPE_STATUS_READY);
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                    Herder::ENVELOPE_STATUS_READY);

            // Erase everything above saneEnvelope2's slot.
            auto const maxSlot = saneEnvelope2.statement.slotIndex;
            pendingEnvelopes.eraseOutsideRange(std::nullopt, maxSlot,
                                               lastCheckpointSeq);
            herder.getSCP().purgeSlotsOutsideRange(std::nullopt, maxSlot,
                                                   lastCheckpointSeq);

            // saneEnvelope is still PROCESSED
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);

            // saneEnvelope2 is still PROCESSED
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope2) ==
                    Herder::ENVELOPE_STATUS_PROCESSED);

            // saneEnvelope3 was erased.
            // Re-receiving it goes to READY (txset/qset still resolvable
            // via refs held by in-range slots).
            REQUIRE(pendingEnvelopes.recvSCPEnvelope(saneEnvelope3) ==
                    Herder::ENVELOPE_STATUS_READY);
        }

        SECTION("eraseOutsideRange with both bounds")
        {
            // Erase everything outside [envelope2.slot, envelope2.slot],
            // This erases:
            //   - saneEnvelope
            //   - txset cache entry
            // and purges SCP slot 2.
            auto const boundSlot = saneEnvelope2.statement.slotIndex;
            pendingEnvelopes.eraseOutsideRange(boundSlot, boundSlot,
                                               lastCheckpointSeq);

            auto saneQSetP = pendingEnvelopes.getQSet(saneQSetHash);

            // txSet refs: "p", "txSet", SCP (saneEnvelope's slot still in SCP)
            REQUIRE(txSet.use_count() == 3);
            // qSet refs: "saneQSetP", SCP, cache, quorum tracker
            REQUIRE(saneQSetP.use_count() == 4);

            // Purge SCP: saneEnvelope's slot removed
            herder.getSCP().purgeSlotsOutsideRange(boundSlot, boundSlot,
                                                   lastCheckpointSeq);
            REQUIRE(txSet.use_count() == 2);
            REQUIRE(saneQSetP.use_count() == 3);
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

    SECTION("skip-value envelopes gated by SKIP_LEDGER_PROTOCOL_VERSION")
    {
        // Build a skip-value envelope by wrapping a signed StellarValue with
        // makeSkipLedgerValueFromValue. The skip XDR is well-formed at any
        // protocol version; whether `recvSCPEnvelope` admits it is what's
        // gated.
        auto& scpDriver = herder.getHerderSCPDriver();
        Value skipValue = scpDriver.makeSkipLedgerValueFromValue(p.first);
        auto skipEnvelope =
            makeEnvelope(TxPair{skipValue, p.second}, saneQSetHash,
                         lcl.header.ledgerSeq + 1);

        SECTION("rejected before SKIP_LEDGER_PROTOCOL_VERSION")
        {
            for_versions_to(
                static_cast<uint32_t>(SKIP_LEDGER_PROTOCOL_VERSION) - 1, *app,
                [&] {
                    REQUIRE(pendingEnvelopes.recvSCPEnvelope(skipEnvelope) ==
                            Herder::ENVELOPE_STATUS_DISCARDED);
                });
        }

        SECTION("accepted at SKIP_LEDGER_PROTOCOL_VERSION and beyond")
        {
            for_versions_from(
                static_cast<uint32_t>(SKIP_LEDGER_PROTOCOL_VERSION), *app,
                [&] {
                    // Passes the value-type filter; qset is not cached so
                    // the envelope still has work to do. Assert it's not
                    // DISCARDED rather than asserting a specific status,
                    // since the downstream outcome depends on details of
                    // the tx-set fetcher's treatment of SKIP_LEDGER_HASH.
                    REQUIRE(pendingEnvelopes.recvSCPEnvelope(skipEnvelope) !=
                            Herder::ENVELOPE_STATUS_DISCARDED);
                });
        }
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

    SECTION("value wrapper keeps tx set alive via onTxSetReceived")
    {
        // The tx set exists but is NOT in the herder's known tx set cache.
        // When wrapStellarValue/wrapValue is called, the wrapper won't find
        // the tx set and will register in mPendingTxSetWrappers for later
        // update via onTxSetReceived().
        auto& scpDriver = herder.getHerderSCPDriver();
        auto txSetHash = txSet->getContentsHash();

        StellarValue sv =
            herder.makeStellarValue(txSetHash, 10, emptyUpgradeSteps, s);

        SECTION("wrapStellarValue registers and receives tx set")
        {
            // "txSet" and "p.second" hold the only references
            REQUIRE(txSet.use_count() == 2);

            // Wrap the value - tx set is not in herder's cache, so the
            // wrapper registers in mPendingTxSetWrappers
            auto wrapper = scpDriver.wrapStellarValue(sv);

            // Wrapper doesn't have the tx set yet, ref count unchanged
            REQUIRE(txSet.use_count() == 2);

            // Deliver the tx set via onTxSetReceived
            scpDriver.onTxSetReceived(txSetHash, txSet);

            // Now the wrapper holds a reference to the tx set
            REQUIRE(txSet.use_count() == 3);

            // Dropping the wrapper releases its reference
            wrapper.reset();
            REQUIRE(txSet.use_count() == 2);
        }

        SECTION("wrapValue registers and receives tx set")
        {
            REQUIRE(txSet.use_count() == 2);

            auto wrapper = scpDriver.wrapValue(p.first);

            REQUIRE(txSet.use_count() == 2);

            scpDriver.onTxSetReceived(txSetHash, txSet);

            REQUIRE(txSet.use_count() == 3);

            wrapper.reset();
            REQUIRE(txSet.use_count() == 2);
        }

        SECTION("multiple wrappers all receive tx set")
        {
            REQUIRE(txSet.use_count() == 2);

            auto wrapper1 = scpDriver.wrapStellarValue(sv);
            auto wrapper2 = scpDriver.wrapStellarValue(sv);

            // Neither wrapper has the tx set yet
            REQUIRE(txSet.use_count() == 2);

            scpDriver.onTxSetReceived(txSetHash, txSet);

            // Both wrappers now hold a reference
            REQUIRE(txSet.use_count() == 4);

            wrapper1.reset();
            REQUIRE(txSet.use_count() == 3);

            wrapper2.reset();
            REQUIRE(txSet.use_count() == 2);
        }

        SECTION("expired wrapper does not leak tx set")
        {
            REQUIRE(txSet.use_count() == 2);

            auto wrapper = scpDriver.wrapStellarValue(sv);
            // Drop the wrapper before the tx set arrives
            wrapper.reset();

            REQUIRE(txSet.use_count() == 2);

            // The weak_ptr in the registry has expired, so no update occurs
            scpDriver.onTxSetReceived(txSetHash, txSet);

            // No leak - ref count unchanged
            REQUIRE(txSet.use_count() == 2);
        }
    }
}

// TODO: There's a lot of duplicate setup code in this test. Refactor
TEST_CASE_VERSIONS(
    "PendingEnvelopes recvSCPEnvelope without parallel tx set download",
    "[herder]")
{
    // Mirrors the gates-on TEST_CASE above, but leaves
    // EXPERIMENTAL_PARALLEL_TX_SET_DOWNLOAD at its default (false). With Gate
    // B off, a PREPARE envelope missing its tx set must stay in FETCHING
    // even when the qset is cached -- the inverse of the gates-on test in
    // "PREPARE: process before tx set arrives".
    Config cfg(getTestConfig());
    cfg.MANUAL_CLOSE = false;

    VirtualClock clock;

    auto s = SecretKey::pseudoRandomForTesting();
    auto& pk = s.getPublicKey();
    cfg.QUORUM_SET.validators.emplace_back(s.getPublicKey());
    Application::pointer app = createTestApplication(clock, cfg);

    auto const lcl = app->getLedgerManager().getLastClosedLedgerHeader();

    auto& herder = static_cast<HerderImpl&>(app->getHerder());

    auto root = app->getRoot();
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
    auto makePrepareEnvelope = [&](TxPair const& p, Hash qSetHash,
                                   uint64_t slotIndex) {
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
                      [&]() { return accs[index++].tx({payment(*root, 1)}); });
        return makeTxSetFromTransactions(txs, *app, 0, 0).first;
    };

    auto makePublicKey = [](int i) {
        auto hash = sha256("NODE_SEED_" + std::to_string(i));
        auto secretKey = SecretKey::fromSeed(hash);
        return secretKey.getPublicKey();
    };

    auto saneQSet = SCPQuorumSet{};
    saneQSet.threshold = 1;
    saneQSet.validators.push_back(makePublicKey(0));
    auto saneQSetHash = sha256(xdr::xdr_to_opaque(saneQSet));

    auto txSet = makeTransactions(lcl.hash, numAccounts);
    auto p = makeTxPair(txSet, 10, STELLAR_VALUE_SIGNED);

    auto& pendingEnvelopes = herder.getPendingEnvelopes();

    SECTION("PREPARE stays in FETCHING without tx set, even with qset cached")
    {
        auto prepEnv =
            makePrepareEnvelope(p, saneQSetHash, lcl.header.ledgerSeq + 1);

        // Initial receipt: nothing cached → FETCHING
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(herder.getSCP().getLatestMessage(pk) == nullptr);

        // Qset arrives. With Gate B off, the envelope must STAY in FETCHING
        // (and SCP must not see it yet). This is the inverse of the
        // gates-on test in the TEST_CASE above, which asserts SCP did
        // receive the envelope at exactly this point.
        REQUIRE(pendingEnvelopes.recvSCPQuorumSet(saneQSetHash, saneQSet));
        REQUIRE(pendingEnvelopes.recvSCPEnvelope(prepEnv) ==
                Herder::ENVELOPE_STATUS_FETCHING);
        REQUIRE(herder.getSCP().getLatestMessage(pk) == nullptr);

        // Tx set arrives — envelope becomes READY.
        REQUIRE(
            pendingEnvelopes.recvTxSet(p.second->getContentsHash(), p.second));
        REQUIRE(herder.getSCP().getLatestMessage(pk) != nullptr);
    }
}
