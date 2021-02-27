// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/PeerDoor.h"
#include "overlay/TCPPeer.h"
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

    auto cfgGen = [](int cfgNum) {
        Config cfg = getTestConfig(cfgNum);
        // do not close ledgers
        cfg.MANUAL_CLOSE = true;
        cfg.FORCE_SCP = false;
        return cfg;
    };

    const int nbTx = 100;

    std::vector<TestAccount> sources;
    SequenceNumber expectedSeq = 0;

    std::vector<std::shared_ptr<Application>> nodes;

    auto test = [&](std::function<void(int)> inject,
                    std::function<bool(std::shared_ptr<Application>)> acked) {
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
                    LedgerTxn ltx(n->getLedgerTxnRoot(), false);
                    ltx.create(gen);
                    ltx.commit();
                }
            }
        }

        expectedSeq = root.getLastSequenceNumber() + 1;

        // enough for connections to be made
        simulation->crankForAtLeast(std::chrono::seconds(1), false);

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
        simulation->crankUntil(checkSim, std::chrono::seconds(60), true);

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
        auto injectTransaction = [&](int i) {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::pseudoRandomForTesting();

            // round robin
            auto inApp = nodes[i % nodes.size()];

            auto account = TestAccount{*inApp, sources[i]};
            auto tx1 = account.tx(
                {createAccount(dest.getPublicKey(), txAmount)}, expectedSeq);

            // this is basically a modified version of Peer::recvTransaction
            auto msg = tx1->toStellarMessage();
            auto res = inApp->getHerder().recvTransaction(tx1);
            REQUIRE(res == TransactionQueue::AddResult::ADD_STATUS_PENDING);
            inApp->getOverlayManager().broadcastMessage(msg);
        };

        auto ackedTransactions = [&](std::shared_ptr<Application> app) {
            // checks if an app received all transactions or not
            size_t okCount = 0;
            for (auto const& s : sources)
            {
                okCount +=
                    (app->getHerder().getMaxSeqInPendingTxs(s) == expectedSeq)
                        ? 1
                        : 0;
            }
            bool res = okCount == sources.size();
            LOG_DEBUG(DEFAULT_LOG, "{}{}{} / {} authenticated peers: {}",
                      app->getConfig().PEER_PORT, (res ? " OK " : " BEHIND "),
                      okCount, sources.size(),
                      app->getOverlayManager().getAuthenticatedPeersCount());
            return res;
        };

        auto txFloodingTests = [&](bool delayed) {
            auto cfgGen2 = [&](int n) {
                auto cfg = cfgGen(n);
                // adjust delayed tx flooding
                cfg.FLOOD_TX_PERIOD_MS = delayed ? 10 : 0;
                return cfg;
            };
            SECTION("core")
            {
                SECTION("loopback")
                {
                    simulation =
                        Topologies::core(4, .666f, Simulation::OVER_LOOPBACK,
                                         networkID, cfgGen2);
                    test(injectTransaction, ackedTransactions);
                }
                SECTION("tcp")
                {
                    simulation = Topologies::core(
                        4, .666f, Simulation::OVER_TCP, networkID, cfgGen2);
                    test(injectTransaction, ackedTransactions);
                }
            }

            SECTION("outer nodes")
            {
                SECTION("loopback")
                {
                    simulation = Topologies::hierarchicalQuorumSimplified(
                        5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen2);
                    test(injectTransaction, ackedTransactions);
                }
                SECTION("tcp")
                {
                    simulation = Topologies::hierarchicalQuorumSimplified(
                        5, 10, Simulation::OVER_TCP, networkID, cfgGen2);
                    test(injectTransaction, ackedTransactions);
                }
            }
        };

        SECTION("direct tx broadcast")
        {
            txFloodingTests(false);
        }
        SECTION("delayed tx broadcast")
        {
            txFloodingTests(true);
        }
    }

    SECTION("scp messages flooding")
    {
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
            auto const& lcl =
                inApp->getLedgerManager().getLastClosedLedgerHeader();
            TxSetFrame txSet(lcl.hash);
            txSet.add(tx1);
            txSet.sortForHash();
            auto& herder = static_cast<HerderImpl&>(inApp->getHerder());

            // build the quorum set used by this message
            // use sources as validators
            SCPQuorumSet qset;
            qset.threshold = 1;
            qset.validators.emplace_back(sources[i]);

            Hash qSetHash = sha256(xdr::xdr_to_opaque(qset));

            // build an SCP message for the next ledger
            auto ct = std::max<uint64>(
                lcl.header.scpValue.closeTime + 1,
                VirtualClock::to_time_t(inApp->getClock().system_now()));
            StellarValue sv = herder.makeStellarValue(
                txSet.getContentsHash(), ct, emptyUpgradeSteps, keys[0]);

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
                test(injectSCP, ackedSCP);
            }
            SECTION("tcp")
            {
                simulation =
                    Topologies::core(4, 1.0f, Simulation::OVER_TCP, networkID,
                                     cfgGen, quorumAdjuster);
                test(injectSCP, ackedSCP);
            }
        }

        SECTION("outer nodes")
        {
            SECTION("loopback")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen, 1,
                    quorumAdjuster);
                test(injectSCP, ackedSCP);
            }
            SECTION("tcp")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_TCP, networkID, cfgGen, 1,
                    quorumAdjuster);
                test(injectSCP, ackedSCP);
            }
        }
    }
}
}
