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

namespace stellar
{
using namespace txtest;

TEST_CASE("Transaction flooding", "[flood][overlay]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    Simulation::pointer simulation;
    SecretKey root = getRoot(networkID);
    PublicKey rootPub = root.getPublicKey();

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
        // enough for connections to be made
        simulation->crankForAtLeast(std::chrono::seconds(1), false);

        std::shared_ptr<Application> app0 = simulation->getNodes()[0];

        SequenceNumber rootSeq = getAccountSeqNum(root, *app0) + 1;

        int64 amount = 10000000;

        auto acked = [&](std::shared_ptr<Application> app)
        {
            bool res = app->getHerder().getMaxSeqInPendingTxs(rootPub) == rootSeq;
            LOG(DEBUG) << app->getConfig().PEER_PORT << (res ? " OK" : " BEHIND");
            return res;
        };

        LOG(DEBUG) << "Injecting work";

        // inject a transaction
        {
            SecretKey dest = SecretKey::random();

            auto tx1 = createCreateAccountTx(networkID, root, dest, rootSeq, amount);

            // this is basically a modified version of Peer::recvTransaction
            auto msg = tx1->toStellarMessage();
            auto res = app0->getHerder().recvTransaction(tx1);
            REQUIRE(res == Herder::TX_STATUS_PENDING);
            app0->getOverlayManager().broadcastMessage(msg);
            REQUIRE(acked(app0));
        }

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
        // see if the tx got propagated properly
        simulation->crankUntil(
            checkSim,
            std::chrono::seconds(10), true);

        REQUIRE(checkSim());
    };

    SECTION("core")
    {
        simulation = Topologies::cycle4(networkID, cfgGen);
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
