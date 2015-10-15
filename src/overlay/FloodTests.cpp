// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Timer.h"
#include "TCPPeer.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/test.h"
#include "overlay/PeerDoor.h"
#include "main/Config.h"
#include "util/Logging.h"
#include "simulation/Simulation.h"
#include "overlay/OverlayManager.h"
#include "simulation/Topologies.h"
#include "transactions/TxTests.h"
#include "herder/Herder.h"
#include "ledger/LedgerDelta.h"

namespace stellar
{
using namespace txtest;

TEST_CASE("Transaction flooding", "[flood][overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation;

    // make closing very slow
    auto cfgGen = []()
    {
        static int cfgNum = 1;
        Config cfg = getTestConfig(cfgNum++);
        cfg.ARTIFICIALLY_SET_CLOSE_TIME_FOR_TESTING = 10000;
        return cfg;
    };

    auto test = [&]()
    {
        simulation->startAllNodes();

        auto nodes = simulation->getNodes();
        std::shared_ptr<Application> app0 = nodes[0];

        const int nbTx = 100;

        SecretKey root = getRoot(networkID);
        auto rootA =
            AccountFrame::loadAccount(root.getPublicKey(), app0->getDatabase());

        // directly create a bunch of accounts by cloning the root account (one
        // per tx so that we can easily identify them)
        std::vector<SecretKey> sources;
        std::vector<PublicKey> sourcesPub;
        {
            LedgerEntry gen(rootA->mEntry);
            auto& account = gen.data.account();
            for (int i = 0; i < nbTx; i++)
            {
                sources.emplace_back(SecretKey::random());
                sourcesPub.emplace_back(sources.back().getPublicKey());
                account.accountID = sourcesPub.back();
                auto newAccount = EntryFrame::FromXDR(gen);

                // need to create on all nodes
                for (auto n : nodes)
                {
                    LedgerHeader lh;
                    Database& db = n->getDatabase();
                    LedgerDelta delta(lh, db, false);
                    newAccount->storeAdd(delta, db);
                }
            }
        }

        SequenceNumber expectedSeq = getAccountSeqNum(root, *app0) + 1;

        // enough for connections to be made
        simulation->crankForAtLeast(std::chrono::seconds(1), false);

        int64 amount = 10000000;

        LOG(DEBUG) << "Injecting work";

        // inject transactions
        for (int i = 0; i < nbTx; i++)
        {
            SecretKey dest = SecretKey::random();

            auto tx1 = createCreateAccountTx(networkID, sources[i], dest,
                                             expectedSeq, amount);

            // round robin
            auto inApp = nodes[i % nodes.size()];

            // this is basically a modified version of Peer::recvTransaction
            auto msg = tx1->toStellarMessage();
            auto res = inApp->getHerder().recvTransaction(tx1);
            REQUIRE(res == Herder::TX_STATUS_PENDING);
            inApp->getOverlayManager().broadcastMessage(msg);
        }

        LOG(DEBUG) << "Done injecting work";

        auto acked = [&](std::shared_ptr<Application> app)
        {
            // checks if an app received all transactions or not
            size_t okCount = 0;
            for (auto const& s : sourcesPub)
            {
                okCount += (app->getHerder().getMaxSeqInPendingTxs(s) ==
                              expectedSeq) ? 1 : 0;
            }
            bool res = okCount == sourcesPub.size();
            LOG(DEBUG) << app->getConfig().PEER_PORT
                << (res ? " OK " : " BEHIND ") << okCount << " / " << sourcesPub.size()
                << " peers: " << app->getOverlayManager().getPeers().size();
            return res;
        };

        auto checkSim = [&]()
        {
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
            LOG(DEBUG) << " ~~~~~~ " << n->getConfig().PEER_PORT << " :\n" << out.str();
        }
        REQUIRE(checkSim());
    };

    SECTION("core")
    {
        Simulation::Mode mode = Simulation::OVER_LOOPBACK;
        SECTION("loopback")
        {
            mode = Simulation::OVER_LOOPBACK;
        }
        SECTION("tcp")
        {
            mode = Simulation::OVER_TCP;
        }
        simulation = Topologies::core(4, .666f, mode, networkID, cfgGen);
        test();
    }

    SECTION("outer nodes")
    {
        Simulation::Mode mode = Simulation::OVER_LOOPBACK;
        SECTION("loopback")
        {
            mode = Simulation::OVER_LOOPBACK;
        }
        SECTION("tcp")
        {
            mode = Simulation::OVER_TCP;
        }
        simulation = Topologies::hierarchicalQuorumSimplified(
            5, 10, mode, networkID, cfgGen);
        test();
    }
}
}
