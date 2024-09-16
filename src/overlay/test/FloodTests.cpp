// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/test/BucketTestUtils.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerDoor.h"
#include "overlay/TCPPeer.h"
#include "overlay/test/OverlayTestUtils.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/TestAccount.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Logging.h"
#include "util/Timer.h"
#include "xdrpp/marshal.h"

namespace stellar
{
using namespace txtest;

TEST_CASE("Flooding", "[flood][overlay][acceptance]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation;

    const int nbTx = 100;

    std::vector<TestAccount> sources;
    SequenceNumber expectedSeq = 0;

    std::vector<std::shared_ptr<Application>> nodes;

    auto test = [&](std::function<void(int)> inject,
                    std::function<bool(std::shared_ptr<Application>)> acked,
                    bool syncNodes) {
        simulation->startAllNodes();

        nodes = simulation->getNodes();
        std::shared_ptr<Application> app0 = nodes[0];

        auto root = TestAccount::createRoot(*app0);

        // directly create a bunch of accounts by cloning the root account (one
        // per tx so that we can easily identify them)
        {
            LedgerEntry gen;
            {
                LedgerTxn ltx(app0->getLedgerTxnRoot());
                gen = stellar::loadAccount(ltx, root.getPublicKey()).current();
            }

            for (int i = 0; i < nbTx; i++)
            {
                sources.emplace_back(
                    TestAccount{*app0, SecretKey::pseudoRandomForTesting(), 0});
                gen.data.account().accountID = sources.back();

                // need to create on all nodes
                for (auto n : nodes)
                {
                    auto const& header = n->getLedgerManager()
                                             .getLastClosedLedgerHeader()
                                             .header;
                    BucketTestUtils::addLiveBatchAndUpdateSnapshot(
                        *n, header, {}, {gen}, {});
                }
            }
        }

        if (syncNodes)
        {
            // Wait until all nodes externalize
            simulation->crankUntil(
                [&]() { return simulation->haveAllExternalized(2, 1); },
                std::chrono::seconds(1), false);
            for (auto const& n : nodes)
            {
                REQUIRE(n->getLedgerManager().isSynced());
            }
        }
        else
        {
            // enough for connections to be made
            simulation->crankForAtLeast(std::chrono::seconds(1), false);
        }

        expectedSeq = root.getLastSequenceNumber() + 1;

        LOG_DEBUG(DEFAULT_LOG, "Injecting work");

        // inject transactions
        for (int i = 0; i < nbTx; i++)
        {
            inject(i);
        }

        LOG_DEBUG(DEFAULT_LOG, "Done injecting work");

        auto checkSim = [&]() {
            bool res = true;
            for (auto n : simulation->getNodes())
            {
                // done in this order to display full list
                res = acked(n) && res;
            }
            return res;
        };

        // see if the transactions got propagated properly
        simulation->crankUntil(checkSim, std::chrono::seconds(60), false);

        for (auto n : nodes)
        {
            auto& m = n->getMetrics();
            std::stringstream out;
            medida::reporting::ConsoleReporter reporter(m, out);
            for (auto const& kv : m.GetAllMetrics())
            {
                auto& metric = kv.first;
                if (metric.domain() == "overlay")
                {
                    out << metric.domain() << "." << metric.type() << "."
                        << metric.name() << std::endl;
                    kv.second->Process(reporter);
                }
            }
            LOG_DEBUG(DEFAULT_LOG, " ~~~~~~ {} :\n{}", n->getConfig().PEER_PORT,
                      out.str());
        }
        REQUIRE(checkSim());
    };

    SECTION("transaction flooding")
    {
        TransactionTestFramePtr testTransaction = nullptr;
        auto injectTransaction = [&](int i) {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::pseudoRandomForTesting();

            // round robin
            auto inApp = nodes[i % nodes.size()];

            auto account = TestAccount{*inApp, sources[i]};
            auto tx1 = account.tx(
                {createAccount(dest.getPublicKey(), txAmount)}, expectedSeq);
            if (!testTransaction)
            {
                testTransaction = tx1;
            }
            // this is basically a modified version of Peer::recvTransaction
            auto msg = tx1->toStellarMessage();
            auto addResult = inApp->getHerder().recvTransaction(tx1, false);
            REQUIRE(addResult.code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            inApp->getOverlayManager().broadcastMessage(msg,
                                                        tx1->getFullHash());
        };

        auto ackedTransactions = [&](std::shared_ptr<Application> app) {
            // checks if an app received all transactions or not
            size_t okCount = 0;
            auto& herder = static_cast<HerderImpl&>(app->getHerder());

            for (auto const& s : sources)
            {
                auto accState =
                    herder.getTransactionQueue().getAccountTransactionQueueInfo(
                        s);
                auto seqNum = accState.mTransaction
                                  ? accState.mTransaction->mTx->getSeqNum()
                                  : 0;

                okCount += !!(seqNum == expectedSeq);
            }
            bool res = okCount == sources.size();
            LOG_DEBUG(DEFAULT_LOG, "{}{}{} / {} authenticated peers: {}",
                      app->getConfig().PEER_PORT, (res ? " OK " : " BEHIND "),
                      okCount, sources.size(),
                      app->getOverlayManager().getAuthenticatedPeersCount());
            return res;
        };

        auto cfgGen2 = [&](int n) {
            auto cfg = getTestConfig(n);
            // adjust delayed tx flooding and how often to pull
            cfg.FLOOD_TX_PERIOD_MS = 10;
            cfg.FLOOD_DEMAND_PERIOD_MS = std::chrono::milliseconds(10);
            return cfg;
        };
        SECTION("core")
        {
            SECTION("loopback")
            {
                simulation = Topologies::core(4, 1, Simulation::OVER_LOOPBACK,
                                              networkID, cfgGen2);
                test(injectTransaction, ackedTransactions, true);
            }
            SECTION("tcp")
            {
                simulation = Topologies::core(4, .666f, Simulation::OVER_TCP,
                                              networkID, cfgGen2);
                test(injectTransaction, ackedTransactions, true);
            }
            auto cfgGenPullMode = [&](int n) {
                auto cfg = getTestConfig(n);
                // adjust delayed tx flooding
                cfg.FLOOD_TX_PERIOD_MS = 10;
                // Using an unrealistically small tx set size
                // leads to an unrealistic batching scheme of adverts/demands
                // (i.e., no batching)
                // While there's no strict requirement for batching,
                // it seems more useful to test more realistic settings.
                cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 1000;
                return cfg;
            };
            SECTION("pull mode with 2 nodes")
            {
                // Limit the number of nodes to 2.
                // This makes the process of flooding crystal clear.
                int const numNodes = 2;

                simulation =
                    Topologies::core(numNodes, .666f, Simulation::OVER_LOOPBACK,
                                     networkID, cfgGenPullMode);
                auto advertCheck = [&](std::shared_ptr<Application> app) {
                    if (!ackedTransactions(app))
                    {
                        return false;
                    }

                    // Check pull-mode metrics
                    auto& om = app->getOverlayManager().getOverlayMetrics();
                    auto advertsSent = om.mSendFloodAdvertMeter.count();
                    auto advertsRecvd = om.mRecvFloodAdvertTimer.count();
                    auto demandsSent = om.mSendFloodDemandMeter.count();
                    auto hashesQueued =
                        overlaytestutils::getAdvertisedHashCount(app);
                    auto demandFulfilled =
                        overlaytestutils::getFulfilledDemandCount(app);
                    auto messagesUnfulfilled =
                        overlaytestutils::getUnfulfilledDemandCount(app);

                    LOG_DEBUG(
                        DEFAULT_LOG,
                        "Peer {}: sent {} adverts, queued {} codes, received "
                        "{} adverts, sent {} demands, fulfilled {} demands, "
                        "unfulfilled {} demands",
                        app->getConfig().PEER_PORT, advertsSent, hashesQueued,
                        advertsRecvd, demandsSent, demandFulfilled,
                        messagesUnfulfilled);

                    // There are only two peers.
                    // Each should send one or more advert(s) with exactly (nbTx
                    // / 2) hashes to each other, and demand & fulfill
                    // demands(s). The number of demands may depend on how many
                    // hashes are in one demand.

                    bool res = true;
                    res = res && advertsSent >= 1 && advertsRecvd >= 1;
                    res = res && hashesQueued == (nbTx / 2);
                    res = res && demandFulfilled >= 1 && demandsSent >= 1;
                    res = res && messagesUnfulfilled == 0;

                    return res;
                };
                test(injectTransaction, advertCheck, true);

                SECTION("advertise same transaction after some time")
                {
                    for (auto const& node : nodes)
                    {
                        auto before =
                            overlaytestutils::getAdvertisedHashCount(node);
                        node->getOverlayManager().broadcastMessage(
                            testTransaction->toStellarMessage(),
                            testTransaction->getFullHash());
                        REQUIRE(before ==
                                overlaytestutils::getAdvertisedHashCount(node));
                    }

                    // Now crank for some time and trigger cleanups
                    auto numLedgers =
                        nodes[0]->getConfig().MAX_SLOTS_TO_REMEMBER +
                        nodes[0]->getLedgerManager().getLastClosedLedgerNum();
                    simulation->crankUntil(
                        [&] {
                            return simulation->haveAllExternalized(numLedgers,
                                                                   1);
                        },
                        numLedgers * Herder::EXP_LEDGER_TIMESPAN_SECONDS,
                        false);

                    // Ensure old transaction gets re-broadcasted
                    for (auto const& node : nodes)
                    {
                        auto before =
                            overlaytestutils::getAdvertisedHashCount(node);
                        node->getOverlayManager().broadcastMessage(
                            testTransaction->toStellarMessage(),
                            testTransaction->getFullHash());

                        REQUIRE(before + 1 ==
                                overlaytestutils::getAdvertisedHashCount(node));
                    }
                }
            }
            SECTION("pull mode with 4 nodes")
            {
                int const numNodes = 4;

                simulation =
                    Topologies::core(numNodes, .666f, Simulation::OVER_TCP,
                                     networkID, cfgGenPullMode);
                auto advertCheck = [&](std::shared_ptr<Application> app) {
                    if (!ackedTransactions(app))
                    {
                        return false;
                    }

                    // Check pull-mode metrics
                    auto& om = app->getOverlayManager().getOverlayMetrics();
                    auto advertsSent = om.mSendFloodAdvertMeter.count();
                    auto advertsRecvd = om.mRecvFloodAdvertTimer.count();
                    auto demandsSent = om.mSendFloodDemandMeter.count();
                    auto hashesQueued =
                        overlaytestutils::getAdvertisedHashCount(app);
                    auto demandFulfilled =
                        overlaytestutils::getFulfilledDemandCount(app);
                    auto messagesUnfulfilled =
                        overlaytestutils::getUnfulfilledDemandCount(app);

                    LOG_DEBUG(
                        DEFAULT_LOG,
                        "Peer {}: sent {} adverts, queued {} codes, received "
                        "{} adverts, sent {} demands, fulfilled {} demands, "
                        "unfulfilled {} demands",
                        app->getConfig().PEER_PORT, advertsSent, hashesQueued,
                        advertsRecvd, demandsSent, demandFulfilled,
                        messagesUnfulfilled);

                    // There are four peers.
                    // Each node starts with 25 txns.
                    // For every node to get all the 100 txns,
                    // every node has to advertise, demand, and fulfill demands
                    // at least once.
                    // No node should do so more than 3 * nbTx times.

                    bool res = true;

                    res = res && 1 <= advertsSent && advertsSent <= 3 * nbTx;
                    res = res && 1 <= advertsRecvd && advertsRecvd <= 3 * nbTx;

                    // If this node queued < 25 hashes and/or responded to < 25
                    // demands, then no other node can obtain the 25 txns that
                    // this node has.
                    res = res && (nbTx / 4) <= hashesQueued;
                    res = res && hashesQueued <= 3 * nbTx;
                    res = res && (nbTx / 4) <= demandFulfilled;
                    res = res && demandFulfilled <= 3 * nbTx;

                    res = res && 1 <= demandsSent && demandsSent <= 3 * nbTx;

                    // Each advert must contain at least one hash.
                    res = res && advertsSent <= hashesQueued;

                    // # demands fullfulled
                    // <= # demands sent
                    // <= # hashes sent
                    // <= # hashes queued.
                    res = res && demandFulfilled <= hashesQueued;

                    res = res && messagesUnfulfilled == 0;

                    return res;
                };
                test(injectTransaction, advertCheck, true);
            }
        }

        SECTION("outer nodes")
        {
            SECTION("loopback")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen2);
                test(injectTransaction, ackedTransactions, true);
            }
            SECTION("tcp")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_TCP, networkID, cfgGen2);
                test(injectTransaction, ackedTransactions, true);
            }
        }
    }

    SECTION("scp messages flooding")
    {
        auto cfgGen = [](int cfgNum) {
            Config cfg = getTestConfig(cfgNum);
            // do not close ledgers
            cfg.MANUAL_CLOSE = true;
            cfg.FORCE_SCP = false;
            return cfg;
        };

        // SCP messages depend on
        // a quorum set
        // a valid transaction set

        std::vector<SecretKey> keys;
        UnorderedMap<PublicKey, SecretKey> keysMap;
        for (int i = 0; i < nbTx; i++)
        {
            keys.emplace_back(SecretKey::pseudoRandomForTesting());
            auto& k = keys.back();
            keysMap.insert(std::make_pair(k.getPublicKey(), k));
        }

        auto injectSCP = [&](int i) {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::pseudoRandomForTesting();

            // round robin
            auto inApp = nodes[i % nodes.size()];

            auto account = TestAccount{*inApp, sources[i]};
            auto tx1 = account.tx(
                {createAccount(dest.getPublicKey(), txAmount)}, expectedSeq);

            // create the transaction set containing this transaction

            auto txSet = makeTxSetFromTransactions({tx1}, *inApp, 0, 0).first;
            auto& herder = static_cast<HerderImpl&>(inApp->getHerder());

            // build the quorum set used by this message
            // use sources as validators
            SCPQuorumSet qset;
            qset.threshold = 1;
            qset.validators.emplace_back(sources[i]);

            Hash qSetHash = sha256(xdr::xdr_to_opaque(qset));
            auto const& lcl =
                inApp->getLedgerManager().getLastClosedLedgerHeader();
            // build an SCP message for the next ledger
            auto ct = std::max<uint64>(
                lcl.header.scpValue.closeTime + 1,
                VirtualClock::to_time_t(inApp->getClock().system_now()));
            StellarValue sv = herder.makeStellarValue(
                txSet->getContentsHash(), ct, emptyUpgradeSteps, keys[0]);

            SCPEnvelope envelope;

            auto& st = envelope.statement;
            st.slotIndex = lcl.header.ledgerSeq + 1;
            st.pledges.type(SCP_ST_PREPARE);
            auto& prep = st.pledges.prepare();
            prep.ballot.value = xdr::xdr_to_opaque(sv);
            prep.ballot.counter = 1;
            prep.quorumSetHash = qSetHash;

            st.nodeID = keys[i].getPublicKey();
            envelope.signature = keys[i].sign(xdr::xdr_to_opaque(
                inApp->getNetworkID(), ENVELOPE_TYPE_SCP, st));

            // inject the message
            REQUIRE(herder.recvSCPEnvelope(envelope, qset, txSet) ==
                    Herder::ENVELOPE_STATUS_READY);
        };

        auto ackedSCP = [&](std::shared_ptr<Application> app) {
            // checks if an app received and processed all SCP messages
            size_t okCount = 0;
            auto const& lcl =
                app->getLedgerManager().getLastClosedLedgerHeader();

            HerderImpl& herder = *static_cast<HerderImpl*>(&app->getHerder());
            herder.getSCP().processCurrentState(
                lcl.header.ledgerSeq + 1,
                [&](SCPEnvelope const& e) {
                    if (keysMap.find(e.statement.nodeID) != keysMap.end())
                    {
                        okCount++;
                    }
                    return true;
                },
                true);
            bool res = okCount == sources.size();
            LOG_DEBUG(DEFAULT_LOG, "{}{}{} / {} authenticated peers: {}",
                      app->getConfig().PEER_PORT, (res ? " OK " : " BEHIND "),
                      okCount, sources.size(),
                      app->getOverlayManager().getAuthenticatedPeersCount());
            return res;
        };

        auto quorumAdjuster = [&](SCPQuorumSet const& qSet) {
            auto resQSet = qSet;
            SCPQuorumSet sub;
            for (auto const& k : keys)
            {
                sub.validators.emplace_back(k.getPublicKey());
            }
            sub.threshold = static_cast<uint32>(sub.validators.size());
            resQSet.innerSets.emplace_back(sub);
            // threshold causes all nodes to be stuck on current ledger
            resQSet.threshold = static_cast<uint32>(resQSet.validators.size() +
                                                    resQSet.innerSets.size());
            return resQSet;
        };

        SECTION("core")
        {
            SECTION("loopback")
            {
                simulation =
                    Topologies::core(4, 1.0f, Simulation::OVER_LOOPBACK,
                                     networkID, cfgGen, quorumAdjuster);
                test(injectSCP, ackedSCP, false);
            }
            SECTION("tcp")
            {
                simulation =
                    Topologies::core(4, 1.0f, Simulation::OVER_TCP, networkID,
                                     cfgGen, quorumAdjuster);
                test(injectSCP, ackedSCP, false);
            }
        }

        SECTION("outer nodes")
        {
            SECTION("loopback")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen, 1,
                    quorumAdjuster);
                test(injectSCP, ackedSCP, false);
            }
            SECTION("tcp")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_TCP, networkID, cfgGen, 1,
                    quorumAdjuster);
                test(injectSCP, ackedSCP, false);
            }
        }
    }
}
}
