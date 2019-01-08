// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TCPPeer.h"
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

TEST_CASE("Flooding", "[flood][overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation;

    // make closing very slow
    auto cfgGen = [](int cfgNum) {
        Config cfg = getTestConfig(cfgNum);
        cfg.ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = 10000;
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
                    TestAccount{*app0, SecretKey::random(), 0});
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

        LOG(DEBUG) << "Injecting work";

        // inject transactions
        for (int i = 0; i < nbTx; i++)
        {
            inject(i);
        }

        LOG(DEBUG) << "Done injecting work";

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
            LOG(DEBUG) << " ~~~~~~ " << n->getConfig().PEER_PORT << " :\n"
                       << out.str();
        }
        REQUIRE(checkSim());
    };

    SECTION("transaction flooding")
    {
        auto injectTransaction = [&](int i) {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::random();

            // round robin
            auto inApp = nodes[i % nodes.size()];

            auto account = TestAccount{*inApp, sources[i]};
            auto tx1 = account.tx(
                {createAccount(dest.getPublicKey(), txAmount)}, expectedSeq);

            // this is basically a modified version of Peer::recvTransaction
            auto msg = tx1->toStellarMessage();
            auto res = inApp->getHerder().recvTransaction(tx1);
            REQUIRE(res == Herder::TX_STATUS_PENDING);
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
            LOG(DEBUG) << app->getConfig().PEER_PORT
                       << (res ? " OK " : " BEHIND ") << okCount << " / "
                       << sources.size() << " authenticated peers: "
                       << app->getOverlayManager().getAuthenticatedPeersCount();
            return res;
        };

        SECTION("core")
        {
            SECTION("loopback")
            {
                simulation = Topologies::core(
                    4, .666f, Simulation::OVER_LOOPBACK, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
            SECTION("tcp")
            {
                simulation = Topologies::core(4, .666f, Simulation::OVER_TCP,
                                              networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
        }

        SECTION("outer nodes")
        {
            SECTION("loopback")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_LOOPBACK, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
            SECTION("tcp")
            {
                simulation = Topologies::hierarchicalQuorumSimplified(
                    5, 10, Simulation::OVER_TCP, networkID, cfgGen);
                test(injectTransaction, ackedTransactions);
            }
        }
    }

    SECTION("scp messages flooding")
    {
        // SCP messages depend on
        // a quorum set
        // a valid transaction set

        std::vector<SecretKey> keys;
        for (int i = 0; i < nbTx; i++)
        {
            keys.emplace_back(SecretKey::random());
        }

        auto injectSCP = [&](int i) {
            const int64 txAmount = 10000000;

            SecretKey dest = SecretKey::random();

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
            auto& herder = inApp->getHerder();

            // build the quorum set used by this message
            // use sources as validators
            SCPQuorumSet qset;
            qset.threshold = 1;
            qset.validators.emplace_back(sources[i]);

            Hash qSetHash = sha256(xdr::xdr_to_opaque(qset));

            // build an SCP nomination message for the next ledger

            StellarValue sv(txSet.getContentsHash(),
                            lcl.header.scpValue.closeTime + 1,
                            emptyUpgradeSteps, 0);

            SCPEnvelope envelope;

            auto& st = envelope.statement;
            st.slotIndex = lcl.header.ledgerSeq + 1;
            st.pledges.type(SCP_ST_PREPARE);
            auto& prep = st.pledges.prepare();
            prep.ballot.value = xdr::xdr_to_opaque(sv);
            prep.ballot.counter = 1;
            prep.quorumSetHash = qSetHash;

            // use the sources to sign the message
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
            auto state =
                herder.getSCP().getCurrentState(lcl.header.ledgerSeq + 1);
            for (auto const& s : keys)
            {
                if (std::find_if(
                        state.begin(), state.end(), [&](SCPEnvelope const& e) {
                            return e.statement.nodeID == s.getPublicKey();
                        }) != state.end())
                {
                    okCount += 1;
                }
            }
            bool res = okCount == sources.size();
            LOG(DEBUG) << app->getConfig().PEER_PORT
                       << (res ? " OK " : " BEHIND ") << okCount << " / "
                       << sources.size() << " authenticated peers: "
                       << app->getOverlayManager().getAuthenticatedPeersCount();
            return res;
        };

        auto quorumAdjuster = [&](SCPQuorumSet const& qSet) {
            auto resQSet = qSet;
            SCPQuorumSet sub;
            for (auto const& k : keys)
            {
                sub.threshold++;
                sub.validators.emplace_back(k.getPublicKey());
            }
            resQSet.threshold++;
            resQSet.innerSets.emplace_back(sub);
            return resQSet;
        };

        SECTION("core")
        {
            SECTION("loopback")
            {
                simulation =
                    Topologies::core(4, .666f, Simulation::OVER_LOOPBACK,
                                     networkID, cfgGen, quorumAdjuster);
                test(injectSCP, ackedSCP);
            }
            SECTION("tcp")
            {
                simulation =
                    Topologies::core(4, .666f, Simulation::OVER_TCP, networkID,
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
